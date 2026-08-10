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

Cart select matches the dsPIC CLC: respond only when **A11 and A12 are high** (upper 2K of the slot). Data pins are driven on PD8–PD15; PD0–PD7 (including SDIO CMD) are left alone.

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
- Bus servicing runs from main and from an **EXTI on PE12 (A12)** so SDIO block reads can be preempted.
- Content: FAT/FAT32 card with MovieCart field files (same as stock). Select advances to the next file; joystick/console controls match the original.
- Status LED bit-bang uses **PA1** (optional).
- In-field FIRMWARE.FRM flash update from the dsPIC build is not ported.
