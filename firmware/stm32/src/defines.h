/**
 * MovieCart on MCUDEV DevEBox STM32F407VGT6
 *
 * Cart buses match UnoCart-2600 DevEBox fork:
 *   A0–A12 = PE0–PE12
 *   D0–D7  = PD8–PD15
 * Onboard microSD via SDIO 4-bit (PC8–12, PD2).
 */
#ifndef MOVIECART_STM32_DEFINES_H
#define MOVIECART_STM32_DEFINES_H

#include <stdint.h>
#include <stdbool.h>

#include "movie_defines.h"

/* FatFs / SDIO (Tilen Majerle driver) */
#define FATFS_USE_SDIO			1
#define FATFS_SDIO_4BIT			1
#define FATFS_USE_DETECT_PIN		0
#define FATFS_USE_WRITEPROTECT_PIN	0
#define SDIO_TRANSFER_CLK_DIV		((uint8_t)0x08)

/* UnoCart DevEBox: A12 high selects cart; kernel indexed by A0-A8. */
#define CART_ADDR_MASK			0x1000u
#define CART_ADDR_SELECT		0x1000u

/*
 * Bus-serving code runs from FLASH, not SRAM — the same as UnoCart, which has no
 * RAM-relocated code at all and is stable on this board.
 *
 * SRAM placement was inherited from the abandoned EXTI design, where flash wait
 * states on interrupt entry were the problem. It is actively harmful for a
 * polling loop: on the STM32F407 the Cortex-M4 reaches SRAM over the system bus,
 * so instruction fetches there contend with every data access the loop makes —
 * the romData table, r_coreInfo, the frame buffers, and the GPIO registers
 * themselves. There is no instruction cache on that path either, so each fetch
 * is a fresh bus transaction whose cost depends on where the code happens to
 * land. From flash the fetches go over the I-bus through the ART cache, leaving
 * the system bus entirely to data. That decoupling is also the likely reason
 * this build has been so absurdly sensitive to adding a single instruction.
 *
 * romData stays in SRAM (see core.c): a table read over the system bus now runs
 * in parallel with instruction fetch rather than competing with it.
 */
#define RAMFUNC __attribute__((noinline))

/* Optional status LED on PA1 (DevEBox has LEDs on PA1/PA2/PA3 on many boards). */
#define STATUS_LED_GPIO			GPIOA
#define STATUS_LED_PIN			GPIO_Pin_1

#define TESTA0_LOW	do { STATUS_LED_GPIO->BSRRH = STATUS_LED_PIN; } while (0);
#define TESTA0_HIGH	do { STATUS_LED_GPIO->BSRRL = STATUS_LED_PIN; } while (0);
#define TESTA1_LOW	do { } while (0);
#define TESTA1_HIGH	do { } while (0);
#define TESTA2_LOW	do { } while (0);
#define TESTA2_HIGH	do { } while (0);

#endif
