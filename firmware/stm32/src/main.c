/**
 * MovieCart on DevEBox STM32F407VGT6
 *
 * Title-only validation build. The bus is served by the exact polling pattern
 * from UnoCart-2600's proven driver_4k.c, continuously in main with IRQs off.
 * SD is intentionally disabled until the title display works.
 */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "stm32f4xx.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_gpio.h"
#include "misc.h"

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
dwt_init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* Blink without ever abandoning the UnoCart bus loop. */
static void
led_wait_serving(uint32_t ms)
{
	uint32_t start = DWT->CYCCNT;
	uint32_t limit = (SystemCoreClock / 1000u) * ms;

	while ((DWT->CYCCNT - start) < limit)
		bus_serve_cycle();
}

static void
flash_led(uint8_t num)
{
	TESTA0_HIGH;
	for (uint8_t i = 0; i < num; i++) {
		TESTA0_LOW;
		led_wait_serving(150);
		TESTA0_HIGH;
		led_wait_serving(150);
	}
	TESTA0_HIGH;
	led_wait_serving(300);
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
	/*
	 * Identical to UnoCart's config_gpio_data(), which is proven on this
	 * board: no pull, and GPIO_Speed is deliberately left ineffective —
	 * GPIO_Init only programs OSPEEDR for OUT/AF modes, so these pins keep
	 * the 2 MHz reset-default slew even when flipped to output at runtime.
	 * Forcing 100 MHz here rings and cross-talks on breadboard wiring.
	 */
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

static void
copy_title(uint8_t *dst, const uint8_t *src, int size)
{
	memcpy(dst, src, (size_t)size);
}

static void
setupTitleBuffers(void)
{
	r_coreInfo.frameInfo.buffer = mr_buffer1;
	r_coreInfo.mr_frameInfo1.buffer = mr_buffer1;
	r_coreInfo.mr_frameInfo2.buffer = mr_buffer2;

	frameInitTitle(&r_coreInfo.frameInfo, 0);
	frameInitTitle(&r_coreInfo.mr_frameInfo1, 0);
	frameInitTitle(&r_coreInfo.mr_frameInfo2, 1);

	memset(mr_buffer1, 0, sizeof(mr_buffer1));
	memset(mr_buffer2, 0, sizeof(mr_buffer2));

	r_coreInfo.frameInfo = r_coreInfo.mr_frameInfo1;
}

static void
loadTitlePixels(void)
{
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

/*
 * Main has nothing to do here, so it becomes the (faster) bus server:
 * IRQs masked, tight drain — ~100-150 ns response vs ~300+ ns via EXTI.
 * DWT keeps time (it counts with IRQs masked).
 */
static int
wait_title_sync(uint32_t timeout_ms)
{
	uint32_t start = DWT->CYCCNT;
	uint32_t limit = (SystemCoreClock / 1000u) * timeout_ms;
	int ok = 0;

	r_coreInfo.mr_endFrame = 0;

	__disable_irq();
	while ((DWT->CYCCNT - start) < limit) {
		bus_serve_cycle();
		if (r_coreInfo.mr_endFrame) {
			r_coreInfo.mr_endFrame = 0;
			ok = 1;
			break;
		}
	}
	__enable_irq();
	return ok;
}

/* Main-thread drain while waiting — lowest latency during display. */
static void
waitEndFrame(void)
{
	__disable_irq();
	while (!r_coreInfo.mr_endFrame)
		bus_serve_cycle();
	r_coreInfo.mr_endFrame = 0;
	__enable_irq();
}

static void
setupDisk(void)
{
	flash_led(5);

	while (!pf_mount())
		flash_led(4);

	state.io_frameNumber = 1;
	state.io_bits &= ~STATE_PLAYING;
	while (!pf_open_file(&state.i_numFrames, 1))
		flash_led(3);

	flash_led(2);
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

	while (!pf_seek_block(offset))
		flash_led(4);

	pf_read_block(dst);
	dst += 512;
	frameInit(fInfo);

	int nb = fInfo->numBlocks - 1;
	while (nb) {
		pf_read_block(dst);
		dst += 512;
		nb--;
	}

	updateBuffer(&state, fInfo);
}

static void
coreInfoToState(void)
{
	state.i_swcha = (uint8_t)r_coreInfo.mr_swcha;
	state.i_swchb = (uint8_t)r_coreInfo.mr_swchb;
	state.i_inpt4 = (uint8_t)r_coreInfo.mr_inpt4;
	state.i_inpt5 = (uint8_t)r_coreInfo.mr_inpt5;

	state.i_swcha = ((state.i_swcha << 4) | (state.i_swcha & 0x0f)) & state.i_swcha;
	state.i_inpt4 &= state.i_inpt5;
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
	    ((state.i_swchb & 0x02) && !((uint8_t)r_coreInfo.mr_swchb & 0x02))) {
		state.io_bits &= ~STATE_END;

		(*which)++;
		while (!pf_open_file(&state.i_numFrames, *which))
			*which = 1;

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
	config_gpio_data();
	config_gpio_addr();
	config_status_led();
	dwt_init();

	/* Everything the kernel dispatch touches must be valid before serving
	 * starts, but nothing slow may run before the drain: the console's RC
	 * reset releases the 6507 a few tens of ms after power-on, and its
	 * very first vector fetches decide whether it lives or jams. */
	coreInit();
	setupTitleBuffers();
	loadTitlePixels();

	/*
	 * Enter UnoCart's infinite IRQ-disabled bus loop verbatim. No LEDs,
	 * timeout, frame checks, EXTI, or SD code execute beyond this point.
	 */
	emulate_cartridge();
}
