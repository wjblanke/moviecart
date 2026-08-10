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
#define SDIO_TRANSFER_CLK_DIV		((uint8_t)0x04)

/* Cart selected in upper 2K of slot space: A12 and A11 high (matches dsPIC CLC). */
#define CART_ADDR_MASK			0x1800u
#define CART_ADDR_SELECT		0x1800u

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
