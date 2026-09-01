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
│   Visible scanlines  ← cart ROM (VisibleBars + patched immediates)│
│   Sync / blank / OS  ← RamKernel copied to RIOT $80 at boot     │
└───────────────────────────┬─────────────────────────────────────┘
                            │ A12-high cart fetches (~838 ns/cycle)
┌───────────────────────────▼─────────────────────────────────────┐
│ STM32 main thread (IRQs masked while serving)                     │
│   emulate_cartridge() / bus_serve_cycle()  ← UnoCart poll loop  │
│   bus_dispatch()                           ← core.bin + patches │
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

### RamKernel split (`kernel/core.asm` → `core.bin`)

The Atari-side kernel is built with DASM in `kernel/` and embedded as `mc_core_rom[4096]` in `src/core_rom.c` (regenerated automatically when `core.bin` changes).

| Region | Where it runs | Role |
|--------|---------------|------|
| **ColdStart** | Cart ROM | Copies **RamKernel** into RIOT `$80`, then enters it |
| **RamKernel** | RIOT `$80`+ | VSYNC, VBLANK, overscan, preroll — no cart fetches during blanking |
| **VisibleBars** | Cart ROM | Visible scanline prologue only; eight dual-line kernels with patched immediates |

Blanking therefore executes from RIOT RAM. The cart is only fetched for ColdStart, the RamKernel image bytes, VisibleBars entry/RTS, and per-line immediate patches.

Cart dispatch indexes **`addr & 0xfff`** (12-bit space). Key hook addresses:

| Offset | Symbol | MCU action |
|--------|--------|------------|
| `$F09D` | VisibleBars entry | Buffer swap, reset audio cursor, set `mc_visible_bars_vended` |
| `$F41D` | VisibleBars RTS | End frame (`mr_endFrame`), skip blanking audio tail, set `mc_blanking_window` |
| `$0BF`–`$40D` | Visible lines | Static bytes from `core.bin`; immediates patched via phase map → `g0x3e`…`g0xae` |

Audio in field files is stored **[visible lines][blanking tail]**. VisibleBars only writes `AUDV0` on the eight dual-line kernels, not every scanline and not during RIOT blanking. At `$F41D` the audio pointer skips the unplayed visible tail plus blanking samples so the next field stays aligned.

### Cart bus serving

Serving follows UnoCart-2600's `driver_4k.c` polling loop:

1. Wait for a stable address on PE0–PE12.
2. If A12 high: `bus_dispatch()`, write ODR, enable outputs, wait for address change, tri-state.
3. Repeat forever with **interrupts disabled** on the main thread.

Critical rules (measured on this hardware):

- **No work between address sample and ODR write** — even ~60 ns settle windows black the display.
- **No repeat-address suppression** — the kernel legitimately refetches the same cart address.
- **`SET_DATA()` enables drivers** immediately after writing ODR; dispatch side effects run before returning.
- **Bus code runs from flash** (`.hottext`, ART-line aligned), not SRAM. The `romData` jump table stays in SRAM so table lookups do not contend with instruction fetch.
- Per-cycle budget is ~**100 ns**; one missed ~838 ns cycle can desync the scanline counter.

`moviecart_bus_yield()` serves every cycle while visible; during blanking it serves **A12-high (cart) cycles only** so `$F09D` is not missed and `mc_blanking_window` clears. A12-low blanking cycles are skipped for faster SDIO polls.

### SD I/O (no IRQs, no WaitCart)

The stock UnoCart SDIO driver (`fatfs_sd_sdio.c`) keeps **DMA** for sector reads. Completion flags (`TransferEnd`, `DMAEndOfTransfer`) are set by calling `SD_ProcessIRQSrc()` and `SD_ProcessDMAIRQ()` from the wait loops — not from interrupt handlers. **IRQs stay masked** for the entire Atari serve run.

Every SDIO wait loop calls `moviecart_bus_yield()` (or `SD_PollDmaAndYield()` for DMA drains). Long delays use `moviecart_delay_ms()` instead of busy DWT spins.

**Blanking-only SDIO control:** `mc_blanking_window_gen` increments on each VisibleBars RTS (`$F41D`). By default `moviecart_sdio_gate()` blocks until that counter advances (mount, title probe, file select). Playback command initiation and completion polling use the relaxed gate in their respective blanking intervals. The DMA itself runs asynchronously between them while the other field buffer is displayed.

### Playback loop

After the embedded title screen and SD mount:

1. **`runTitle()`** — title hold; geometry probe reads one field sector while yielding.
2. **`runFrameLoop()`** — two-buffer DMA pipeline:
   - `waitEndFrame()` — blocked on `mr_endFrame` (set at VisibleBars RTS).
   - `checkSelectVideo()` / `updateTransport()` — console controls; file change opens the next movie file while yielding.
   - Finish and validate the CMD18 DMA started in the preceding blanking interval, then swap display pointers **during that same blanking window**.
   - Start one CMD18 DMA for the following field into the buffer whose visible use just ended. The title probe caches `numBlocks`, so all 1–6 contiguous sectors use one transfer.
   - At **`$F09D`** VisibleBars starts using the already-swapped buffer. The first pipeline pass repeats the title once while priming DMA.

Field files: FAT32 root, first playable file by default; `MVC\0` header, 1–6 sectors (≤ 3 KiB field buffer). Controls match stock MovieCart (Select advances files, etc.).

FatFs cluster walks and other CPU-bound loops call `moviecart_bus_yield()` so the cart keeps being served.

### Memory map

| Region | Contents |
|--------|----------|
| **Flash `.hottext`** | `bus_dispatch`, `emulate_cartridge`, `bus_serve_cycle`, `moviecart_bus_pump` — fixed, line-aligned |
| **CCM** (`0x10000000`) | Stack (`_estack = 0x10010000`), `r_coreInfo`, `romData` — D-bus only |
| **SRAM1** (`0x20000000`) | `mr_buffer1`, `diskBuffer`, remaining `.data`/`.bss` |
| **SRAM2** (`0x2001C000`) | `mr_buffer2` |

CCM takes the serve-path table and stack off the AHB matrix. The two field buffers sit on opposite SRAM slaves so a DMA write to one does not share a port with a display read of the other.

### Source layout

| Path | Role |
|------|------|
| `src/bus_service.c` | UnoCart poll loop, yield/pump helpers |
| `src/core.c` | Cart dispatch, RamKernel hooks, WaitCart protocol, frame diagnostics |
| `src/core_rom.c` | Generated embed of `kernel/core.bin` |
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
| 4 | Frame length out of range (250–275 scanlines) |
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

The build runs `make -C ../../kernel core.bin` and regenerates `src/core_rom.c` from it.

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
