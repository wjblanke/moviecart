#ifndef HOST_RCC_H
#define HOST_RCC_H
#include "stm32f4xx.h"
#define RCC_AHB1Periph_GPIOA 0x01u
#define RCC_AHB1Periph_GPIOB 0x02u
#define RCC_AHB1Periph_GPIOC 0x04u
#define RCC_AHB1Periph_GPIOD 0x08u
#define RCC_AHB1Periph_GPIOE 0x10u
void RCC_AHB1PeriphClockCmd(uint32_t periph, FunctionalState state);
#endif
