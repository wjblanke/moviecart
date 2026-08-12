/**
 * MovieCart on DevEBox STM32F407VGT6
 *
 * The Atari is served by UnoCart's polling loop with interrupts off for the
 * entire run. That loop is the only thing on this board fast enough to hit the
 * 6507's data-valid deadline.
 *
 * SD field streaming coexists by yielding back into bus_serve_cycle() from every
 * SDIO command wait, DMA completion poll, Delayms replacement, FatFs cluster
 * hop, and updateBuffer byte batch — so main never abandons the bus for more
 * than a handful of instructions at a time.
 */
/* make NO_SD=1 builds the title-only baseline: the known-good reference. */
#ifndef MOVIECART_ENABLE_SD
#define MOVIECART_ENABLE_SD 1
#endif

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
#include "moviecart_yield.h"
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

/* Main-thread drain while waiting — lowest latency during display. */
static void
waitEndFrame(void)
{
	while (!r_coreInfo.mr_endFrame)
		bus_serve_cycle();
	r_coreInfo.mr_endFrame = 0;
}

/*
 * Blink by counting the kernel's own frames, not wall-clock milliseconds.
 *
 * Blinks used to be timed by moviecart_delay_ms, i.e. served through
 * moviecart_bus_pump. That is the right shape for an SD wait, but flash_led(1) is
 * 600 ms — roughly 700,000 cart cycles — and the pump pays per-cycle bookkeeping:
 * the pump_tick counter inside the drive window, the dead-bus guard state after
 * it. At that volume the small per-cycle cost accumulates into missed fetches and
 * kills the picture. SD_STAGE=1 proved it: no SD code runs in that build at all,
 * yet it went black immediately after its single blink, while the NO_SD baseline
 * (same code minus the blink) is pixel-perfect.
 *
 * waitEndFrame is the loop the baseline runs — one SRAM load between serves and
 * nothing else — so timing blinks off it costs the display nothing. At 60 Hz,
 * 9 frames is ~150 ms.
 *
 * The trade-off: a blink now needs a running kernel, so a fully dead console
 * shows no LED. Bounding the wait is what the dead-bus guard was for, and that
 * guard is repeatedly what breaks the display. A diagnostic that damages the
 * system it is measuring is worse than no diagnostic.
 */
#define LED_FRAMES_ON	9u	/* ~150 ms at 60 Hz */
#define LED_FRAMES_GAP	18u	/* ~300 ms */

static void
led_wait_frames(unsigned frames)
{
	while (frames--)
		waitEndFrame();
}

/*
 * Wall-clock blink timing, for the codes that report a *failure*.
 *
 * Frame-timed blinks are only calibrated while the kernel is healthy, which is
 * precisely when we do not need them. When the section machine desyncs,
 * mr_endFrame stops meaning "1/60 s" and starts firing spuriously fast, so a
 * frame-timed code compresses into an uncountable flicker — reported from
 * hardware as "blinking too rapidly to tell what they are reporting". A
 * diagnostic that becomes unreadable in the failure case is no diagnostic.
 *
 * DWT is independent of the kernel, so the code stays countable no matter how
 * badly the display has desynced. The catch that made blinks dangerous before was
 * never the clock source, it was *what runs between serves*: reading a counter
 * after every serve is per-cycle work, and over the ~700k cycles of a blink that
 * is enough to jam the 6507. So the deadline is sampled once per LED_CHECK_EVERY
 * serves, leaving the hot loop byte-identical to the baseline's — the same
 * arrangement wait_title_sync already uses successfully.
 */
#define LED_CHECK_EVERY	256u		/* ~214 us between samples */

static void
led_wait_ms(uint32_t ms)
{
	uint32_t start = DWT->CYCCNT;
	uint32_t limit = (SystemCoreClock / 1000u) * ms;

	do {
		for (unsigned i = 0; i < LED_CHECK_EVERY; i++)
			bus_serve_cycle();
	} while ((DWT->CYCCNT - start) < limit);
}

/*
 * Deliberately slow and lopsided so a code can be counted by eye: a long "here
 * comes a code" lead-in, then clearly separated flashes.
 */
static void
flash_led_slow(uint8_t num)
{
	TESTA0_HIGH;
	led_wait_ms(1200);
	for (uint8_t i = 0; i < num; i++) {
		TESTA0_LOW;
		led_wait_ms(300);
		TESTA0_HIGH;
		led_wait_ms(400);
	}
	TESTA0_HIGH;
	led_wait_ms(1200);
}

static void
flash_led(uint8_t num)
{
	TESTA0_HIGH;
	for (uint8_t i = 0; i < num; i++) {
		TESTA0_LOW;
		led_wait_frames(LED_FRAMES_ON);
		TESTA0_HIGH;
		led_wait_frames(LED_FRAMES_ON);
	}
	TESTA0_HIGH;
	led_wait_frames(LED_FRAMES_GAP);
}

#if MOVIECART_GAP_PROBE
/*
 * Report the worst serve-to-serve gap, then run the plain loop.
 *
 * Two blink groups, separated by a long pause:
 *   first  = region, 1..8 (see MC_PHASE_* in bus_service.h): 1-5 are one-time
 *            init, 6-8 are the per-sector read path (6 command/arm, 7 DMA drain,
 *            8 card-ready poll). 9 would mean no region was ever marked, i.e.
 *            distrust the reading rather than silently blaming region 1.
 *   second = gap size in whole Atari cycles (1 = under one cycle, i.e. fine),
 *            capped at 9
 *
 * The values are snapshotted before blinking, because blinking serves the bus
 * and would otherwise overwrite what we are trying to read out. Blinks are
 * wall-clock timed so the report stays countable even if the kernel is desynced.
 */
void
mc_probe_report(void)
{
	uint8_t phase = mc_gap_worst_phase;
	uint32_t cycles = mc_gap_max_cycles;
	uint32_t per_atari = SystemCoreClock / 1190000u;	/* ~141 @ 168 MHz */
	uint8_t bars = (uint8_t)(cycles / per_atari) + 1u;

	if (bars > 9u)
		bars = 9u;

	/* Wall-clock timed: a probe report is worthless if a desynced kernel
	 * compresses it into an unreadable flicker. */
	led_wait_ms(1500);
	flash_led_slow(phase ? phase : 9u);
	led_wait_ms(1500);
	flash_led_slow(bars);

	emulate_cartridge();
}
#endif

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
	while (size > 0) {
		*dst++ = *src++;
		size--;
		if (!(size & 7))
			moviecart_bus_yield();
	}
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
/*
 * Do not "optimise" the cycle-counter read out of this loop. It looks like the
 * same after-serve gap that had to be removed from the SD waits, and it was
 * changed twice on that reasoning — moving the deadline into the pump's free
 * window, then sampling it once per 4096 cycles. Both builds were worse on
 * hardware than this one: the first jammed every boot (the pump is the bounded
 * serve, and its dead-bus hint latches during sync, when the guard expiring is
 * normal), and the second lost the picture before the first blink. Sync is not
 * the accumulating-gap case the SD waits were — it lasts one frame, not 100k
 * cycles — and this exact shape is what reliably reaches the first blink.
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
	return ok;
}

#if MOVIECART_STALL_TEST && MOVIECART_STALL_TEST != 3
/*
 * Stall the bus loop for a known time, serving nothing. This is the thing we are
 * measuring: exactly what an SDIO command wait or a FatFs cluster walk does to
 * the Atari. DWT keeps counting with interrupts masked.
 *
 * The polling loop costs a handful of cycles per iteration, so the shortest steps
 * of the fine schedule overshoot their nominal figure somewhat. That only makes
 * the result conservative: a step that passes really did stall at least that long.
 */
static void
stall_ns(uint32_t ns)
{
	uint32_t start = DWT->CYCCNT;
	uint32_t limit = ns * (SystemCoreClock / 1000000u) / 1000u;

	while ((DWT->CYCCNT - start) < limit)
		;
}

/*
 * Sweep the stall length upward, announcing each step on the status LED, and let
 * the display be the verdict. The LED is dark for the whole of each test window,
 * so the pattern reads as: N flashes, dark test window, N+1 flashes, ...
 *
 * Step 1 stalls for nothing and must look identical to the normal title build; it
 * is the control. The first step whose window garbles the picture bounds the
 * budget, and the previous step is what any SD restructuring has to fit inside.
 *
 * STALL_TEST=1 sweeps microseconds, to find out whether whole missed Atari cycles
 * are survivable. STALL_TEST=2 sweeps the sub-cycle region, to find out how much
 * of a single 838 ns cycle is actually slack — that is what bounds the size of a
 * cooperative work step, and it also re-tests the old "settle window" result
 * against a proper 0 ns control.
 */
#define STALL_TEST_FRAMES	300u	/* ~5 s per step; the failure is intermittent */

static __attribute__((noreturn)) void
run_stall_sweep(void)
{
#if MOVIECART_STALL_TEST == 2
	static const uint16_t steps_ns[] = { 0, 100, 200, 300, 400, 600, 800, 1000 };
#else
	static const uint16_t steps_ns[] = {
		0, 1000, 2000, 4000, 8000, 16000, 32000, 64000
	};
#endif

	__disable_irq();

	/* Lock onto a complete frame first, as the title build does. */
	while (!r_coreInfo.mr_endFrame)
		bus_serve_cycle();
	r_coreInfo.mr_endFrame = 0;

	for (;;) {
		for (unsigned s = 0; s < sizeof(steps_ns) / sizeof(steps_ns[0]); s++) {
			flash_led((uint8_t)(s + 1));
			led_wait_frames(42);	/* countable gap; bus stays served */

			for (unsigned f = 0; f < STALL_TEST_FRAMES; f++) {
				waitEndFrame();
				if (steps_ns[s])
					stall_ns(steps_ns[s]);
			}
		}
	}
}
#elif MOVIECART_STALL_TEST == 3
/*
 * Interleave proof: one 100 ns cooperative step every cart cycle, injected in
 * bus_service.c after SET_DATA_MODE_OUT. Three announcement flashes, then the
 * kernel's normal 1-flash-per-second diagnostic owns the LED. Clean title +
 * that heartbeat means cooperative SD stepping is viable; garble means the
 * end-of-frame slack does not transfer to mid-cycle work.
 */
static __attribute__((noreturn)) void
run_interleave_proof(void)
{
	__disable_irq();

	flash_led(3);
	led_wait_frames(42);

	emulate_cartridge();
}
#endif /* MOVIECART_STALL_TEST */

static __attribute__((noreturn)) void
fatalBlink(uint8_t code)
{
	__disable_irq();
	for (;;)
		flash_led_slow(code);	/* wall-clock: readable even with a desynced kernel */
}

/*
 * Boot progress on the status LED. Every blink keeps serving the bus, so these
 * are free to leave in: they are the only way to tell how far boot got when the
 * picture itself is unusable.
 *
 *   1  title synchronized    5  mount failed
 *   2  card mounted          6  title never synchronized
 *   3  movie file opened     7  bad field header or size
 *   4  first field valid     8  no playable file found
 *
 * MOVIECART_SD_STAGE stops after a chosen milestone and hands the bus back to
 * the plain title loop, so a failure can be bisected one step at a time instead
 * of guessing which layer starved the bus.
 */
static void
setupDisk(void)
{
#if MOVIECART_SD_STAGE == 1
	emulate_cartridge();
#endif
	if (!pf_mount())
		fatalBlink(5);
	flash_led(2);

#if MOVIECART_SD_STAGE == 2
#if MOVIECART_GAP_PROBE
	/* pf_mount has now read the MBR/boot sector/FAT, so the worst gap has
	 * seen the real per-sector read path (regions 6-8), not just init. */
	mc_probe_report();
#else
	emulate_cartridge();
#endif
#endif
	state.io_frameNumber = 1;
	state.io_bits &= ~STATE_PLAYING;
	if (!pf_open_file(&state.i_numFrames, 1))
		fatalBlink(8);
	flash_led(3);

#if MOVIECART_SD_STAGE == 3
	emulate_cartridge();
#endif
}

static bool
prepareNextFrame(void)
{
	struct frameInfo *fInfo;

	if (r_coreInfo.mr_bufferIndex)
		fInfo = &r_coreInfo.mr_frameInfo1;
	else
		fInfo = &r_coreInfo.mr_frameInfo2;

	uint8_t *dst = fInfo->buffer;
	uint32_t offset = (uint32_t)(state.io_frameNumber * FIELD_NUM_BLOCKS);

	pf_seek_block(offset);

	pf_read_block(dst);
	dst += 512;

	/* Every field starts with "MVC\0". Reject a bad sector/header before its
	 * geometry can turn into an out-of-bounds DMA destination below. */
	if (memcmp(fInfo->buffer, "MVC\0", 4) != 0)
		return false;

	frameInit(fInfo);
	if (!fInfo->visibleLines || !fInfo->numBlocks ||
	    fInfo->numBlocks > FIELD_MAX_BLOCKS)
		return false;

	int nb = fInfo->numBlocks - 1;
	while (nb) {
		pf_read_block(dst);
		dst += 512;
		nb--;
	}

	updateBuffer(&state, fInfo);
	return true;
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

	pf_seek_block(offset);

	waitEndFrame();
	if (!r_coreInfo.mr_bufferIndex)
		waitEndFrame();

	/*
	 * Probing the file's geometry needs a whole sector of scratch, and the only
	 * spare 512 bytes is a live display buffer — so this deliberately reads on
	 * top of the title's colour data and restores it immediately below.
	 */
	struct frameInfo fInfo;
	fInfo.buffer = r_coreInfo.mr_frameInfo1.colorBuf;
	pf_read_block(fInfo.buffer);

	if (memcmp(fInfo.buffer, "MVC\0", 4) != 0)
		fatalBlink(7);

	frameInit(&fInfo);
	if (!fInfo.visibleLines || !fInfo.numBlocks ||
	    fInfo.numBlocks > FIELD_MAX_BLOCKS)
		fatalBlink(7);
	uint8_t fileVis = fInfo.visibleLines;

	flash_led(4);

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

/*
 * Zeroing both field buffers is several kilobytes of stores. Done in one memset
 * it silences the cart for tens of microseconds mid-playback, so clear it in
 * small chunks and serve a cycle between each.
 */
static void
clearFieldBuffers(void)
{
	const uint32_t chunk = 64;

	for (uint32_t off = 0; off < sizeof(mr_buffer1); off += chunk) {
		uint32_t n = sizeof(mr_buffer1) - off;

		if (n > chunk)
			n = chunk;
		memset(&mr_buffer1[off], 0, n);
		memset(&mr_buffer2[off], 0, n);
		moviecart_bus_yield();
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
			if (!(i & 7))
				moviecart_bus_yield();
		}

		state.io_frameNumber = 1;
		state.io_bits |= STATE_PLAYING;

		clearFieldBuffers();

		for (int i = 0; i < 30; i++)
			waitEndFrame();
	}

	coreInfoToState();
}

static void
runFrameLoop(void)
{
	int which = 1;

	state.io_frameNumber = 1;
	state.io_bits = STATE_PLAYING;

	while (1) {
		waitEndFrame();

		/*
		 * No status LED here. PA1 belongs to the kernel's end-of-frame
		 * diagnostic; a once-per-frame write from this loop as well made
		 * the blink count unreadable, which is the only channel out.
		 */
		checkSelectVideo(&which);
		updateTransport(&state);
		if (!prepareNextFrame())
			fatalBlink(7);
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

#if MOVIECART_STALL_TEST == 3
	run_interleave_proof();
#elif MOVIECART_STALL_TEST
	run_stall_sweep();
#elif MOVIECART_ENABLE_SD
	MC_PROBE(MC_PHASE_PRE_SD);
	if (!wait_title_sync(3000u))
		fatalBlink(6);
	flash_led(1);

	setupDisk();

	updateInit();
	runTitle();
	runFrameLoop();
#else
	emulate_cartridge();
#endif
}
