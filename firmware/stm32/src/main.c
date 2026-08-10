/**
 * MovieCart firmware for MCUDEV DevEBox STM32F407VGT6
 *
 * Wiring (same as UnoCart-2600 DevEBox fork):
 *   A0–A12 → PE0–PE12
 *   D0–D7  → PD8–PD15
 *   microSD → SDIO (PC8–PC12, PD2)
 */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "stm32f4xx.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_gpio.h"
#include "stm32f4xx_exti.h"
#include "stm32f4xx_syscfg.h"
#include "misc.h"
#include "tm_stm32f4_delay.h"

#include "defines.h"
#include "cartridge_io.h"
#include "bus_service.h"
#include "core.h"
#include "pff.h"
#include "update.h"
#include "title_data.h"
#include "sd_reader.h"

extern struct coreInfo r_coreInfo;
extern struct fileSystemInfo fsInfo;
extern struct queueInfo qinfo;

struct stateVars state;
uint8_t mr_buffer1[FIELD_SIZE] __attribute__((aligned(4)));
uint8_t mr_buffer2[FIELD_SIZE] __attribute__((aligned(4)));

void updateInit(void);

static void
flash_led(uint8_t num)
{
	for (uint8_t i = 0; i < num; i++) {
		TESTA0_LOW;
		for (volatile uint32_t d = 0; d < 800000; d++)
			bus_service();
		TESTA0_HIGH;
		for (volatile uint32_t d = 0; d < 800000; d++)
			bus_service();
	}
	if (!num) {
		for (volatile uint32_t d = 0; d < 2000000; d++)
			bus_service();
	}
	for (volatile uint32_t d = 0; d < 2000000; d++)
		bus_service();
}

static void
config_gpio_data(void)
{
	GPIO_InitTypeDef gpio;

	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOD, ENABLE);

	gpio.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_11 |
			GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_14 | GPIO_Pin_15;
	gpio.GPIO_Mode = GPIO_Mode_IN;
	gpio.GPIO_OType = GPIO_OType_PP;
	gpio.GPIO_Speed = GPIO_Speed_25MHz;
	gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;
	GPIO_Init(GPIOD, &gpio);
}

static void
config_gpio_addr(void)
{
	GPIO_InitTypeDef gpio;

	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOE, ENABLE);

	gpio.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2 | GPIO_Pin_3 |
			GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7 |
			GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_11 |
			GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_14 | GPIO_Pin_15;
	gpio.GPIO_Mode = GPIO_Mode_IN;
	gpio.GPIO_OType = GPIO_OType_PP;
	gpio.GPIO_Speed = GPIO_Speed_100MHz;
	gpio.GPIO_PuPd = GPIO_PuPd_DOWN;
	GPIO_Init(GPIOE, &gpio);
}

static void
config_status_led(void)
{
	GPIO_InitTypeDef gpio;

	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);

	gpio.GPIO_Pin = STATUS_LED_PIN;
	gpio.GPIO_Mode = GPIO_Mode_OUT;
	gpio.GPIO_OType = GPIO_OType_PP;
	gpio.GPIO_Speed = GPIO_Speed_2MHz;
	gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;
	GPIO_Init(STATUS_LED_GPIO, &gpio);
	TESTA0_HIGH;
}

/*
 * A12 (PE12) rising edge preempts SD/main work and serves the cart window
 * until A11+A12 drop — required because SDIO block reads exceed a 6502 cycle.
 */
static void
config_exti_a12(void)
{
	EXTI_InitTypeDef exti;
	NVIC_InitTypeDef nvic;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);
	SYSCFG_EXTILineConfig(EXTI_PortSourceGPIOE, EXTI_PinSource12);

	exti.EXTI_Line = EXTI_Line12;
	exti.EXTI_Mode = EXTI_Mode_Interrupt;
	exti.EXTI_Trigger = EXTI_Trigger_Rising;
	exti.EXTI_LineCmd = ENABLE;
	EXTI_Init(&exti);

	nvic.NVIC_IRQChannel = EXTI15_10_IRQn;
	nvic.NVIC_IRQChannelPreemptionPriority = 0;
	nvic.NVIC_IRQChannelSubPriority = 0;
	nvic.NVIC_IRQChannelCmd = ENABLE;
	NVIC_Init(&nvic);
}

void
EXTI15_10_IRQHandler(void)
{
	if (EXTI_GetITStatus(EXTI_Line12) != RESET) {
		EXTI_ClearITPendingBit(EXTI_Line12);
		while ((ADDR_IN & CART_ADDR_MASK) == CART_ADDR_SELECT)
			bus_service();
		SET_DATA_MODE_IN;
	}
}

static void
copy_title(uint8_t *dst, const uint8_t *src, int size)
{
	memcpy(dst, src, (size_t)size);
}

static void
setupTitle(void)
{
	r_coreInfo.frameInfo.buffer = mr_buffer1;
	r_coreInfo.mr_frameInfo1.buffer = mr_buffer1;
	r_coreInfo.mr_frameInfo2.buffer = mr_buffer2;

	frameInitTitle(&r_coreInfo.frameInfo, 0);
	frameInitTitle(&r_coreInfo.mr_frameInfo1, 0);
	frameInitTitle(&r_coreInfo.mr_frameInfo2, 1);

	memset(mr_buffer1, 0, sizeof(mr_buffer1));
	memset(mr_buffer2, 0, sizeof(mr_buffer2));

	int lineTotal = r_coreInfo.mr_frameInfo1.visibleLines * 5;

	copy_title(r_coreInfo.mr_frameInfo1.graphBuf, TitleGraph1, lineTotal);
	copy_title(r_coreInfo.mr_frameInfo1.colorBuf, TitleColor1, lineTotal);
	copy_title(r_coreInfo.mr_frameInfo1.colorBKBuf, TitleBackColor1,
		   r_coreInfo.mr_frameInfo1.visibleLines);

	copy_title(r_coreInfo.mr_frameInfo2.graphBuf, TitleGraph2, lineTotal);
	copy_title(r_coreInfo.mr_frameInfo2.colorBuf, TitleColor2, lineTotal);
	copy_title(r_coreInfo.mr_frameInfo2.colorBKBuf, TitleBackColor2,
		   r_coreInfo.mr_frameInfo2.visibleLines);
}

static void
setupDisk(void)
{
	while (!pf_mount()) {
		flash_led(4);
	}

	state.io_frameNumber = 1;
	state.io_bits &= ~STATE_PLAYING;
	while (!pf_open_file(&state.i_numFrames, 1)) {
		flash_led(3);
	}
}

static void
prepareNextFrame(void)
{
	struct frameInfo *fInfo;

	if (r_coreInfo.mr_bufferIndex)
		fInfo = &r_coreInfo.mr_frameInfo1;
	else
		fInfo = &r_coreInfo.mr_frameInfo2;

	uint8_t *dst = fInfo->buffer;
	uint32_t offset = (uint32_t)(state.io_frameNumber * FIELD_NUM_BLOCKS);

	while (!pf_seek_block(offset)) {
		flash_led(4);
	}

	pf_read_block(dst);
	dst += 512;
	frameInit(fInfo);

	int nb = fInfo->numBlocks - 1;
	while (nb) {
		pf_read_block(dst);
		dst += 512;
		nb--;
		bus_service();
	}

	updateBuffer(&state, fInfo);
}

static void
waitEndFrame(void)
{
	while (!r_coreInfo.mr_endFrame)
		bus_service();
	r_coreInfo.mr_endFrame = 0;
}

static void
coreInfoToState(void)
{
	state.i_swcha = r_coreInfo.mr_swcha;
	state.i_swchb = r_coreInfo.mr_swchb;
	state.i_inpt4 = r_coreInfo.mr_inpt4;
	state.i_inpt5 = r_coreInfo.mr_inpt5;

	state.i_swcha = ((state.i_swcha << 4) | (state.i_swcha & 0x0f)) & state.i_swcha;
	state.i_inpt4 &= r_coreInfo.mr_inpt5;
}

static void
runTitle(void)
{
	uint16_t m_titleFrame = 300;

	state.io_frameNumber = 1;
	uint32_t offset = (uint32_t)(state.io_frameNumber * FIELD_NUM_BLOCKS);

	while (!pf_seek_block(offset)) {
		flash_led(2);
		flash_led(3);
	}

	waitEndFrame();
	if (!r_coreInfo.mr_bufferIndex)
		waitEndFrame();

	struct frameInfo fInfo;
	fInfo.buffer = r_coreInfo.mr_frameInfo1.colorBuf;
	pf_read_block(fInfo.buffer);

	frameInit(&fInfo);
	uint8_t fileVis = fInfo.visibleLines;

	copy_title(r_coreInfo.mr_frameInfo1.colorBuf, TitleColor1, 512);

	int diff = (int)fileVis - (int)r_coreInfo.mr_frameInfo1.visibleLines;

	r_coreInfo.mr_frameInfo1.visibleLines += diff;
	r_coreInfo.mr_frameInfo2.visibleLines += diff;
	r_coreInfo.mr_frameInfo1.totalLines += diff;
	r_coreInfo.mr_frameInfo2.totalLines += diff;

	while (m_titleFrame--) {
		waitEndFrame();
		coreInfoToState();
		updateTransport(&state);
		if (state.io_bits & STATE_PLAYING)
			break;
	}
}

static void
checkSelectVideo(int *which)
{
	if ((state.io_bits & STATE_END) ||
	    ((state.i_swchb & 0x02) && !(r_coreInfo.mr_swchb & 0x02))) {
		state.io_bits &= ~STATE_END;

		(*which)++;
		while (!pf_open_file(&state.i_numFrames, *which)) {
			*which = 1;
		}

		qinfo.head = 0;
		for (int i = 0; i < QUEUE_SIZE; i++) {
			qinfo.block[i] = 0xffffffffu;
			qinfo.clust[i] = 0xffffffffu;
		}

		state.io_frameNumber = 1;
		state.io_bits |= STATE_PLAYING;

		memset(mr_buffer1, 0, sizeof(mr_buffer1));
		memset(mr_buffer2, 0, sizeof(mr_buffer2));

		for (int i = 0; i < 30; i++)
			waitEndFrame();
	}

	coreInfoToState();
}

static void
runFrameLoop(void)
{
	uint_fast8_t t = 0;
	int which = 1;

	state.io_frameNumber = 1;
	state.io_bits = STATE_PLAYING;

	while (1) {
		waitEndFrame();

		t++;
		if (t == 60) {
			TESTA0_LOW;
			t = 0;
		} else {
			TESTA0_HIGH;
		}

		checkSelectVideo(&which);
		updateTransport(&state);
		prepareNextFrame();
	}
}

int
main(void)
{
	SystemInit();
	TM_DELAY_Init();

	config_gpio_addr();
	config_gpio_data();
	config_status_led();
	config_exti_a12();

	coreInit();
	SET_DATA_MODE_IN;

	setupTitle();
	setupDisk();
	updateInit();
	runTitle();
	runFrameLoop();

	return 0;
}
