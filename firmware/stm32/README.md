# MovieCart — STM32F407VGT6 (DevEBox)

Port of the MovieCart cart firmware to the **MCUDEV DevEBox STM32F407VGT6**, using the same cartridge pin map and onboard microSD wiring as the nearby [UnoCart-2600 DevEBox fork](../../../UnoCart-2600).

The original dsPIC33CK firmware remains under `firmware/` (parent of this directory).

## Wiring

| Atari | MCU |
|-------|-----|
| A0–A12 | **PE0–PE12** |
| D0–D7 | **PD8–PD15** |
| +5V / GND | Board 5V input / GND (MCU is 3.3 V; level-shift or series resistors as on your breakout) |

Onboard microSD (SDIO 4-bit):

| SD | MCU |
|----|-----|
| D0–D3 | PC8–PC11 |
| CK | PC12 |
| CMD | PD2 |

**Power:** DevEBox is powered from the Atari (same power-up).

The current build is deliberately **title-only** to validate the display bus
independently of SD. After initialization it enters UnoCart's infinite bus loop
and never leaves. SD initialization and movie playback are temporarily disabled,
so the only LED activity is the ~1 Hz kernel heartbeat described below.

The serving loop is copied in shape from UnoCart-2600's `emulate_cartridge()` (`source/STM32firmware/standalone/src/driver_4k.c`), which is known good on this exact board and wiring: converge on a stable address, write the byte to ODR **while the pins are still inputs**, then switch to output, then tristate once the address changes (not when A12 falls, so a following 6502 write cycle never meets our drivers). GPIO config for PD8-15 also matches UnoCart exactly: no pull, reset-default slew.

Do not add work between the address read and the moment the drivers turn on.
Inserting a ~60 ns settle window there — to reject the composite addresses that
appear while the lines slew, which MovieCart's stateful dispatch cannot shrug
off the way a plain ROM cart can — killed the display completely.

Do not add a "don't dispatch the same address twice in a row" filter either. It
was tried on the reasoning that the 6502 never fetches one cart address on
consecutive cycles, so a repeat must be our own data pins coupling into the
address lines. On hardware it made things strictly worse — a permanent black
screen and a constant 3-blink code — because it also suppresses dispatches the
kernel genuinely needed.

`SET_DATA()` in `core.c` enables the output drivers itself,
immediately after writing ODR, instead of leaving it to the caller. A dispatch
case runs its side effects *before* returning, so a `SET_DATA_MODE_OUT` in the
bus loop would be delayed by that case's entire workload. Light cases cost a few
cycles and survive; the end-of-frame chain at `$FFdb`–`$FFe3`, which reloads
every `frameInfo` pointer and restarts the line counter, does not — and those
are precisely the cycles that set up VSYNC.

The bus loop owns the CPU with interrupts off, so the status LED is the only
channel out. `core.c` drives it from the kernel's own end-of-frame, one slot per
frame, blinking the worst condition seen since the last report:

| Flashes | Meaning |
| --- | --- |
| 1 | nominal — frames closing normally, scanline count in range |
| 2 | line counter wrapped in the visible section (a dispatch was missed) |
| 3 | line counter wrapped in the end-lines section |
| 4 | frame was not 250–275 scanlines |

A frozen LED means the kernel stopped completing frames altogether.

The kernel's line counter is stepped by the 6502's fetches, so one missed or
duplicated dispatch flips its parity, `lines -= 2` steps past zero and wraps,
and the frame never ends: buffer pointers advance forever, the picture becomes a
scroll through SRAM, and the read finally leaves SRAM and faults the CPU (which
looks like the console dying a few seconds in). `LINES_EXHAUSTED()` treats an
impossibly large counter as end-of-section, so the frame closes, every pointer
is reloaded from `mr_frameInfo1/2`, and the kernel resyncs instead of running
away.

The bus loop runs in main with interrupts disabled, exactly like UnoCart. There
is no TIM1 or EXTI bus server in this validation build.

## Build

Needs `arm-none-eabi-gcc` (the UnoCart `tools/` toolchain works):

```bash
export PATH="/path/to/arm-none-eabi/bin:$PATH"
cd firmware/stm32
make clean
make bin hex    # build/firmware.bin and build/firmware.hex
```

Flash with ST-Link (`make flash` / `st-flash`) or DFU (BOOT0) on the DevEBox.

## Runtime notes

- System clock: HSI → PLL @ 168 MHz (same as UnoCart DevEBox fork); SDIOCLK 48 MHz.
- Bus servicing: UnoCart stable-address polling loop, main thread, IRQs disabled.
- Content: FAT/FAT32 card with MovieCart field files (same as stock). Select advances to the next file; joystick/console controls match the original.
- Status LED bit-bang uses **PA1** (optional).
- In-field FIRMWARE.FRM flash update from the dsPIC build is not ported.
