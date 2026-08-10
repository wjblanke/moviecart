#ifndef CARTRIDGE_IO_H
#define CARTRIDGE_IO_H

#include "stm32f4xx.h"

/* Address bus A0-A12 on PE0-PE12 (PE13-PE15 unused, pulled down). */
#define ADDR_IN GPIOE->IDR
/* Data bus D0-D7 on PD8-PD15 (high byte). Low PD pins left free (e.g. PD2 SDIO_CMD). */
#define DATA_IN GPIOD->IDR
/* Keep PD0-PD7 (incl. SDIO CMD on PD2) when writing D0-D7 on PD8-PD15. */
#define DATA_OUT_SET(v) do { GPIOD->ODR = (GPIOD->ODR & 0x00FFu) | ((uint16_t)(v) & 0xFF00u); } while (0)
/* Legacy lvalue used as DATA_OUT = (byte<<8); clears PD0-7 — prefer DATA_OUT_SET. */
#define DATA_OUT GPIOD->ODR
#define CONTROL_IN GPIOC->IDR
/* Only touch MODER for pins 8-15; preserve PD0-PD7 (SDIO, etc.). */
#define SET_DATA_MODE_IN  GPIOD->MODER = (GPIOD->MODER & 0x0000FFFFu);
#define SET_DATA_MODE_OUT GPIOD->MODER = (GPIOD->MODER & 0x0000FFFFu) | 0x55550000u;

#endif // CARTRIDGE_IO_H
