# MovieCart — STM32F407VGT6 (DevEBox)

Firmware for the **MCUDEV DevEBox STM32F407VGT6** that emulates a MovieCart cartridge while streaming field files from the onboard microSD. Pin map and SDIO wiring match the [UnoCart-2600 DevEBox fork](../../../UnoCart-2600).

The original dsPIC33CK firmware lives in `firmware/` (parent of this directory).

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
│   First 6 overscan   ← cart $F160 (samples + pack copy)         │
│   Rest of OS / VS / VB ← RamKernel in RIOT $80 (nibble play)    │
└───────────────────────────┬─────────────────────────────────────┘
                            │ A12-high cart fetches (~838 ns/cycle)
┌───────────────────────────▼─────────────────────────────────────┐
│ STM32 main thread (IRQs masked while serving)                     │
│   emulate_cartridge() / bus_serve_cycle()  ← UnoCart poll loop  │
│   bus_dispatch()                           ← original-style jump table │
│                                                                 │
│ All SD (mount, open, probe, select, playback field loads):      │
│   DMA + manual completion poll in fatfs_sd_sdio.c               │
│   every wait yields → bus_serve_cycle() while RamKernel runs    │
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
| **RamKernel** | RIOT `$80`–`$CB` | Visible `jsr`, remaining OS/VS/VB nibble play, HMOVE pad (A12 low) |
| **Pack** | RIOT `$D0`–`$EF` | 32 bytes / 64 nibbles (blanking tail after 6 cart OS samples) |
| **VisibleBars** | Cart ROM | Looping dual-line kernel (`g0x3e`…`g0xb6`); `AUDV0` every scanline |

There is **no preroll**. The first 6 overscan lines run from cart (`$F160`): play 6 tail samples and copy the 32-byte pack. Remaining OS 23/24 + VSYNC + VBLANK run from RIOT so SDIO finish/begin stay A12-low. NTSC field is **192 + 6 + 23/24 + 3 + 37 = 261 / 262**. **NTSC is the target.** PAL still uses the NTSC 23/24+3+37 RamKernel (`visibleLines` still comes from the field header). Blanking audio is documented under **RIOT nibble audio** below.

Cart dispatch indexes **`addr & 0xfff`** (12-bit space). Key hook addresses:

| Offset | Symbol | MCU action |
|--------|--------|------------|
| `$F09D` | VisibleBars entry (nop) | Title: swap display buffer. Playback: no swap (already done in blanking). Reset audio cursor, set `mc_visible_bars_vended` |
| `$F09E`–`$F116` | right/left pair | Patch graphics/color/audio; `jmp $F09E` or `$F160` |
| `$F140`–`$F15F` | CartPack | 32 packed bytes (tail after 6 cart samples) |
| `$F16C` / `$F199` | overscan `lda #` | Next tail sample immediate (6 fetches) |
| `$F160`–`$F1C1` | overscan head | 5×6 + 2 copy, 6× `AUDV0`, joystick + `WSYNC`, RTS |
| `$F200`+ | RamKernel image | Copied to RIOT `$80` at ColdStart |
| `$FE00+x` | gstore | Latch SWCHA / SWCHB / INPT4 / INPT5 |

### RIOT nibble audio

Field files store **`totalLines`** volume bytes in play order: **[visible lines][blanking tail]**. The looping kernel writes `AUDV0` on every visible scanline (192 NTSC). The tail is typically 70 samples (overscan + VSYNC + VBLANK). There is no preroll; the field is **261 / 262** lines.

The last 64 tail samples cannot be cart immediates during RamKernel (A12 low, SDIO). The first **6** overscan lines are still cart: play those 6 samples and copy the remaining **32 packed bytes** into RIOT. RamKernel then plays the rest.

#### Pipeline

1. **Swap** — pack `audio[visibleLines+6 … totalLines)` (capped at 64 samples) into `mc_aud_pack[32]`. Playback swap is in `finishFieldRead`; title still swaps at `$F09D`.
2. **Visible** — 192 movie lines, unchanged.
3. **Overscan head** (`$F160`, 6 lines) — `lda #` / `sta AUDV0` for tail[0..5]; copy 5×6 + 2 bytes from `$F140` → `$D0`. Joystick and `WSYNC` on line 6, then `RTS` (blanking window starts).
4. **RamKernel OS/VS/VB** — unpack one nibble per line. Even OS 23, odd 24; +3 +37 = 63 / 64 play lines. A12 low. SDIO window.
5. Next swap fills the pack for the following field. The 6-line copy at the end of that field’s picture loads it.

Pack is taken from the display buffer **base** at swap (`+ visible + 6`), not from the live audio cursor. The 6 head samples are consumed from the cursor during `$F160`. `RTS` snaps the cursor to `base + totalLines`.

#### Who does what

| Side | When | Action |
|------|------|--------|
| STM32 | swap | Pack two samples per byte into `mc_aud_pack[32]`; serve `$F140` |
| STM32 | `$F16C` / `$F199` | Next tail sample as `lda #` immediate (6 fetches) |
| 6502 overscan head | After 192 picture lines | Play 6 samples; `lda $F140,x` / `sta $D0,x` |
| 6502 RamKernel | A12 low | `AUDIDX` even → low nibble; odd → `lsr ×4`; both `and #$0F`; `sta AUDV0` |

#### RIOT map (128 bytes, `$80`–`$FF`)

| RIOT | Size | Role |
|------|-----:|------|
| `$80`–`$A7` | 40 | Frame: `jsr $F09D`, remaining OS 23/24, VS 3, VB 37, `stx VBLANK` / `AUDIDX` |
| `$A8`–`$B2` | 11 | HMOVE pad (`ldx #7` / `nop` / `bit $00` / `jmp $80`) — not a scanline |
| `$B3`–`$CB` | 25 | `Play` (`lda $D0,y`, nibble unpack, `sta AUDV0` / `WSYNC`) |
| `$CC`–`$CF` | 4 | unused |
| `$D0`–`$EF` | 32 | Packed tail after the 6 cart OS samples |
| `$F0`–`$FA` | 11 | unused |
| `$FB` | 1 | `FIELD` — even/odd (OS 23/24 only). Not the STM32 field buffer. |
| `$FC` | 1 | `AUDIDX` |
| `$FD` | 1 | unused |
| `$FE`–`$FF` | 2 | 6502 stack for `jsr VisibleBars` / `jsr Play` |

`$80`–`$CB` is 76 bytes (copied from `$F200`). `$80`–`$FF` is 128.

#### RamKernel listing

Firmware image `mc_ram_kernel[]` at `$F200`, copied to `$80`. Not assembled from `core.asm`.

```asm
; rorg $80
RamKernel
        jsr     $F09D           ; VisibleBars (cart)
        inc     $FB             ; FIELD
        lda     $FB
        lsr
        ldx     #23
        bcc     .even
        inx                     ; 24 on odd
.even   jsr     Play            ; $B3
        lda     #2
        sta     $00             ; VSYNC
        ldx     #3
        jsr     Play
        stx     $00             ; VSYNC off (X=0)
        lda     #2
        sta     $01             ; VBLANK
        ldx     #37
        jsr     Play
        stx     $01             ; VBLANK off
        stx     $FC             ; AUDIDX = 0
        ldx     #7              ; HMOVE pad, $A8 (44 cycles, not a line)
.pad    dex
        bne     .pad
        nop
        bit     $00
        jmp     $80

Play                            ; $B3
        lda     $FC             ; AUDIDX
        lsr
        tay
        lda     $00D0,y         ; PACK
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
- **Even vs odd** — 6 cart + 63 RIOT = 69; 6 + 64 = 70. The extra even file nibble is dropped.
- **PAL / long tails** — NTSC is the target. PAL still uses the NTSC 23/24+3+37 RamKernel. Pack is fixed at 64 samples / 32 bytes after the 6 head lines.
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

`moviecart_bus_yield()` serves every cycle while visible; during blanking it serves **A12-high (cart) cycles only** so `$F09D` is not missed and `mc_blanking_window` clears. A12-low blanking cycles are skipped for faster SDIO polls.

### SD I/O (no IRQs, no WaitCart)

The stock UnoCart SDIO driver (`fatfs_sd_sdio.c`) keeps **DMA** for sector reads. Completion flags (`TransferEnd`, `DMAEndOfTransfer`) are set by calling `SD_ProcessIRQSrc()` and `SD_ProcessDMAIRQ()` from the wait loops — not from interrupt handlers. **IRQs stay masked** for the entire Atari serve run.

Every SDIO wait loop calls `moviecart_bus_yield()` (or `SD_PollDmaAndYield()` for DMA drains). Long delays use `moviecart_delay_ms()` instead of busy DWT spins.

**Blanking-only SDIO control:** `mc_blanking_window_gen` increments on each overscan-head RTS (`$F1C1`). By default `moviecart_sdio_gate()` blocks until that counter advances (mount, title probe, file select). Playback command initiation and completion polling use the relaxed gate in their respective blanking intervals. The DMA itself runs asynchronously between them while the other field buffer is displayed.

### Playback loop

After the embedded title screen and SD mount:

1. **`runTitle()`** — title hold; geometry probe reads one field sector while yielding.
2. **`runFrameLoop()`** — two-buffer DMA pipeline (see below).

Field files: FAT32 root, first playable file by default; `MVC\0` header, 1–6 sectors (≤ 3 KiB field buffer). Controls match stock MovieCart (Select advances files, etc.).

FatFs cluster walks and other CPU-bound loops call `moviecart_bus_yield()` so the cart keeps being served.

### Double buffering

`mr_buffer1` (SRAM1) and `mr_buffer2` (SRAM2) are a ping-pong pair. One field is displayed while SDIO DMA fills the other. `diskBuffer` is FAT/dir scratch only; field loads never use it.

| Interval | Display | DMA target | Main-thread work |
|----------|---------|------------|------------------|
| Visible N | A | B (may still be filling) | Serve cart only |
| Blanking N (`$F1C1`) | — | B, then A | Finish/validate B, swap to B, start CMD18 into A |
| Visible N+1 | B | A | Serve cart; `$F09D` does **not** swap |
| Blanking N+1 | — | A, then B | Finish/validate A, swap to A, start CMD18 into B |

`$F09D` still swaps during **title** (`mc_playback_pipeline == 0`). Once `runFrameLoop()` sets that flag, the swap happens in blanking inside `finishFieldRead()` after the field is validated — VisibleBars must not see a half-filled buffer.

The first playback pass repeats the title once while the first CMD18 is primed. A failed field is retried into the same inactive buffer and is not swapped in.

`beginFieldRead()` / `finishFieldRead()` each set `mc_sdio_gate_relaxed` so command start and completion polls use the current blanking window instead of waiting for a new `$F1C1` edge. The DMA itself runs through the following visible interval.

#### DMA completion (frame data)

IRQs stay masked. After `waitEndFrame()`, if `field_dma_pending`:

`finishFieldRead()` → `pf_read_blocks_finish()` → `disk_read_blocks_finish()` → `TM_FATFS_SD_SDIO_disk_read_finish()` → `SD_WaitReadOperation()`

`SD_WaitReadOperation()` loops until **either** `TransferEnd` **or** `DMAEndOfTransfer` (or error / timeout):

```
while (!DMAEndOfTransfer && !TransferEnd && TransferError == SD_OK)
    SD_PollDmaAndYield();
```

Each poll:

1. `moviecart_sdio_gate()` — relaxed: wait until `mc_blanking_window` is set.
2. `SD_ProcessIRQSrc()` — if `SDIO_IT_DATAEND`, set `TransferEnd` (also records CRC / timeout / overrun).
3. `SD_ProcessDMAIRQ()` — if DMA2 Stream3 TCIF, set `DMAEndOfTransfer` and clear the flag.
4. `moviecart_bus_yield()` — keep serving A12-high cart cycles.

Those two `SD_Process*` helpers are the stock “IRQ” handlers; they are called from the wait loop, not from NVIC.

After the flags trip, the finish path also:

- waits until `SDIO->STA` is not `RXACT`
- sends CMD12 (`SD_StopTransfer`) because the field was a multi-block CMD18
- polls `SD_GetStatus()` (CMD13) until the card is not `SD_TRANSFER_BUSY`

Then `finishFieldRead()` checks `MVC\0` and geometry against the title-probe cache (`playback_field_blocks`). Only a valid field calls `updateBuffer()` and `mc_field_swap_to_display()`.

### RamKernel timing budget

RamKernel is only the A12-low nibble-play tail: `$F1C1` RTS through VBLANK off. That is when `finishFieldRead` / `beginFieldRead` run. The first 6 overscan lines are cart (`$F160`: 6 `AUDV0` + 32-byte pack copy) and finish **before** the blanking window opens — they are not in this budget. After VBLANK, an in-RIOT pad restores HMOVE phase (still A12 low, after the window).

Those 6 lines used to be RIOT overscan (29/30 OS + 3 VS + 37 VB = 69/70, **4.40 / 4.46 ms**). They are now cart, so the SDIO-quiet window is 6 lines shorter: **23/24 + 3 + 37 = 63 / 64**, **4.01 / 4.08 ms**.

Line math is NTSC (76 CPU cycles at 1.193182 MHz = 63.695 µs). SD peak is 8 MHz × 4-bit = 4 MB/s (`DMA_CLKDIV=0x04`). Figures are calculated from the source, not scoped on the board.

| | Even | Odd |
|--|-----:|----:|
| **RamKernel (remaining OS+VS+VB)** | **4.01 ms** (63 lines) | **4.08 ms** (64 lines) |
| Typical playback work | **0.35–0.80 ms** (9–20% of even RamKernel) | same |
| Worst must-fit stack | **1.9 ms** (FAT miss + OSD + slow CMD13) | 2.1 ms slack in 4.01 ms |
| Pathological | **3.4 ms** (plus leftover DMA still on the wire) | 0.6 ms slack; tighter than the old 4.40 ms window |

Playback work must finish before VBLANK goes off. Mount, title probe, and file-select are sliced across many frames by the strict SDIO gate and do not have to finish in one RamKernel.

#### Window breakdown

NTSC field: **192 + 6 + 23/24 + 3 + 37 = 261 / 262**. Only the 63 / 64 Play lines are the SDIO-quiet RamKernel window. VSYNC/VBLANK register writes sit in the first cycles of a Play line (after the previous `WSYNC`); they are not extra scanlines. The HMOVE pad after VBLANK off is **44 cycles** (`ldx #7` / `nop` / `bit $00` / `jmp $80`) — not a line, and after the window.

| Segment | Even (lines) | Odd (lines) | Even (ms) | Odd (ms) | Budget? |
|---------|-------------:|------------:|----------:|---------:|---------|
| Picture (cart) | 192 | 192 | 12.23 | 12.23 | No — A12 high |
| Overscan head (cart `$F160`) | 6 | 6 | 0.38 | 0.38 | No — A12 high; 6 samples + 32-byte copy |
| Remaining overscan (RIOT) | 23 | 24 | 1.47 | 1.53 | Yes |
| VSYNC (3 Play lines) | 3 | 3 | 0.19 | 0.19 | Yes |
| VBLANK (37 Play lines) | 37 | 37 | 2.36 | 2.36 | Yes |
| **RamKernel (Play)** | **63** | **64** | **4.01** | **4.08** | SDIO-quiet window |
| HMOVE pad (RIOT `$A8`) | 0 | 0 | 0.04 | 0.04 | No — after VBLANK off; A12 low |
| **Field** | **261** | **262** | **16.62** | **16.69** | — |

**NTSC is the target.** PAL still uses the NTSC 23/24+3+37 RamKernel; `visibleLines` comes from the field header.

#### Case-by-case

| Case | Where | Gate | Typical | Worst | One window? |
|------|-------|------|---------|-------|-------------|
| 1. Nominal playback | Every field after `waitEndFrame` | Relaxed | 0.35–0.80 ms | 1.2 ms | Yes — this is the budget case |
| 2. Playback + FAT miss | `pf_seek_block` → `get_fat` uncached | Relaxed | +0.3–0.5 ms | +0.8–1.0 ms | Yes; still under 2 ms stacked |
| 3. Playback + OSD | `updateColor` level bars / timecode | Relaxed | +40–80 µs | +0.10 ms | Yes; yields every 4–8 bytes |
| 4. DMA leftover at RTS | `finishFieldRead` poll before DATAEND | Relaxed | 0 (DMA done in visible) | 0.8–1.5 ms | Yes; should not happen if CMD18 started last blanking |
| 5. Field retry | finish fails, `beginFieldRead` again | Relaxed | +0.4 ms | +1.0 ms | Yes; then waits for `$F09D` and skips swap |
| 6. Title probe | `runTitle` `pf_seek` + 1-sector read | Strict | 1–3 frames | Several frames | No — one SDIO touch per edge |
| 7. File select | `checkSelectVideo` open + probe + wipe | Strict, then relaxed wipe | Many frames | Directory walk + 30-frame hold | No |
| 8. Mount / `SD_Init` | `setupDisk` before playback | Strict + `delay_ms` | Hundreds of ms to seconds | 5× (`SD_Init` + 50 ms) | No — title is already looping |
| 9. RTS / entry hooks | `$F1C1` and `$F09D` dispatch | On the cart cycle | <2 µs | <5 µs | Cycle budget (~100 ns), not RamKernel |

#### Playback blanking — step timings

`runFrameLoop` after `waitEndFrame`: `finishFieldRead` → `checkSelectVideo` (usually a compare) → `updateTransport` → `beginFieldRead`. That stack must finish in the 4.01 ms RamKernel window (before VBLANK off). The 32-byte pack copy already happened in the 6 cart overscan-head lines.

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
| `pf_seek_block` hit | Queue scan, same cluster | 10–50 µs | 50 µs | 128-entry queue, yield /16 |
| `pf_seek_block` FAT miss | `get_fat` → CMD17 512 B | 300–500 µs | 800–1000 µs | Same relaxed window as CMD18 |
| CMD18 arm | DMA setup + CMD16? + CMD18 | 100–200 µs | 250 µs | Payload runs in the next visible |

Stacked totals in one **RamKernel** (168 MHz CPU, 8 MHz 4-bit SDIO). Shares are of 4.01 ms even-field.

| Stack | Sum | Share of 4.01 ms |
|-------|-----|------------------|
| Sequential + cache hit | finish 0.20 + volume 0.05 + color 0.27 + seek 0.03 + CMD18 0.15 + pack 0.01 = **0.71 ms** | 18% |
| FAT miss + OSD + slow busy | finish 0.50 + volume 0.06 + color 0.35 + FAT 0.80 + CMD18 0.20 = **1.91 ms** | 48% |
| Plus leftover DMA | add 1.5 ms for a full PAL field still on the wire = **3.41 ms** | 85% |

#### Work that is not a single-window max

`moviecart_sdio_gate()` in strict mode waits for a new `mc_blanking_window_gen` on every call. Each `SendCommand` / `DataConfig` / poll therefore consumes at most one RamKernel, then sleeps until the next RTS. Per-window STM32 time is one command or one status poll.

| Path | Per-window slice | How it spans frames |
|------|------------------|---------------------|
| `SD_Init` / mount (400 kHz, then 8 MHz) | One CMD + response, or 2 ms / 50 ms `delay_ms` pump | Init clock 48 MHz/(0x76+2) ≈ 400 kHz. `delay_ms` yields on A12-low. |
| Title probe (1 sector) | CMD17 data 128 µs + commands ~200–400 µs if it all landed in one window; usually split | Each `gate()` inside `WaitReadOperation` waits for the next edge. |
| `pf_open_file` / directory walk | One 512 B directory or FAT sector | Repeats until the first playable file is found. |
| `clearFieldBuffers` after select | ~100–200 µs (6 KiB memset, yield /64) | Then 30× `waitEndFrame` hold. Not SDIO. |
| Rewind cluster walk | One `get_fat`; FAT sector often cached after the first read | Checkpoint every 63 clusters (`pff.c` `skip &= 63`). Comment: stutter ~19 min reverse on a formatted card. |

#### What is not charged to RamKernel

| Work | When it runs | Duration |
|------|--------------|----------|
| Field CMD18 payload (5–6 × 512 B) | Visible interval after `beginFieldRead` | 0.64 ms peak (4 MB/s, 5 sectors) · ~0.77 ms PAL 6 sectors · up to ~1.5 ms on a 2 MB/s card |
| Looping visible kernel dispatch | Every cart cycle of ~192 lines | Must stay under ~100 ns per fetch |
| `$F1C1` RTS hook (snap audio, diag, `mr_endFrame`) | End of 6-line overscan head | <2 µs on that cycle |
| HMOVE pad (RIOT `$A8`–`$B2`) | After VBLANK off; A12 low | 44 cycles (`ldx #7` / `nop` / `bit $00` / `jmp $80`); not a line |
| Title buffer swap + pack at `$F09D` | Start of visible | Pointers + 32-byte pack, <15 µs |
| Overscan head (`$F160`, 6 lines) | After 192 picture lines; A12 high | 6 `AUDV0` + 32-byte copy; before blanking window |

For a healthy playback field: plan on **0.7 ms typical, 1.9 ms** if the FAT sector is cold and OSD is on. That is the number that must stay under **4.01 ms** (even RamKernel). Everything else either runs in visible / overscan head (DMA payload, pack copy) or is gated to one short SDIO touch per RamKernel (mount, probe, select).

### Memory map

| Region | Contents |
|--------|----------|
| **Flash `.hottext`** | `bus_dispatch`, `emulate_cartridge`, `bus_serve_cycle`, `moviecart_bus_pump` — fixed, line-aligned |
| **CCM** (`0x10000000`) | Stack (`_estack = 0x10010000`), `.data`/`.bss`, `r_coreInfo`, `romData` — D-bus only |
| **SRAM1** (`0x20000000`) | `mr_buffer1`, `diskBuffer` — SDIO DMA targets |
| **SRAM2** (`0x2001C000`) | `mr_buffer2` |

CCM holds all CPU data so it does not share an AHB port with DMA. The two field buffers sit on opposite SRAM slaves so a DMA write to one does not share a port with a display read of the other.

### Source layout

| Path | Role |
|------|------|
| `src/bus_service.c` | UnoCart poll loop, yield/pump helpers |
| `src/core.c` | Cart dispatch (looping kernel, ColdStart, RamKernel image, nibble pack), frame diagnostics |
| `src/main.c` | Boot, SD milestones, title/playback loops, LED codes |
| `src/pff.c` | Petit FatFs (read-only) |
| `src/sd_reader.c` | Sector cache, UnoCart `disk_read` wrapper |
| `src/frame.c`, `update.c` | Field layout, transport/state machine |
| `Libraries/tm_stm32f4_fatfs/fatfs/drivers/fatfs_sd_sdio.c` | Stock UnoCart SDIO driver |
| `stm32f4_flash.ld` | Flash/RAM/SRAM2 sections |

## Status LED

**PA1** has one writer at a time. `mc_led_host` suppresses the kernel heartbeat while main blinks a code.

### Boot milestones (`flash_led`, wall-clock)

| Flashes | Meaning |
|---------|---------|
| 1 | Title synchronized |
| 2 | Card mounted |
| 3 | Movie file opened |
| 4 | Title geometry OK, playback starting |

### Fatal codes (`fatalBlink`, slow wall-clock repeat)

| Code | Meaning |
|------|---------|
| 5 | Mount failed |
| 6 | Title never synchronized |
| 7 | Bad field header (`MVC\0` missing) |
| 8 | No playable file |
| 9 | Invalid field geometry (`visibleLines` / `numBlocks`) |
| 10 | Sector read failed after retries |

**Long 2 s strobe** (not a count): field signature found in scratch buffer but not in the target field buffer — DMA destination issue.

### Kernel heartbeat (during playback)

Once per second when healthy — worst condition since last report:

| Flashes | Meaning |
|---------|---------|
| 1 | Nominal |
| 2 | Line counter wrapped in visible section |
| 3 | Line counter wrapped in end-lines section |
| 4 | Visible line count out of range |
| 5 | Field load recovered after retry (`DIAG_FIELD_RETRY`) |

Frozen LED: kernel stopped completing frames.

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

### Reference and diagnostic builds

| Make flag | Purpose |
|-----------|---------|
| `NO_SD=1` | Title + heartbeat only — pixel-perfect baseline |
| `SD_STAGE=1\|2\|3` | Stop after title sync / mount / file open |
| `MOUNT_PHASE=1\|2` | Bisect inside `pf_mount` (init vs first sector read) |
| `STALL_TEST=1\|2\|3` | Bus timing measurement sweeps (see `defines.h`) |
| `GAP_PROBE=1` | Worst serve-to-serve gap report (disturbs timing — diagnostic only) |
| `POLL_READ=1` | DMA-free FIFO drain path (experimental) |
| `FASTPATH=0` | Full DMA re-init per sector instead of M0AR-only retarget |
| `DMA_CLKDIV=N` | SDIO clock divider for DMA path (default `0x04` ≈ 8 MHz) |

Not ported from dsPIC: in-field **FIRMWARE.FRM** flash update.
