# MovieCart — STM32F407VGT6 (DevEBox)

Firmware for the **MCUDEV DevEBox STM32F407VGT6** that emulates a MovieCart cartridge while streaming field files from the onboard microSD. Pin map and SDIO wiring match the [UnoCart-2600 DevEBox fork](../../../UnoCart-2600).

The original dsPIC33CK firmware lives in `firmware/` (parent of this directory).

## Status

Working on real hardware (2600 + DevEBox):

- Title screen, SD mount, file select and playback of `.MVC` field files.
- Transport controls (see [Controls](#controls)): play/pause, fast-forward and rewind while playing, frame stepping while paused, volume, brightness, Reset (back 10 s), Select (next file).

Known issues:

- In-field **FIRMWARE.FRM** flash update is not ported from the dsPIC.

## Hardware

| Atari | MCU |
|-------|-----|
| A0–A12 | PE0–PE12 |
| D0–D7 | PD8–PD15 |
| +5V / GND | Board 5V / GND (MCU is 3.3 V; level-shift as on your breakout) |

Onboard microSD (SDIO 4-bit):

| SD | MCU |
|----|-----|
| D0–D3 | PC8–PC11 |
| CK | PC12 |
| CMD | PD2 |

Status LED: **PA1** (optional, bit-banged).

Power: DevEBox is powered from the Atari.

System clock: HSI → PLL @ 168 MHz (same as UnoCart DevEBox); SDIOCLK 48 MHz.

## Architecture overview

```
┌─────────────────────────────────────────────────────────────────┐
│ 6502 / 6507                                                     │
│   Visible scanlines  ← looping kernel in core.c (g0x3e–g0xb6)   │
│   First 6 overscan   ← cart $F164 (samples + pack copy)         │
│   Rest of OS / VS / VB ← RamKernel in RIOT $80 (nibble play)    │
└───────────────────────────┬─────────────────────────────────────┘
                            │ A12-high cart fetches (~838 ns/cycle)
┌───────────────────────────▼─────────────────────────────────────┐
│ STM32 main thread (IRQs masked while serving)                     │
│   emulate_cartridge() / RamKernel waits   ← UnoCart poll loop  │
│   bus_dispatch()                           ← original-style jump table │
│                                                                 │
│ All SD (mount, open, probe, select, playback field loads):      │
│   DMA + manual completion poll in fatfs_sd_sdio.c               │
│   every wait yields → serve to the next RamKernel entry         │
└───────────────────────────┬─────────────────────────────────────┘
                            │ SDIO DMA → SRAM1 (buf1) / SRAM2 (buf2)
┌───────────────────────────▼─────────────────────────────────────┐
│ microSD (FAT32, MovieCart field files)                          │
└─────────────────────────────────────────────────────────────────┘
```

### RamKernel split (`src/core.c`)

Visible serving is the original MovieCart computed-goto kernel (one right/left pair, `jmp` back until `lines == 0`). Blanking is RamKernel copied to RIOT `$80` at ColdStart (`mc_ram_kernel[]` at `$F200`). The firmware build does not assemble or embed `core.bin`.

| Region | Where it runs | Role |
|--------|---------------|------|
| **ColdStart** | Cart ROM `$F000` | Copies **RamKernel** from `$F200` into RIOT `$80`, then enters it |
| **RamKernel** | RIOT `$80`–`$D1` | Visible `jsr`, remaining OS/VS/VB nibble play, HMOVE pad (A12 low) |
| **Pack** | RIOT `$D8`–`$F7` | 32 bytes / 64 nibbles (blanking tail after 6 cart OS samples) |
| **VisibleBars** | Cart ROM | Looping dual-line kernel (`g0x3e`…`g0xb6`); `AUDV0` every scanline |

There is **no preroll**. The first 6 overscan lines run from cart (`$F164`): play 6 tail samples and copy the 32-byte pack. Remaining overscan + VSYNC + VBLANK run from RIOT so SDIO finish/begin stay A12-low.

**Interlace.** The line counts follow the parity of the field just shown, as in the PIC firmware: after an even field, 30 overscan (6 cart + 24 RIOT) + 3 VSYNC + 38 blank (37 VBLANK + the pad line) = **263** lines; after an odd field, 29 + 3 + 37 (36 + pad) = **261**. The picture therefore starts one line later after an even field than after an odd one, and with alternating fields every frame is **262** lines from VSYNC to VSYNC. OsHead loads the parity into RIOT `$FB` on its 6th line (`lda #odd / sta $FB`, the immediate served from `frameInfo.odd`), because the cart cannot write RIOT. The pad line is the line after VBLANK where the visible kernel only starts at cycle 71 and draws nothing. **NTSC is the target.** PAL still uses the same RamKernel (`visibleLines` still comes from the field header). Blanking audio is documented under **RIOT nibble audio** below.

Cart dispatch indexes **`addr & 0xfff`** (12-bit space). Key hook addresses:

| Offset | Symbol | MCU action |
|--------|--------|------------|
| `$F09D` | VisibleBars entry (nop) | Serve the byte only. The next visible kernel was set up at the RamKernel entry (`mc_on_ram_kernel_entry()`: picture and audio pointers, line count) |
| `$F09E`–`$F116` | right/left pair | Patch graphics/color/audio; `jmp $F09E` or `$F164` |
| `$F140`–`$F15F` | CartPack | 32 packed bytes (tail after 6 cart samples) |
| `$F16C` / `$F199` | overscan `lda #` | Next tail sample immediate (6 fetches) |
| `$F164`–`$F1C5` | overscan head | 5×6 + 2 copy, 6× `AUDV0`, joystick, parity, `WSYNC`, RTS |
| `$F1BF` / `$F1C0` | overscan `lda #` | Field parity immediate (`frameInfo.odd`) for the RamKernel's line counts |
| `$F200`+ | RamKernel image | Copied to RIOT `$80` at ColdStart |
| `$FE00+x` | gstore | Latch SWCHA / SWCHB / INPT4 / INPT5 |

### RIOT nibble audio

Field files store **`totalLines`** volume bytes in play order: **[visible lines][blanking tail]**. The looping kernel writes `AUDV0` on every visible scanline (192 NTSC). The tail is typically 70 samples (overscan + VSYNC + VBLANK). There is no preroll. A frame plays 192 + 6 + 64 = 262 samples after an even field and 192 + 6 + 62 = 260 after an odd one; the pad line holds the last VBLANK sample.

The last 64 tail samples cannot be cart immediates during RamKernel (A12 low, SDIO). The first **6** overscan lines are still cart: play those 6 samples and copy the remaining **32 packed bytes** into RIOT. RamKernel then plays the rest.

#### Pipeline

1. **Swap** — pack `audio[visibleLines+6 … totalLines)` (capped at 64 samples) into `mc_aud_pack[32]`. Playback swap is in `finishFieldRead`; title still swaps at `$F09D`.
2. **Visible** — 192 movie lines, unchanged.
3. **Overscan head** (`$F164`, 6 lines) — `lda #` / `sta AUDV0` for tail[0..5]; copy 5×6 + 2 bytes from `$F140` → `$D8`. Joystick, field parity and `WSYNC` on line 6, then `RTS` (blanking window starts).
4. **RamKernel OS/VS/VB** — unpack one nibble per line. After an even field 24 + 3 + 37 = 64 play lines, after an odd one 23 + 3 + 36 = 62. A12 low. SDIO window.
5. Next swap fills the pack for the following field. The 6-line copy at the end of that field’s picture loads it.

Pack is taken from the display buffer **base** at swap (`+ visible + 6`), not from the live audio cursor. The 6 head samples are consumed from the cursor during `$F164`; each advances the cursor on its operand fetch, because the loop's taken `bne` makes a dummy read of `$F199` that must not consume a sample. The RAM kernel entry resets the cursor for the next frame.

#### Who does what

| Side | When | Action |
|------|------|--------|
| STM32 | swap | Pack two samples per byte into `mc_aud_pack[32]`; serve `$F140` |
| STM32 | `$F16C` / `$F199` | Next tail sample as `lda #` immediate (6 fetches) |
| 6502 overscan head | After 192 picture lines | Play 6 samples; `lda $F140,x` / `sta $D8,x`; `lda #odd` / `sta $FB` |
| 6502 RamKernel | A12 low | `AUDIDX` even → low nibble; odd → `lsr ×4`; both `and #$0F`; `sta AUDV0` |

#### RIOT map (128 bytes, `$80`–`$FF`)

| RIOT | Size | Role |
|------|-----:|------|
| `$80`–`$AB` | 44 | Frame: `jsr $F09D`, overscan 24/23, VS 3, VB 37/36 (by parity), `stx VBLANK` / `AUDIDX` |
| `$AC`–`$B8` | 13 | HMOVE pad (`ldx #7` / `nop` / `bit $00` ×2 / `jmp $80`) |
| `$B9`–`$D1` | 25 | `Play` (`lda $D8,y`, nibble unpack, `sta AUDV0` / `WSYNC`) |
| `$D2`–`$D7` | 6 | unused |
| `$D8`–`$F7` | 32 | Packed tail after the 6 cart OS samples |
| `$F8`–`$FA` | 3 | unused |
| `$FB` | 1 | Field parity of the field just shown (written by OsHead) |
| `$FC` | 1 | `AUDIDX` |
| `$FD` | 1 | unused |
| `$FE`–`$FF` | 2 | 6502 stack for `jsr VisibleBars` / `jsr Play` |

`$80`–`$D1` is 82 bytes (copied from `$F200`). `$80`–`$FF` is 128.

#### RamKernel listing

Firmware image `mc_ram_kernel[]` at `$F200`, copied to `$80`. Not assembled from `core.asm`.

```asm
; rorg $80
RamKernel
        jsr     $F09D           ; VisibleBars + OsHead (cart); OsHead sets $FB
        lda     $FB             ; parity of the field just shown (1 = odd)
        lsr
        ldx     #24             ; even: 24 more overscan lines
        bcc     .os
        dex                     ; odd: 23
.os     jsr     Play            ; $B9
        lda     #2
        sta     $00             ; VSYNC
        ldx     #3
        jsr     Play
        stx     $00             ; VSYNC off (X=0)
        lda     #2
        sta     $01             ; VBLANK
        lda     $FB
        lsr
        ldx     #37             ; even: 37 (+ pad line = 38 blank)
        bcc     .vb
        dex                     ; odd: 36 (+ pad = 37)
.vb     jsr     Play
        stx     $01             ; VBLANK off
        stx     $FC             ; AUDIDX = 0
        ldx     #7              ; HMOVE pad, $AC
.pad    dex
        bne     .pad
        nop
        bit     $00
        bit     $00
        jmp     $80

Play                            ; $B9
        lda     $FC             ; AUDIDX
        lsr
        tay
        lda     $00D8,y         ; PACK
        bcc     .lo
        lsr
        lsr
        lsr
        lsr
.lo     and     #$0F
        sta     $19             ; AUDV0
        inc     $FC
        sta     $02             ; WSYNC
        dex
        bne     Play
        rts
```

#### Edges

- **First field** — pack is zeros until the first swap; first OS/VS/VB after the 6 cart lines is silent if the pack was empty.
- **Even vs odd** — after an even field 6 cart + 64 RIOT = 70 tail samples play; after an odd one 6 + 62 = 68, dropping the last two.
- **PAL / long tails** — NTSC is the target. PAL still uses the same RamKernel line counts. Pack is fixed at 64 samples / 32 bytes after the 6 head lines.
- **Title** — swap packs `audio + visible + 6` in the title buffer; the 6 head lines play title audio at `visible`.

### Cart bus serving

Serving follows UnoCart-2600's `driver_4k.c` polling loop:

1. Wait for a stable address on PE0–PE12.
2. If A12 high: `bus_dispatch()`, write ODR, enable outputs, wait for address change, tri-state.
3. Repeat forever with **interrupts disabled** on the main thread.

Critical rules (measured on this hardware):

- **No work between address sample and ODR write** — even ~60 ns settle windows black the display.
- **No repeat-address suppression** — the kernel legitimately refetches the same cart address.
- **`SET_DATA()` enables drivers** immediately after writing ODR; dispatch side effects run before returning.
- **Bus code runs from flash** (`.hottext`, ART-line aligned), not SRAM. The `romData` jump table lives in CCM so lookups are D-bus only.
- Per-cycle budget is ~**100 ns**; one missed ~838 ns cycle can desync the scanline counter.

`moviecart_bus_yield()` marks a step boundary. With `mc_sdio_gate_relaxed` set it does nothing: the caller's whole list of steps is assumed to finish inside the current RamKernel window. Otherwise it serves the cart until the next `inramkernel` false → true edge (and swaps the title field), so the next step starts at the beginning of a fresh window.

### SD I/O (no IRQs, no WaitCart)

The stock UnoCart SDIO driver (`fatfs_sd_sdio.c`) keeps **DMA** for sector reads. Completion flags (`TransferEnd`, `DMAEndOfTransfer`) are set by calling `SD_ProcessIRQSrc()` and `SD_ProcessDMAIRQ()` from the wait loops — not from interrupt handlers. **IRQs stay masked** for the entire Atari serve run.

Every SDIO wait loop calls `moviecart_bus_yield()` (or `SD_PollDmaAndYield()` for DMA drains). Long delays use `moviecart_delay_ms()` instead of busy DWT spins.

**Blanking-only SDIO control:** every bus loop tracks `inramkernel`, which is true from the RamKernel's first opcode fetch (`$0080` after ColdStart, `$0083` after OsHead's RTS) until its last cycle (`$0082`, the `jsr VisibleBars` high byte). `moviecart_wait_ram_kernel_entry()` serves the cart until that flag goes false → true. The SDIO driver calls `moviecart_bus_yield()` before each command and data setup; outside relaxed mode that waits for the next entry (mount, title probe, file select). The frame waits (`emulate_cartridge`, `moviecart_wait_ram_kernel_entry`, `bus_wait_endframe`) run the title/NO_SD field swap right after the entry. Playback command initiation and completion polling run relaxed (yield is a no-op) in their respective blanking intervals. The DMA itself runs asynchronously between them while the other field buffer is displayed.

### Playback loop

After the embedded title screen and SD mount:

1. **`runTitle()`** — title hold; geometry probe reads one field sector while yielding.
2. **`runFrameLoop()`** — two-buffer DMA pipeline (see below).

Field files: FAT32 root, first playable file by default (8.3 extension `.MVC`, at least one field long, name not starting with `.` or `_`, so macOS `.DS_Store` and `._*` files are skipped); `MVC\0` header, 1–6 sectors (≤ 3 KiB field buffer). Controls match stock MovieCart (Select advances files, etc.). A file change (Select or end of file) first silences the shown field and the blanking pack (`mc_mute_display()`), so the directory walk and the 30-frame hold are quiet instead of a 60 Hz buzz.

The directory walk and other CPU-bound loops call `moviecart_bus_yield()` between steps. Playback reads no FAT: when a movie is opened, `pf_map_file()` reads the FAT six sectors per frame (one async CMD18, like a field) and records the file's clusters as runs (up to 8,192), so `pf_seek_block()` is a binary search plus arithmetic. During playback field loads (relaxed) those calls do nothing; elsewhere each one waits for the next RamKernel entry.

### Controls

Joystick input is read once per frame in OsHead and handled by `updateTransport()` (`src/update.c`). Up/down cycles three modes: **time → volume → brightness → time**. (The PIC firmware had a fourth, dead mode that showed the volume label but ignored left/right; `MODE_LAST` is now `MODE_BRIGHT`.)

| Input | Effect |
|-------|--------|
| Fire | Toggle play / pause |
| Up / down | Previous / next mode; shows that mode's display for ~3 s |
| Left / right, time mode, playing | Rewind / fast-forward while held. Normal speed for the first ~¼ s, then the step grows by 4 fields every 16 frames, up to 125 |
| Left / right, time mode, paused | Step one field pair per push; holding repeats after 24 frames, then every 8 (no acceleration) |
| Left / right, volume / brightness | One level per push; holding repeats after 24 frames, then every 8 |
| Reset switch | Jump back 10 s |
| Select switch | Next file |

The PIC firmware only changed volume/brightness when a 16-frame counter wrapped, so quick taps were ignored. Volume/brightness and paused stepping now use their own counter (`levelRepeat`); fast-forward/rewind keeps the original `joyRepeat` ramp.

### Double buffering

`mr_buffer1` (SRAM1) and `mr_buffer2` (SRAM2) are a ping-pong pair. One field is displayed while SDIO DMA fills the other. FAT, directory and boot sectors (and the title probe's header) go through an 8-slot LRU sector cache in SRAM1 (`disk_read_block1()`); field loads never use it.

| Interval | Display | DMA target | Main-thread work |
|----------|---------|------------|------------------|
| Visible N | A | B (may still be filling) | Serve cart only |
| Blanking N (`$F1C5`) | — | B, then A | Finish/validate B, swap to B, start CMD18 into A |
| Visible N+1 | B | A | Serve cart; `$F09D` does **not** swap |
| Blanking N+1 | — | A, then B | Finish/validate A, swap to A, start CMD18 into B |

`$F09D` still swaps during **title** (`mc_playback_pipeline == 0`). Once `runFrameLoop()` sets that flag, the swap happens in blanking inside `finishFieldRead()` after the field is validated — VisibleBars must not see a half-filled buffer.

The first playback pass repeats the title once while the first CMD18 is primed. A failed field is retried into the same inactive buffer and is not swapped in.

`beginFieldRead()` / `finishFieldRead()` each set `mc_sdio_gate_relaxed` so command start and completion polls use the current blanking window instead of waiting for a new `$F1C5` edge. The DMA itself runs through the following visible interval.

#### DMA completion (frame data)

IRQs stay masked. After `waitEndFrame()`, if `field_dma_pending`:

`finishFieldRead()` → `pf_read_blocks_finish()` → `disk_read_blocks_finish()` → `TM_FATFS_SD_SDIO_disk_read_finish()` → `SD_WaitReadOperation()`

Before the finish, `disk_read_blocks_finish()` waits a bounded time for the read to end (`TM_FATFS_SD_SDIO_disk_read_done()`, which polls the same two helpers without yielding). It polls for up to 1 ms per RamKernel window; a read still going then waits for the next window (the shown field repeats, the DMA carries on), up to 20 windows. A read that never ends is aborted (`TM_FATFS_SD_SDIO_disk_read_abort()`, CMD12 if the card is still sending) and counts as a failed attempt. The card's own data timeout is `SD_READ_DATATIMEOUT`, 250 ms at the transfer clock (the SD spec caps SDHC read access at 100 ms), so a stuck card normally ends the read with `SD_DATA_TIMEOUT` before the 20-window backstop. A failed finish is also aborted, so the retry starts from a clean data path.

`SD_WaitReadOperation()` then loops until **either** `TransferEnd` **or** `DMAEndOfTransfer` (or error / timeout), which has already happened:

```
while (!DMAEndOfTransfer && !TransferEnd && TransferError == SD_OK)
    SD_PollDmaAndYield();
```

Each poll:

1. `moviecart_bus_yield()` — no-op (relaxed).
2. `SD_ProcessIRQSrc()` — if `SDIO_IT_DATAEND`, set `TransferEnd` (also records CRC / timeout / overrun).
3. `SD_ProcessDMAIRQ()` — if DMA2 Stream3 TCIF, set `DMAEndOfTransfer` and clear the flag.
4. `moviecart_bus_yield()` — no-op (relaxed).

Those two `SD_Process*` helpers are the stock “IRQ” handlers; they are called from the wait loop, not from NVIC.

After the flags trip, the finish path also:

- waits until `SDIO->STA` is not `RXACT`
- sends CMD12 (`SD_StopTransfer`) because the field was a multi-block CMD18
- polls `SD_GetStatus()` (CMD13) until the card is not `SD_TRANSFER_BUSY`

Then `finishFieldRead()` checks `MVC\0` and geometry against the title-probe cache (`playback_field_blocks`). Only a valid field calls `updateBuffer()` and `mc_field_swap_to_display()`.

### RamKernel timing budget

RamKernel is only the A12-low nibble-play tail: `$F1C5` RTS through VBLANK off. That is when `finishFieldRead` / `beginFieldRead` run. The first 6 overscan lines are cart (`$F164`: 6 `AUDV0` + 32-byte pack copy) and finish **before** the blanking window opens — they are not in this budget. After VBLANK, an in-RIOT pad restores HMOVE phase (still A12 low, after the window).

Those 6 lines used to be RIOT overscan. They are now cart, so the SDIO-quiet window is **24 + 3 + 37 = 64** lines (**4.08 ms**) after an even field and **23 + 3 + 36 = 62** (**3.95 ms**) after an odd one.

Line math is NTSC (76 CPU cycles at 1.193182 MHz = 63.695 µs). SD peak is 8 MHz × 4-bit = 4 MB/s (`DMA_CLKDIV=0x04`). Figures are calculated from the source, not scoped on the board.

| | Even | Odd |
|--|-----:|----:|
| **RamKernel (remaining OS+VS+VB)** | **4.08 ms** (64 lines) | **3.95 ms** (62 lines) |
| Typical playback work | **0.2–0.45 ms** (5–11%) | same |
| Worst bounded | **~1.1 ms** (32 CMD13 polls + OSD + restarted CMD18) | 2.8 ms slack in 3.95 ms |

Playback work must finish before VBLANK goes off. Mount, title probe, and file-select are sliced across many frames by the strict SDIO gate and do not have to finish in one RamKernel.

#### Window breakdown

NTSC frame: **263** lines after an even field, **261** after an odd one, 262 from VSYNC to VSYNC when fields alternate. Only the 64 / 62 Play lines are the SDIO-quiet RamKernel window. VSYNC/VBLANK register writes sit in the first cycles of a Play line (after the previous `WSYNC`); they are not extra scanlines. The HMOVE pad after VBLANK off (`ldx #7` / `nop` / `bit $00` / `jmp $80`, then `jsr $F09D`) fills the next line up to cycle 71, where the visible kernel starts, so it costs one blank line; it is after the window.

| Segment | Even (lines) | Odd (lines) | Even (ms) | Odd (ms) | Budget? |
|---------|-------------:|------------:|----------:|---------:|---------|
| Picture (cart) | 192 | 192 | 12.23 | 12.23 | No — A12 high |
| Overscan head (cart `$F164`) | 6 | 6 | 0.38 | 0.38 | No — A12 high; 6 samples + 32-byte copy |
| Remaining overscan (RIOT) | 24 | 23 | 1.53 | 1.47 | Yes |
| VSYNC (3 Play lines) | 3 | 3 | 0.19 | 0.19 | Yes |
| VBLANK (Play lines) | 37 | 36 | 2.36 | 2.29 | Yes |
| **RamKernel (Play)** | **64** | **62** | **4.08** | **3.95** | SDIO-quiet window |
| HMOVE pad line (RIOT `$AC`) | 1 | 1 | 0.06 | 0.06 | No — after VBLANK off; A12 low |
| **Frame** | **263** | **261** | **16.75** | **16.62** | — |

**NTSC is the target.** PAL still uses the same RamKernel line counts; `visibleLines` comes from the field header.

#### Case-by-case

| Case | Where | Gate | Typical | Worst | One window? |
|------|-------|------|---------|-------|-------------|
| 1. Nominal playback | Every field after `waitEndFrame` | Relaxed | 0.35–0.80 ms | 1.2 ms | Yes — this is the budget case |
| 3. Playback + OSD | `updateColor` level bars / timecode | Relaxed | +40–80 µs | +0.10 ms | Yes; yields every 4–8 bytes |
| 4. DMA leftover at RTS | `finishFieldRead` poll before DATAEND | Relaxed | 0 (DMA done in visible) | 0.8–1.5 ms | Yes; should not happen if CMD18 started last blanking |
| 5. Field retry | finish fails, `beginFieldRead` again | Relaxed | +0.4 ms | +1.0 ms | Yes; then waits for `$F09D` and skips swap |
| 6. Title probe | `runTitle` `pf_seek` + 1-sector read | Strict | 1–3 frames | Several frames | No — one SDIO touch per edge |
| 7. File select | `checkSelectVideo` open + probe + wipe | Strict, then relaxed wipe | Many frames | Directory walk + 30-frame hold | No |
| 8. Mount / `SD_Init` | `setupDisk` before playback | Strict + `delay_ms` | Hundreds of ms to seconds | 5× (`SD_Init` + 50 ms) | No — title is already looping |
| 9. RTS / entry hooks | `$F1C5` and `$F09D` dispatch | On the cart cycle | <2 µs | <5 µs | Cycle budget (~100 ns), not RamKernel |

#### Playback blanking — step timings

`runFrameLoop` after `waitEndFrame`: `finishFieldRead` → `checkSelectVideo` (usually a compare) → `updateTransport` → `beginFieldRead`. That stack must finish in the 3.95–4.08 ms RamKernel window (before VBLANK off). The 32-byte pack copy already happened in the 6 cart overscan-head lines.

| Step | What | Typical | Worst | Notes |
|------|------|---------|-------|-------|
| finish: wait DMA | `SD_WaitReadOperation` flags | 0 µs | 800–1500 µs | 5–6×512 B at 2–4 MB/s if still in flight |
| finish: RXACT + CMD12 | `StopTransfer` after CMD18 | 80–150 µs | 250 µs | Always; field is multi-block |
| finish: CMD13 busy | `SD_GetStatus` until not `TRANSFER_BUSY` | 50–200 µs | 500 µs | Card-dependent |
| finish: validate | `memcmp MVC\0` + `frameInit` | <10 µs | 20 µs | Pointer math only |
| `updateVolume` | `totalLines` table remap, yield /4 | 45 µs | 60 µs | 262 NTSC / ~312 PAL stores |
| `updateColor` | bright/B&W on 5×visible + BK | 250–300 µs | 350 µs | OSD overlays add ~50–80 µs |
| `mc_field_swap_to_display` | Pointers + pack 64 tail samples | <15 µs | 25 µs | 32-byte `mc_aud_pack`; no pixel copy |
| `updateTransport` | Joystick → `frameNumber` step | <10 µs | 20 µs | No disk |
| `pf_seek_block` | Binary search of the extent map, no FAT access | <5 µs | 5 µs | Map built once by `pf_map_file()` at open |
| CMD18 arm | DMA setup + CMD16? + CMD18 | 100–200 µs | 250 µs | Payload runs in the next visible |

Stacked totals in one **RamKernel** (168 MHz CPU, 8 MHz 4-bit SDIO). Shares are of 4.01 ms even-field.

| Stack | Sum | Share of 4.01 ms |
|-------|-----|------------------|
| Sequential + cache hit | finish 0.20 + volume 0.05 + color 0.27 + seek 0.03 + CMD18 0.15 + pack 0.01 = **0.71 ms** | 18% |
| FAT miss + OSD + slow busy | finish 0.50 + volume 0.06 + color 0.35 + FAT 0.80 + CMD18 0.20 = **1.91 ms** | 48% |
| Plus leftover DMA | add 1.5 ms for a full PAL field still on the wire = **3.41 ms** | 85% |

#### Work that is not a single-window max

Outside relaxed mode, `moviecart_bus_yield()` waits for a fresh RamKernel entry on every call. Each `SendCommand` / `DataConfig` / poll therefore consumes at most one RamKernel, then sleeps until the next entry. Per-window STM32 time is one command or one status poll.

| Path | Per-window slice | How it spans frames |
|------|------------------|---------------------|
| `SD_Init` / mount (400 kHz, then 8 MHz) | One CMD + response, or a 2 ms / 50 ms `moviecart_delay_ms()` | Init clock 48 MHz/(0x76+2) ≈ 400 kHz. `delay_ms` checks its deadline once per RamKernel entry. |
| Title probe (1 sector) | CMD17 data 128 µs + commands ~200–400 µs if it all landed in one window; usually split | Each `gate()` inside `WaitReadOperation` waits for the next edge. |
| `pf_open_file` / directory walk | One 512 B directory or FAT sector | Repeats until the first playable file is found. |
| `clearFieldBuffers` after select | ~100–200 µs (6 KiB memset, yield /64) | Then 30× `waitEndFrame` hold. Not SDIO. |
| `pf_map_file` at open | One async CMD18 of 6 FAT sectors per frame, walked into runs | ~1 frame per 768 clusters: ~0.6 s for an hour of video on 32 KB clusters, ~5 s on 4 KB clusters. Too many runs (>8,192) or a broken chain → blink 11. |

#### What is not charged to RamKernel

| Work | When it runs | Duration |
|------|--------------|----------|
| Field CMD18 payload (5–6 × 512 B) | Visible interval after `beginFieldRead` | 0.64 ms peak (4 MB/s, 5 sectors) · ~0.77 ms PAL 6 sectors · up to ~1.5 ms on a 2 MB/s card |
| Looping visible kernel dispatch | Every cart cycle of ~192 lines | Must stay under ~100 ns per fetch |
| `$F1C5` RTS hook (arm the entry tracker) | End of 6-line overscan head | One store |
| `mc_on_ram_kernel_entry()` (pointers, line count) | RamKernel entry `$0083`, A12 low | A few pointer copies, no cart deadline |
| HMOVE pad (RIOT `$A8`–`$B2`) | After VBLANK off; A12 low | 44 cycles (`ldx #7` / `nop` / `bit $00` / `jmp $80`); not a line |
| Title buffer swap + pack | Right after the RamKernel entry, A12 low | Pointers + 32-byte pack, <15 µs |
| Overscan head (`$F164`, 6 lines) | After 192 picture lines; A12 high | 6 `AUDV0` + 32-byte copy; before blanking window |

For a healthy playback field: plan on **0.7 ms typical, 1.9 ms** if the FAT sector is cold and OSD is on. That is the number that must stay under **4.01 ms** (even RamKernel). Everything else either runs in visible / overscan head (DMA payload, pack copy) or is gated to one short SDIO touch per RamKernel (mount, probe, select).

### Memory map

| Region | Contents |
|--------|----------|
| **Flash `.hottext`** | `bus_dispatch`, `emulate_cartridge`, `wait_ram_kernel` (static) — fixed, line-aligned |
| **CCM** (`0x10000000`) | Stack (`_estack = 0x10010000`), `.data`/`.bss`, `r_coreInfo`, `romData` — D-bus only |
| **SRAM1** (`0x20000000`) | `mr_buffer1`, `diskBuffer` — SDIO DMA targets |
| **SRAM2** (`0x2001C000`) | `mr_buffer2` |

CCM holds all CPU data so it does not share an AHB port with DMA. The two field buffers sit on opposite SRAM slaves so a DMA write to one does not share a port with a display read of the other.

### Source layout

| Path | Role |
|------|------|
| `src/bus_service.c` | UnoCart poll loop, RamKernel waits, yield and delay |
| `src/core.c` | Cart dispatch (looping kernel, ColdStart, RamKernel image, nibble pack), frame diagnostics |
| `src/main.c` | Boot, SD milestones, title/playback loops, LED codes |
| `src/pff.c` | Petit FatFs (read-only) |
| `src/sd_reader.c` | Sector cache, UnoCart `disk_read` wrapper |
| `src/frame.c`, `update.c` | Field layout, transport/state machine |
| `Libraries/tm_stm32f4_fatfs/fatfs/drivers/fatfs_sd_sdio.c` | Stock UnoCart SDIO driver |
| `stm32f4_flash.ld` | Flash/RAM/SRAM2 sections |

## Status LED

**PA1** is driven only by the blink routines (`moviecart_stage_blink()`, `flash_led_slow()`, `flash_led()`); there is no kernel heartbeat.

### Boot milestones (`stage_pulse`, counted in frames)

| Flashes | Meaning |
|---------|---------|
| 1 | Title synchronized |
| 2 | Card mounted |
| 3 | Movie file opened |
| 4 | Title geometry OK |
| 5 | Playback starting |

Each milestone is 120 + 41×n frames, about 20 s of boot in all. They are off by default; `make NO_LED=0` turns them on (fatal codes always blink). The title keeps alternating fields through all five.

### Fatal codes (`fatalBlink`, slow wall-clock repeat)

| Code | Meaning |
|------|---------|
| 5 | Mount failed |
| 6 | Title never synchronized |
| 7 | Bad field header (`MVC\0` missing) |
| 8 | No playable file |
| 9 | Invalid field geometry (`visibleLines` / `numBlocks`) |
| 10 | Sector read failed after retries |

There is no kernel heartbeat during playback. Diagnostic codes are still recorded (`mc_diag_note()`, e.g. `DIAG_FIELD_RETRY`), but nothing currently reads them out.

## Build

Needs a complete **arm-none-eabi** toolchain (compiler **and** newlib). Homebrew's `arm-none-eabi-gcc` alone is built without headers and will not work.

```bash
cd firmware/stm32
./scripts/setup-toolchain.sh    # downloads ARM GNU 14.2 into tools/ (gitignored)
# or: brew install --cask gcc-arm-embedded

make clean && make              # build/firmware.elf, .bin, .hex
make flash                      # st-flash, if installed
```

Override toolchain: `make TOOLCHAIN=/path/to/arm-none-eabi/bin`

### Standalone on-target SD test harness

The standalone harness runs the real SDIO, DMA, Petit FatFs, field parsing, and
transport/update paths on the STM32 without reading or driving the Atari bus.
The Atari synchronization/yield calls become no-ops; required SD initialization
delays still use the DWT cycle counter.

```bash
make standalone-test
# build/standalone_test.elf, .bin, and .hex
make flash-standalone-test      # st-flash, if installed
```

Load `build/standalone_test.elf` in the STM32 debugger and set a breakpoint on
`moviecart_test_checkpoint`. Each stop occurs immediately before the operation
named by `mc_test_step`. Success and failure end in `moviecart_test_halt` with a
`BKPT`, so GDB stops instead of spinning. The build uses `-Og -g3 -fno-inline`
so calls can be stepped into.

Debugger-visible results:

- `mc_test_step` — current `enum moviecart_test_step`
- `mc_test_error` and `mc_test_detail` — failure reason and associated sector/value
- `mc_test_sector`, `mc_test_num_frames`, `mc_test_visible_lines`,
  `mc_test_field_blocks` — values discovered from the card and movie
- `mc_test_completed` — set to 1 after the full synchronous and asynchronous path

The SD card must contain at least one MovieCart file, as for normal firmware.
Breakpoints are safest at harness checkpoints or function entry. Halting for a
long time in the middle of an active SDIO command can allow the peripheral/card
transaction to finish while the CPU is stopped.

### Reference and diagnostic builds

| Make flag | Purpose |
|-----------|---------|
| `NO_SD=1` | Title + heartbeat only — pixel-perfect baseline |
| `SD_STAGE=1\|2\|3` | Stop after title sync / mount / file open |
| `MOUNT_PHASE=1\|2` | Bisect inside `pf_mount` (init vs first sector read) |
| `STALL_TEST=1\|2\|3` | Bus timing measurement sweeps (see `defines.h`) |
| `POLL_READ=1` | DMA-free FIFO drain path (experimental) |
| `FASTPATH=0` | Full DMA re-init per sector instead of M0AR-only retarget |
| `DMA_CLKDIV=N` | SDIO clock divider for DMA path (default `0x04` ≈ 8 MHz) |
| `NO_LED=0` | Bring back the milestone blinks (off by default; fatal codes always blink) |
| `NO_LOG=1` | Drop the RAM event log (`src/mclog.c`) |

The Makefile does not track these flags: object files built with one setting are reused under another. Run `make clean` whenever you change a flag.

### Event log

With the log built in (the default unless `NO_LOG=1`), `scripts/read_log.py` reads the 512-entry ring from SRAM2 over the ST-Link. Read it with the same `build/firmware.bin` that is on the board, and before power-cycling the 2600. The log survives a reset but not a power-off.

The SD driver's "SD cmd response still waiting" message in `CmdResp1Error_raw()` fires on practically every command during playback (three entries per frame), so the ring fills in seconds. Throttle or remove that message before using the log to diagnose anything else.

Not ported from dsPIC: in-field **FIRMWARE.FRM** flash update.
