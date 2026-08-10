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
and never leaves; therefore **no LED blinks are expected**. SD initialization
and movie playback are temporarily disabled.

The serving loop is copied in shape from UnoCart-2600's `emulate_cartridge()` (`source/STM32firmware/standalone/src/driver_4k.c`), which is known good on this exact board and wiring: converge on a stable address (spin until two consecutive reads agree), write the byte to ODR **while the pins are still inputs**, then switch to output, then tristate the moment the address changes (not when A12 falls, so a following 6502 write cycle never meets our drivers). GPIO config for PD8-15 also matches UnoCart exactly: no pull, reset-default slew.

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
