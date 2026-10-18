/* Host stand-in for the STM32F4 device header: just enough for the firmware
 * sources to compile on macOS. Registers are plain memory. */
#ifndef HOST_STM32F4XX_H
#define HOST_STM32F4XX_H
#include <stdint.h>

typedef enum { DISABLE = 0, ENABLE = 1 } FunctionalState;

typedef struct {
	volatile uint32_t MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR;
	volatile uint16_t BSRRL, BSRRH;
	volatile uint32_t LCKR, AFR[2];
} GPIO_TypeDef;
extern GPIO_TypeDef host_gpio[5];
#define GPIOA (&host_gpio[0])
#define GPIOB (&host_gpio[1])
#define GPIOC (&host_gpio[2])
#define GPIOD (&host_gpio[3])
#define GPIOE (&host_gpio[4])

typedef struct { volatile uint32_t CTRL, CYCCNT; } DWT_Type;
extern DWT_Type host_dwt;
#define DWT (&host_dwt)
typedef struct { volatile uint32_t DEMCR; } CoreDebug_Type;
extern CoreDebug_Type host_coredebug;
#define CoreDebug (&host_coredebug)
#define CoreDebug_DEMCR_TRCENA_Msk	(1u << 24)
#define DWT_CTRL_CYCCNTENA_Msk		1u

extern uint32_t SystemCoreClock;
#endif
