#ifndef HOST_GPIO_H
#define HOST_GPIO_H
#include "stm32f4xx.h"
typedef enum { GPIO_Mode_IN = 0, GPIO_Mode_OUT = 1, GPIO_Mode_AF = 2, GPIO_Mode_AN = 3 } GPIOMode_TypeDef;
typedef enum { GPIO_OType_PP = 0, GPIO_OType_OD = 1 } GPIOOType_TypeDef;
typedef enum { GPIO_Speed_2MHz = 0, GPIO_Speed_25MHz = 1, GPIO_Speed_50MHz = 2, GPIO_Speed_100MHz = 3 } GPIOSpeed_TypeDef;
typedef enum { GPIO_PuPd_NOPULL = 0, GPIO_PuPd_UP = 1, GPIO_PuPd_DOWN = 2 } GPIOPuPd_TypeDef;
typedef struct {
	uint32_t GPIO_Pin;
	GPIOMode_TypeDef GPIO_Mode;
	GPIOSpeed_TypeDef GPIO_Speed;
	GPIOOType_TypeDef GPIO_OType;
	GPIOPuPd_TypeDef GPIO_PuPd;
} GPIO_InitTypeDef;
#define GPIO_Pin_0  0x0001u
#define GPIO_Pin_1  0x0002u
#define GPIO_Pin_2  0x0004u
#define GPIO_Pin_3  0x0008u
#define GPIO_Pin_4  0x0010u
#define GPIO_Pin_5  0x0020u
#define GPIO_Pin_6  0x0040u
#define GPIO_Pin_7  0x0080u
#define GPIO_Pin_8  0x0100u
#define GPIO_Pin_9  0x0200u
#define GPIO_Pin_10 0x0400u
#define GPIO_Pin_11 0x0800u
#define GPIO_Pin_12 0x1000u
#define GPIO_Pin_13 0x2000u
#define GPIO_Pin_14 0x4000u
#define GPIO_Pin_15 0x8000u
void GPIO_Init(GPIO_TypeDef *gpio, GPIO_InitTypeDef *init);
#endif
