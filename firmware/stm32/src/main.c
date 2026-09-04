/**
 * MovieCart on DevEBox STM32F407VGT6
 *
 * The Atari is served by UnoCart's polling loop. No IRQs: SDIO/DMA completion
 * is polled in the driver; every SD wait yields back into bus_serve_cycle()
 * so RamKernel keeps running on the 6502.
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
/*
 * Field buffers sit on opposite AHB SRAM slaves. Stack, r_coreInfo, and
 * romData live in CCM (D-bus only), so they never share a port with DMA
 * or GPIO. DMA writes the inactive buffer; display reads the other.
 */
uint8_t mr_buffer1[FIELD_SIZE] __attribute__((aligned(16), section(".sram1")));
uint8_t mr_buffer2[FIELD_SIZE] __attribute__((aligned(16), section(".sram2")));

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
		moviecart_bus_yield();
	r_coreInfo.mr_endFrame = 0;
}

/*
 * Block until VisibleBars ($F09D) is served — blanking in RIOT is done and the
 * cart-visible section of this frame is about to run.
 */
static void
waitVisibleBarsVended(void)
{
	mc_visible_bars_vended = 0;
	while (!mc_visible_bars_vended)
		bus_serve_cycle();
	mc_visible_bars_vended = 0;
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
 * badly the display has desynced. The deadline is sampled only on A12-low
 * (bus_serve_ms), so cart cycles stay identical to emulate_cartridge().
 */

static void
led_wait_ms(uint32_t ms)
{
	bus_serve_ms(ms);
}

/*
 * Deliberately slow and lopsided so a code can be counted by eye: a long "here
 * comes a code" lead-in, then clearly separated flashes.
 */
static void
flash_led_slow(uint8_t num)
{
	mc_led_host = 1;	/* keep the kernel heartbeat off this LED */
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
	/* mc_led_host stays set: a failure code repeats forever and must never
	 * be interleaved with the heartbeat between repeats. */
}

/*
 * Milestone codes are wall-clock timed too, for the same reason the failure
 * codes are: a milestone is reported at exactly the moment the thing it reports
 * on might have broken the kernel. Frame-timed, a blink that begins with the LED
 * lit and then loses the kernel never reaches its TESTA0_HIGH — waitEndFrame
 * spins forever on an mr_endFrame that has stopped arriving. That is read from
 * hardware as "two flashes with the LED remaining on", which hides whether the
 * next milestone was reached at all. DWT does not care whether the display is
 * alive, and led_wait_ms samples it once per 256 serves, so the hot loop stays
 * as cheap as the frame-timed version's.
 */
#define LED_MS_ON	150u
#define LED_MS_GAP	300u

static void
flash_led(uint8_t num)
{
#if MOVIECART_NO_LED
	(void)num;
#else
	mc_led_host = 1;	/* keep the kernel heartbeat off this LED */
	TESTA0_HIGH;
	for (uint8_t i = 0; i < num; i++) {
		TESTA0_LOW;
		led_wait_ms(LED_MS_ON);
		TESTA0_HIGH;
		led_wait_ms(LED_MS_ON);
	}
	TESTA0_HIGH;
	led_wait_ms(LED_MS_GAP);
	mc_led_host = 0;
#endif
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

static int
wait_title_sync(uint32_t timeout_ms)
{
	return bus_wait_endframe(timeout_ms);
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
	flash_led(3);
	led_wait_frames(42);

	emulate_cartridge();
}
#endif /* MOVIECART_STALL_TEST */

static __attribute__((noreturn)) void fatalBlink(uint8_t code);

static __attribute__((noreturn)) void
fatalBlink(uint8_t code)
{
	for (;;)
		flash_led_slow(code);	/* wall-clock: readable even with a desynced kernel */
}

/*
 * One specific finding gets a pattern instead of a count: a long 2 s on, short
 * off. It means the failed field read succeeded when repeated into the SRAM2
 * scratch buffer, i.e. the SRAM1 field buffer is the bad DMA target rather than a
 * wrong-sector computation. Counting flashes by eye is unreliable, and this result
 * is too important to risk misreading — it decides which layer to fix.
 */
static __attribute__((noreturn)) void
fatalStrobe(void)
{
	mc_led_host = 1;
	for (;;) {
		TESTA0_LOW;
		led_wait_ms(2000);
		TESTA0_HIGH;
		led_wait_ms(400);
	}
}

/*
 * Was the field readable into SRAM2 but not SRAM1?
 *
 * Re-reads the exact sector the failed field read used, this time into the SRAM2
 * scratch buffer (disk_read_block1). The raw read path is identical; only the DMA
 * destination region differs. The two outcomes need opposite fixes:
 *
 *   valid MVC\0 in scratch  -> the sector is right and the read works; the SRAM1
 *                              field buffer is a bad DMA target (contention or a
 *                              coherency gap against the per-cycle bus_dispatch).
 *   wrong in scratch too    -> we computed the wrong sector; the FAT walk / seek is
 *                              at fault, and the destination region is innocent.
 *
 * Caller must re-seek first so pf_current_sector() names the field's first sector.
 */
static int
field_reads_ok_in_scratch(uint32_t offset)
{
	pf_seek_block(offset);
	const uint8_t *scratch = disk_read_block1(pf_current_sector());
	return memcmp(scratch, "MVC\0", 4) == 0;
}

/*
 * Boot progress on the status LED. Every blink keeps serving the bus, so these
 * are free to leave in: they are the only way to tell how far boot got when the
 * picture itself is unusable.
 *
 *   1  title synchronized    5  mount failed
 *   2  card mounted          6  title never synchronized
 *   3  movie file opened     8  no playable file found
 *   4  first field valid
 *
 * Field faults (title probe and every playback frame) were all reported as a
 * single "7", which hid which of three different things went wrong. They are now
 * split, because they point at different layers:
 *
 *   7   field header wrong ("MVC\0" missing) — wrong bytes, right length
 *   9   field geometry invalid (visibleLines / numBlocks out of range)
 *   10  sector read failed after DISK_READ_RETRIES — the card/read path, not
 *       the data. Previously this hung silently on the last good frame; that is
 *       the readable-diagnostic rule, so it now has a code.
 *
 * One result is a *pattern* rather than a count — a long 2 s strobe means the same
 * sector read cleanly into the SRAM2 scratch buffer, so the SRAM1 field buffer is
 * the bad DMA target, not the sector computation. See fatalStrobe().
 *
 * MOVIECART_SD_STAGE stops after a chosen milestone and hands the bus back to
 * the plain title loop, so a failure can be bisected one step at a time instead
 * of guessing which layer starved the bus.
 */
#define BLINK_FIELD_HEADER	7u
#define BLINK_FIELD_GEOMETRY	9u
#define BLINK_SECTOR_READ	10u

static void stage_pulse(uint8_t n);

static bool disk_mount_ok;
static bool disk_open_ok;
static uint8_t playback_field_blocks;

static void
mount_work(void)
{
	disk_mount_ok = pf_mount();
}

static void
open_work(void)
{
	disk_open_ok = pf_open_file(&state.i_numFrames, 1);
}

static void
setupDisk(void)
{
#if MOVIECART_SD_STAGE == 1
	emulate_cartridge();
#endif
	/* Pins are already up. Yields wait for a fresh $F1C1 only in this
	 * function — not during title copy or the 2 s hold. */
	mc_sd_strict = 1;
	moviecart_wait_blanking_start();
	mount_work();
	if (!disk_mount_ok)
		fatalBlink(5);
#if MOVIECART_INIT_PHASE == 13
	emulate_cartridge();
#endif
	stage_pulse(2);

#if MOVIECART_SD_STAGE == 2
#if MOVIECART_GAP_PROBE
	mc_probe_report();
#else
	emulate_cartridge();
#endif
#endif
	state.io_frameNumber = 1;
	state.io_bits &= ~STATE_PLAYING;
	/* flash_led returns mid-visible; do not start the directory walk there. */
	moviecart_wait_blanking_start();
	open_work();
	if (!disk_open_ok)
		fatalBlink(8);
#if MOVIECART_INIT_PHASE == 14
	emulate_cartridge();
#endif
	moviecart_wait_blanking_start();
#if MOVIECART_INIT_PHASE == 151
	emulate_cartridge();
#endif
	stage_pulse(3);
	mc_sd_strict = 0;
	moviecart_wait_blanking_start();
#if MOVIECART_INIT_PHASE == 152
	emulate_cartridge();
#endif

#if MOVIECART_SD_STAGE == 3
	emulate_cartridge();
#endif
}

static void
stage_pulse(uint8_t n)
{
	moviecart_stage_blink(n);
}

#define FIELD_LOAD_ATTEMPTS	4

static struct frameInfo *pending_frame;
static uint32_t pending_offset;
static uint8_t pending_attempt;
static uint8_t field_dma_pending;

static struct frameInfo *
inactiveFrameInfo(void)
{
	return r_coreInfo.mr_bufferIndex
		? &r_coreInfo.mr_frameInfo1
		: &r_coreInfo.mr_frameInfo2;
}

/*
 * Start the field's one CMD18 transfer, but deliberately do not drain or poll
 * it here. DMA may finish while the other buffer is visible.
 */
static uint8_t
beginFieldRead(struct frameInfo *fInfo, uint32_t offset, uint8_t attempt)
{
	if (!playback_field_blocks)
		return BLINK_FIELD_GEOMETRY;

	mc_sdio_gate_relaxed = 1;
	pf_seek_block(offset);
	bool started = pf_read_blocks_begin(fInfo->buffer,
					    playback_field_blocks);
	mc_sdio_gate_relaxed = 0;

	if (!started)
		return BLINK_SECTOR_READ;

	pending_frame = fInfo;
	pending_offset = offset;
	pending_attempt = attempt;
	field_dma_pending = 1;
	return 0;
}

/*
 * Drain the transfer started in the preceding blanking interval. A failed read
 * is restarted into the same inactive buffer; it is never marked ready or
 * exposed to VisibleBars until both DMA completion and field validation pass.
 *
 * Return 0 when a buffer became ready, 1 when a retry was started, or an LED
 * fault code when all attempts failed.
 */
static uint8_t
finishFieldRead(void)
{
	mc_sdio_gate_relaxed = 1;
	bool read_ok = pf_read_blocks_finish();
	mc_sdio_gate_relaxed = 0;
	field_dma_pending = 0;

	uint8_t fault = 0;
	if (!read_ok) {
		fault = BLINK_SECTOR_READ;
	} else if (memcmp(pending_frame->buffer, "MVC\0", 4) != 0) {
		fault = BLINK_FIELD_HEADER;
	} else {
		frameInit(pending_frame);
		if (!pending_frame->visibleLines || !pending_frame->numBlocks ||
		    pending_frame->numBlocks > FIELD_MAX_BLOCKS ||
		    pending_frame->numBlocks != playback_field_blocks)
			fault = BLINK_FIELD_GEOMETRY;
	}

	if (!fault) {
		updateBuffer(&state, pending_frame);
		mc_field_swap_to_display();
		return 0;
	}

	if (++pending_attempt >= FIELD_LOAD_ATTEMPTS)
		return fault;

	mc_diag_note(DIAG_FIELD_RETRY);
	disk_read_invalidate();
	if (beginFieldRead(pending_frame, pending_offset, pending_attempt))
		return fault;
	return 1;
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

static uint32_t title_offset;
static uint8_t *title_dst;
static uint8_t title_probe_fault;	/* 0 ok, else BLINK_* or 0xff for strobe */

static void
title_probe_work(void)
{
	pf_seek_block(title_offset);
	if (!pf_read_block(title_dst)) {
		title_probe_fault = BLINK_SECTOR_READ;
		return;
	}
	if (memcmp(title_dst, "MVC\0", 4) != 0) {
		title_probe_fault = field_reads_ok_in_scratch(title_offset)
			? 0xffu : BLINK_FIELD_HEADER;
		return;
	}
	title_probe_fault = 0;
}

static void
runTitle(void)
{
	uint16_t m_titleFrame = 300;

	state.io_frameNumber = 1;
	title_offset = (uint32_t)(state.io_frameNumber * FIELD_NUM_BLOCKS);

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
	title_dst = fInfo.buffer;
	title_probe_work();
	if (title_probe_fault == 0xffu)
		fatalStrobe();
	if (title_probe_fault)
		fatalBlink(title_probe_fault);

	frameInit(&fInfo);
	if (!fInfo.visibleLines || !fInfo.numBlocks ||
	    fInfo.numBlocks > FIELD_MAX_BLOCKS)
		fatalBlink(BLINK_FIELD_GEOMETRY);
	playback_field_blocks = fInfo.numBlocks;
	uint8_t fileVis = fInfo.visibleLines;

	stage_pulse(4);

	copy_title(r_coreInfo.mr_frameInfo1.colorBuf, TitleColor1, 512);
#if MOVIECART_INIT_PHASE == 16
	emulate_cartridge();
#endif

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

	if (!mc_sd_prepare_playback_read())
		fatalBlink(BLINK_SECTOR_READ);
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

static int select_which;

static void
select_open_work(void)
{
	disk_open_ok = pf_open_file(&state.i_numFrames, select_which);
}

static bool
checkSelectVideo(int *which)
{
	bool selected = false;

	if ((state.io_bits & STATE_END) ||
	    ((state.i_swchb & 0x02) && !((uint8_t)r_coreInfo.mr_swchb & 0x02))) {
		selected = true;
		state.io_bits &= ~STATE_END;

		(*which)++;
		mc_sd_strict = 1;
		for (;;) {
			select_which = *which;
			select_open_work();
			if (disk_open_ok)
				break;
			*which = 1;
		}
		mc_sd_strict = 0;

		/* Drop cluster checkpoints from the previous file before seeking in
		 * the newly selected one. */
		qinfo.head = 0;
		for (int i = 0; i < QUEUE_SIZE; i++) {
			qinfo.block[i] = 0xffffffffu;
			qinfo.clust[i] = 0xffffffffu;
			if (!(i & 7))
				moviecart_bus_yield();
		}

		/* Cache the newly selected file's field size before its DMA pipeline
		 * starts; selected files may differ between NTSC and PAL geometry. */
		pf_seek_block(FIELD_NUM_BLOCKS);
		uint8_t *probe = disk_read_block1(pf_current_sector());
		struct frameInfo probe_info;
		probe_info.buffer = probe;
		if (memcmp(probe, "MVC\0", 4) != 0)
			fatalBlink(BLINK_FIELD_HEADER);
		frameInit(&probe_info);
		if (!probe_info.visibleLines || !probe_info.numBlocks ||
		    probe_info.numBlocks > FIELD_MAX_BLOCKS)
			fatalBlink(BLINK_FIELD_GEOMETRY);
		playback_field_blocks = probe_info.numBlocks;

		state.io_frameNumber = 1;
		state.io_bits |= STATE_PLAYING;

		clearFieldBuffers();

		for (int i = 0; i < 30; i++)
			waitEndFrame();
	}

	coreInfoToState();
	return selected;
}

static void
runFrameLoop(void)
{
	int which = 1;

	state.io_frameNumber = 1;
	state.io_bits = STATE_PLAYING;
	field_dma_pending = 0;
	mc_playback_pipeline = 1;
	stage_pulse(5);

	while (1) {
		waitEndFrame();

		if (field_dma_pending) {
			uint8_t finish = finishFieldRead();
			if (finish > 1)
				fatalBlink(finish);
			if (finish == 1) {
				while (!mc_visible_bars_vended)
					bus_serve_cycle();
				continue;
			}
		}

		checkSelectVideo(&which);
		updateTransport(&state);

		/*
		 * After $F1C1 the 6502 is in RIOT blanking. Finish the previous
		 * field's DMA, validate it, and swap display pointers here — still
		 * in blanking — then start the next CMD18 into the buffer whose
		 * visible use just ended. $F09D only begins VisibleBars; it does
		 * not swap. The first pass fills the inactive buffer and repeats
		 * the title once.
		 */
		mc_visible_bars_vended = 0;
		uint32_t offset =
			(uint32_t)(state.io_frameNumber * FIELD_NUM_BLOCKS);
		uint8_t start_fault =
			beginFieldRead(inactiveFrameInfo(), offset, 0);
		if (start_fault)
			fatalBlink(start_fault);

		while (!mc_visible_bars_vended)
			bus_serve_cycle();
	}
}

int
main(void)
{
	config_gpio_data();
	config_gpio_addr();
	config_status_led();
	dwt_init();
#if MOVIECART_ENABLE_SD
	/* SDIO on PD2 / PC8-12: do this before the 6507 is served so cart
	 * GPIOD MODER/ODR is not rewritten in the middle of a frame. */
	mc_sd_pins_init();
#endif

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
#if MOVIECART_INIT_PHASE == 1
	/* Same path as NO_SD: tight loop only. No sync wait, hold, LED, or SD. */
	emulate_cartridge();
#endif
	if (!wait_title_sync(3000u))
		fatalBlink(6);
	/* Title only — same serve as NO_SD — before mount, blinks, or SDIO. */
	bus_serve_ms(2000u);
#if MOVIECART_INIT_PHASE == 2
	emulate_cartridge();
#endif
	moviecart_wait_blanking_start();
#if MOVIECART_INIT_PHASE == 3
	emulate_cartridge();
#endif
	mc_led_host = 1;
	stage_pulse(1);

	setupDisk();

	updateInit();
#if MOVIECART_INIT_PHASE == 15
	emulate_cartridge();
#endif
	runTitle();
#if MOVIECART_INIT_PHASE == 17
	emulate_cartridge();
#endif
	runFrameLoop();
#else
	emulate_cartridge();
#endif
}
