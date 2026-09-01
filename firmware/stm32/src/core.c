
#include "pff.h"
#include "core.h"
#include "defines.h"
#include "cartridge_io.h"
#include "bus_service.h"

#define ST_OFF					0x86	/* stx(0) */
#define ST_ON					0x84	/* sty(2) */
#define ADDR_BOOT_WSYNC			0x31	/* sta WSYNC; HMOVE; wait_cnt; right_line */

/* Whole jmp targets. The RAM routine is in page 0; the kernel is in page $FF. */
#define JMP_RIGHT_LINE			0xff3eu
#define JMP_END_LINES			0xffb7u
#define JMP_WAIT_RAM			0x0084u

/*
 * Data on PD8-PD15 (high byte). Address PE0-PE12.
 *
 * Enabling the drivers here, rather than after bus_dispatch() returns, is what
 * keeps the heavy cases legal. Every case opens with SET_DATA and only then does
 * its side-effect work — advancing pointers, stepping the end-of-frame machine,
 * reloading frameInfo — and while that work runs, bus_dispatch() has not
 * returned, so a SET_DATA_MODE_OUT placed in the caller would still be waiting.
 * The 6502 would be handed its byte tens or hundreds of nanoseconds late on
 * exactly the cycles that matter most. Driving one instruction after the byte
 * reaches ODR takes every case's workload off the critical path; the exposure
 * to a stale value is the same zero as UnoCart's write-then-enable pair.
 */
#define SET_DATA(X)     do { DATA_OUT = ((uint16_t)(uint8_t)(X)) << 8; \
			     SET_DATA_MODE_OUT } while (0)
#define READ_DATA()     ((uint8_t)(DATA_IN >> 8))
#define DATA_OUTPUT     SET_DATA_MODE_OUT
#define DATA_INPUT      SET_DATA_MODE_IN

#define EMULATE_DONE    do { return; } while (0);

/*
 * The line counter is stepped by the 6502's own fetches, so a single dispatch
 * that never happens (or happens twice) leaves it with the wrong parity, and
 * `lines -= 2` then steps straight past zero and wraps. Nothing in the kernel
 * would ever end the frame again: the buffer pointers advance every line
 * forever, the picture becomes a scroll through SRAM, and the read eventually
 * walks off the end of it and faults the CPU. Treat any impossibly large
 * counter as "end of section" so the frame closes and every pointer is
 * reloaded from mr_frameInfo1/2, which costs nothing in normal operation —
 * the real values are at most visibleLines.
 */
#define LINES_EXHAUSTED(n)	((n) == 0 || (n) > 250)

struct coreInfo r_coreInfo;

/*
 * Frame-driven diagnostics. The bus loop owns the CPU with interrupts off, so
 * the status LED is the only channel out; it is driven entirely from the
 * kernel's own end-of-frame, one slot per frame. The blink count reports the
 * worst thing seen since the last report:
 *
 *   1 flash  nominal - frames closing normally, scanline count in range
 *   2        line counter wrapped in the visible section (missed dispatch)
 *   3        line counter wrapped in the end-lines section
 *   4        frame was not 250-275 scanlines long
 *
 * A frozen LED means the kernel stopped completing frames altogether.
 */
#define DIAG_NOMINAL		1
#define DIAG_WRAP_VISIBLE	2
#define DIAG_WRAP_ENDLINES	3
#define DIAG_FRAME_LENGTH	4
/* DIAG_FIELD_RETRY (5) lives in core.h — it is raised by main.c, not the kernel. */

#define DIAG_SLOT_FRAMES	8	/* one flash: 4 frames on, 4 off */
#define DIAG_GAP_FRAMES		30	/* silence between repeats */

#define DIAG_NOTE(code)	do { \
		if ((code) > diagWorst) \
			diagWorst = (code); \
	} while (0)

static uint8_t  diagWorst;	/* worst code seen since the last report */
static uint8_t  diagShowing;	/* code currently being blinked out */
static uint16_t diagTick;	/* frames into the current blink cycle */
static uint16_t frameLines;	/* scanlines counted in this frame */

/*
 * Set while main.c is blinking a code of its own on the same LED.
 *
 * There is exactly one status LED and two things that want to drive it. Blinking
 * a boot or failure code necessarily keeps serving the bus, and serving runs
 * bus_dispatch, which lands here once per frame and drove TESTA0 unconditionally —
 * so the heartbeat kept stamping on the code mid-flash. That is what made the
 * codes unreadable ("some of the led blinks are overlapped or faster"): they were
 * two patterns superimposed, not one pattern mistimed. Whoever is deliberately
 * blinking wins; the heartbeat resumes when it is done.
 */
volatile uint8_t mc_led_host;

/*
 * Raise a diagnostic code from outside the kernel.
 *
 * The heartbeat already reports the worst thing seen since its last report, and it
 * is non-fatal — exactly the right channel for "this happened but playback carried
 * on". A field load that only succeeded on a retry must not be silent: without
 * this, a healed transient fault and a genuinely clean run look identical, and we
 * would not know whether retries were doing any work.
 */
void
mc_diag_note(uint8_t code)
{
	DIAG_NOTE(code);
}

/*
 * ---------------------------------------------------------------------------
 * WaitCart handoff — UnoCart's sequence, in a ROM we own
 * ---------------------------------------------------------------------------
 *
 * UnoCart's 6502 copies WaitCartRoutine from its menu ROM into $84, then JSR $84.
 * The ARM only leaves the serve loop when it sees a cart fetch issued from RAM.
 * MovieCart owns every opcode in this file, so it does the same: the kernel
 * copies a wait routine out of ROM after ClearMem.
 *
 * The blanking kernel is exactly 76 cycles with no WSYNC. Do not JMP $84 while
 * playing — that desyncs the kernel (black title, then a solid LED because the
 * frame-based park timeout never ticks). Point nextLineJump at $0084 only when
 * ARMED. JMP high byte is $00 for RAM, $FF for $3E/$B7.
 *
 *   1. JSR $FFEC from $FF2D — after RESP0/RESP1 and before the WSYNC at $FF31,
 *      so the copy's length is invisible to the display. PrepareWait copies into
 *      $84, does the lda #$20 / sta HMP1 the JSR displaced, and RTS. A JSR
 *      after HMOVE skipped wait_cnt and jammed the 6507 (black, 6 flashes).
 *   2. Title uses stock $B7/$3E. Handoff sets nextLineJump = $0084.
 *   3. RAM: STA WSYNC, LDA $FFF4. Not READY → loop. READY → VBLANK off,
 *      JMP $FF31 (the boot path that WSYNCs, HMOVEs, and falls into right_line
 *      on-cycle). JMP $FFB7 at the wrong phase blacks the kernel.
 *   4. $FFF4 while ARMED → RUNNING, work(), READY, reload the visible field.
 *      The data bus is tri-stated for the whole of work(); the park loop runs
 *      from RIOT RAM and driving it would contend on every A12-low fetch.
 *
 * Nothing here may add work to a dispatch case the title fetches. The budget is
 * ~100 ns per cycle, so every decision that depends on the handoff state is
 * precomputed into mc_jmp_after_* / mc_gstore_page and the hot cases only load
 * a value, exactly as the baseline did.
 */
#define MC_WAIT_READY		0x5au
#define ADDR_WAIT_DOORBELL	0xf4u	/* $FFF4: only the RAM routine LDAs this */

/*
 * Copied to $84 by PrepareWait. Branch offsets are relative to that base.
 *
 * Park is STA WSYNC + poll $FFF4. Resume is JMP $FF31, not a cycle-counted
 * drop into end_lines: $31 WSYNCs, HMOVEs at @03, wait_cnt, then right_line
 * at the same phase as a cold boot. ARM reloads the visible field on READY
 * so graphBuf is not left pointing at the end of the previous section.
 *
 * VBLANK and VSYNC are both cleared before resuming. Whichever section the
 * kernel happened to end when it parked left its own value in them, and nothing
 * on the path back through right_line rewrites either one — parking out of the
 * vsync section would otherwise resume with VSYNC still asserted and no picture.
 *
 *   84: sta WSYNC
 *   86: lda $fff4
 *   89: cmp #READY
 *   8b: bne $84
 *   8d: ldx #0
 *   8f: stx VBLANK
 *   91: stx VSYNC
 *   93: jmp $ff31
 */
static const uint8_t mc_wait_code[] = {
	0x85, 0x02,
	0xad, ADDR_WAIT_DOORBELL, 0xff,
	0xc9, MC_WAIT_READY,
	0xd0, 0xf7,			/* bne $84; PC after = $8d */
	0xa2, 0x00,
	0x86, 0x01,
	0x86, 0x00,
	0x4c, ADDR_BOOT_WSYNC, 0xff
};

/*
 * Where the kernel jumps at the end of a section. Written only by the handoff,
 * read by the two dispatch cases that end a section, so those cases cost one
 * load instead of a volatile compare against mc_wait_state.
 */
static volatile uint_fast16_t	mc_jmp_after_visible = JMP_END_LINES;
static volatile uint_fast16_t	mc_jmp_after_blank = JMP_RIGHT_LINE;

/*
 * What gstore drives. PrepareWait's `lda $1E00,y` lands here, so during the one
 * boot copy this page holds mc_wait_code; afterwards it is all zeros, which is
 * the value the joystick reads (`lda $FE00,x`) have always been served. Serving
 * it from a table keeps gstore a single indexed load — the copy needs no test on
 * the hot path, and this case runs four times per blanking line.
 */
static uint8_t			mc_gstore_page[256];

volatile uint8_t	mc_wait_state;
volatile uint8_t	mc_wait_ready;
volatile uint8_t	mc_wait_fault;
volatile uint8_t	mc_visible_bars_vended;

static uint8_t		mc_poll_byte;
static volatile uint_fast8_t	mc_store_dummy;	/* gstore sink before joystick setup */

#define MC_WAIT_PARK		1u	/* $FFF4 value meaning "stay in RAM" */

/*
 * After SD work, drop back into the visible kernel the same way a cold boot
 * does: $FF31 WSYNC / HMOVE / wait_cnt / right_line. Reload the field that
 * endState 3 would have loaded, so graphBuf is not left at the end of the
 * section we parked out of (that walks the title through SRAM).
 */
static void
mc_wait_restart_visible(void)
{
	const struct frameInfo *src = r_coreInfo.mr_bufferIndex
		? &r_coreInfo.mr_frameInfo1
		: &r_coreInfo.mr_frameInfo2;

	r_coreInfo.frameInfo.colorBuf = src->colorBuf;
	r_coreInfo.frameInfo.colorBKBuf = src->colorBKBuf;
	r_coreInfo.frameInfo.audioBuf = src->audioBuf;
	r_coreInfo.frameInfo.graphBuf = src->graphBuf;
	r_coreInfo.frameInfo.visibleLines = src->visibleLines;
	r_coreInfo.frameInfo.overscanLines = src->overscanLines;
	r_coreInfo.frameInfo.vsyncLines = src->vsyncLines;
	r_coreInfo.frameInfo.blankLines = src->blankLines;
	r_coreInfo.frameInfo.odd = src->odd;
	r_coreInfo.mr_bufferIndex = !r_coreInfo.mr_bufferIndex;

	r_coreInfo.lines = r_coreInfo.frameInfo.visibleLines;
	r_coreInfo.endState = 0;
	r_coreInfo.vblankState = ST_OFF;
	r_coreInfo.vsyncState = ST_OFF;
	r_coreInfo.nextLineJump = JMP_RIGHT_LINE;
	mc_jmp_after_visible = JMP_END_LINES;
	mc_jmp_after_blank = JMP_RIGHT_LINE;
	frameLines = 0;
}

#define MC_WAIT_CHECK_EVERY	256u	/* DWT sampled this often, not per serve */

/*
 * The 6502 copies the routine itself during boot (JSR $FFEC after ClearMem).
 * By the time the title is up, that RTS has already run. This just waits for
 * it if we got here first.
 */
void
mc_wait_install(void)
{
	while (mc_wait_state != MC_WAIT_INSTALLED)
		bus_serve_cycle();

	/* Copy source is no longer needed; gstore goes back to driving zeros. */
	for (unsigned i = 0; i < sizeof(mc_wait_code); i++)
		mc_gstore_page[i] = 0;
}

void
mc_wait_handoff(void (*work)(void))
{
	if (mc_wait_state != MC_WAIT_INSTALLED) {
		__enable_irq();
		work();
		__disable_irq();
		return;
	}

	mc_wait_ready = 0;
	mc_poll_byte = MC_WAIT_PARK;
	mc_wait_fault = 0;
	mc_wait_state = MC_WAIT_ARMED;
	mc_jmp_after_visible = JMP_WAIT_RAM;
	mc_jmp_after_blank = JMP_WAIT_RAM;
	r_coreInfo.nextLineJump = JMP_WAIT_RAM;

	/*
	 * Time out on DWT, not kernel frames. Sample the counter every 256
	 * serves — a read after every cycle is the gap that jams the 6507, and
	 * this wait runs against a live title.
	 */
	uint32_t t0 = DWT->CYCCNT;
	uint32_t lim = SystemCoreClock * 2u;
	unsigned check = MC_WAIT_CHECK_EVERY;

	while (mc_wait_state != MC_WAIT_RUNNING) {
		bus_serve_cycle();
		if (--check == 0) {
			check = MC_WAIT_CHECK_EVERY;
			if ((DWT->CYCCNT - t0) > lim) {
				mc_wait_state = MC_WAIT_INSTALLED;
				mc_poll_byte = MC_WAIT_READY;
				mc_jmp_after_visible = JMP_END_LINES;
				mc_jmp_after_blank = JMP_RIGHT_LINE;
				r_coreInfo.nextLineJump = JMP_END_LINES;
				mc_wait_fault = 1;
				return;
			}
		}
	}

	/*
	 * Tri-state the data bus for the whole of work(), exactly as UnoCart does
	 * at its `got_cmd:` label before it goes off to touch the card.
	 *
	 * Driving a constant PARK here instead looks like the safer choice — it
	 * guarantees the parked 6502 cannot read a floating $FFF4 as something that
	 * matches READY — and it is the bug that made every resume fail. The park
	 * loop lives in RIOT RAM, so all of it except the one $FFF4 poll is A12-low:
	 * opcode and operand fetches that the RIOT is driving. Holding PD8-PD15
	 * enabled through that means the STM32 and the RIOT drive the same wires on
	 * every one of those cycles, and the 6502 executes whatever wins. It limped
	 * out of the loop eventually — which is why mount and open both completed
	 * and reported success — but by then its instruction stream was corrupt, so
	 * the jump back into the kernel never produced frames.
	 *
	 * A floating read cannot false-trigger READY here: the 6502 sees the last
	 * value driven on the bus, and the cycle before the $FFF4 data cycle is that
	 * instruction's own $FF operand fetch, so the poll reads ~$FF and fails the
	 * cmp against $5A. That is the same reasoning behind UnoCart's $D8 sentinel.
	 */
	TESTA0_HIGH;
	DATA_INPUT;
	__enable_irq();
	work();
	__disable_irq();

	/*
	 * Rebuild ARM kernel state *before* READY, not on the $FFF4 fetch.
	 * The 6502 is still in RAM; that fetch must be SET_DATA and a store,
	 * same as every other title cycle. Doing the pointer reload there put
	 * tens of stores on the first instruction of the resume.
	 */
	mc_wait_restart_visible();
	mc_wait_ready = 1;
	mc_poll_byte = MC_WAIT_READY;

	t0 = DWT->CYCCNT;
	lim = SystemCoreClock / 4u;	/* 250 ms */
	check = MC_WAIT_CHECK_EVERY;
	while (mc_wait_state == MC_WAIT_RUNNING) {
		bus_serve_cycle();
		if (--check == 0) {
			check = MC_WAIT_CHECK_EVERY;
			if ((DWT->CYCCNT - t0) > lim) {
				mc_wait_fault = 3;	/* parked, never left RAM */
				return;
			}
		}
	}

	/*
	 * Leaving RAM is not the same as running again. Demand one end-of-frame
	 * before returning. Same 256-serve sample: this loop *is* the resume, and
	 * a DWT read after every cycle is enough to stop the kernel completing
	 * the frame we are waiting for — which reported as fault 2 on a 1 ms
	 * empty stall with no SD code at all.
	 */
	t0 = DWT->CYCCNT;
	lim = SystemCoreClock / 4u;
	check = MC_WAIT_CHECK_EVERY;
	r_coreInfo.mr_endFrame = 0;
	while (!r_coreInfo.mr_endFrame) {
		bus_serve_cycle();
		if (--check == 0) {
			check = MC_WAIT_CHECK_EVERY;
			if ((DWT->CYCCNT - t0) > lim) {
				mc_wait_fault = 2;
				return;
			}
		}
	}
	r_coreInfo.mr_endFrame = 0;
}

static inline void
diagFrameTick(void)
{
	uint16_t flashing = (uint16_t)diagShowing * DIAG_SLOT_FRAMES;

	if (mc_led_host) {		/* main.c owns the LED right now */
		diagTick = 0;
		return;
	}

	if (diagTick < flashing && (diagTick & (DIAG_SLOT_FRAMES - 1)) <
				   (DIAG_SLOT_FRAMES / 2))
		TESTA0_LOW	/* LED on */
	else
		TESTA0_HIGH

	if (++diagTick >= flashing + DIAG_GAP_FRAMES) {
		diagTick = 0;
		diagShowing = diagWorst ? diagWorst : DIAG_NOMINAL;
		diagWorst = 0;
	}
}

void
coreInit(void)
{
	r_coreInfo.mr_endFrame = 1;
	r_coreInfo.mr_bufferIndex = false;
	mc_visible_bars_vended = 0;

	r_coreInfo.mr_swcha = 0xff;
	r_coreInfo.mr_swchb = 0xff;
	r_coreInfo.mr_inpt4 = 0xff;
	r_coreInfo.mr_inpt5 = 0xff;

	r_coreInfo.peekBus = 0xff;
	r_coreInfo.storeAddress = &r_coreInfo.peekBus;

	/*
	 * Original dsPIC value is 255 (settle through 255 BRK loops before the
	 * kernel starts). On this breadboard port that multiplies the number of
	 * window-first-fetches before anything runs — each one a chance for the
	 * 6502 to swallow a JAM opcode and halt for good. Every build that ever
	 * reached the blue title had 0 here: start the kernel on the first
	 * vector fetch, then rely on BRK recovery.
	 */
	r_coreInfo.breakLoops = 0;

	r_coreInfo.lines = 190;

	r_coreInfo.hiAddress = 0xf0;
	r_coreInfo.vblankState = ST_OFF;
	r_coreInfo.vsyncState = ST_OFF;
	r_coreInfo.endState = 0;
	r_coreInfo.nextLineJump = JMP_RIGHT_LINE;
	r_coreInfo.data = 0;
	r_coreInfo.storeAddress = &mc_store_dummy;

	/* PrepareWait's copy source, read as $1E00,Y. Cleared once installed. */
	for (unsigned i = 0; i < sizeof(mc_wait_code); i++)
		mc_gstore_page[i] = mc_wait_code[i];

	SET_DATA_MODE_IN;
}


HOTFUNC void
bus_dispatch(uint16_t lo_address, uint8_t addr_low8)
{
	/* In SRAM: a flash-resident table costs wait states per lookup, right
	 * on the data-valid critical path. */
	static const void* const romData[512] __attribute__((section(".ramfunc.romtable"))) = 
	{
		&&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore,
		&&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore,
		&&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore,
		&&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore,
		&&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore,
		&&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore,
		&&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore,
		&&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore,
		&&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore,
		&&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore,
		&&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore,
		&&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore,
		&&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore,
		&&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore,
		&&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore,
		&&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore, &&gstore,

		&&g0x00, &&g0x01, &&g0x02, &&g0x03, &&g0x04, &&g0x05, &&g0x06, &&g0x07, &&g0x08, &&g0x09, &&g0x0a, &&g0x0b, &&g0x0c, &&g0x0d, &&g0x0e, &&g0x0f,
		&&g0x10, &&g0x11, &&g0x12, &&g0x13, &&g0x14, &&g0x15, &&g0x16, &&g0x17, &&g0x18, &&g0x19, &&g0x1a, &&g0x1b, &&g0x1c, &&g0x1d, &&g0x1e, &&g0x1f,
		&&g0x20, &&g0x21, &&g0x22, &&g0x23, &&g0x24, &&g0x25, &&g0x26, &&g0x27, &&g0x28, &&g0x29, &&g0x2a, &&g0x2b, &&g0x2c, &&g0x2d, &&g0x2e, &&g0x2f,
		&&g0x30, &&g0x31, &&g0x32, &&g0x33, &&g0x34, &&g0x35, &&g0x36, &&g0x37, &&g0x38, &&g0x39, &&g0x3a, &&g0x3b, &&g0x3c, &&g0x3d, &&g0x3e, &&g0x3f,
		&&g0x40, &&g0x41, &&g0x42, &&g0x43, &&g0x44, &&g0x45, &&g0x46, &&g0x47, &&g0x48, &&g0x49, &&g0x4a, &&g0x4b, &&g0x4c, &&g0x4d, &&g0x4e, &&g0x4f,
		&&g0x50, &&g0x51, &&g0x52, &&g0x53, &&g0x54, &&g0x55, &&g0x56, &&g0x57, &&g0x58, &&g0x59, &&g0x5a, &&g0x5b, &&g0x5c, &&g0x5d, &&g0x5e, &&g0x5f,
		&&g0x60, &&g0x61, &&g0x62, &&g0x63, &&g0x64, &&g0x65, &&g0x66, &&g0x67, &&g0x68, &&g0x69, &&g0x6a, &&g0x6b, &&g0x6c, &&g0x6d, &&g0x6e, &&g0x6f,
		&&g0x70, &&g0x71, &&g0x72, &&g0x73, &&g0x74, &&g0x75, &&g0x76, &&g0x77, &&g0x78, &&g0x79, &&g0x7a, &&g0x7b, &&g0x7c, &&g0x7d, &&g0x7e, &&g0x7f,
		&&g0x80, &&g0x81, &&g0x82, &&g0x83, &&g0x84, &&g0x85, &&g0x86, &&g0x87, &&g0x88, &&g0x89, &&g0x8a, &&g0x8b, &&g0x8c, &&g0x8d, &&g0x8e, &&g0x8f,
		&&g0x90, &&g0x91, &&g0x92, &&g0x93, &&g0x94, &&g0x95, &&g0x96, &&g0x97, &&g0x98, &&g0x99, &&g0x9a, &&g0x9b, &&g0x9c, &&g0x9d, &&g0x9e, &&g0x9f,
		&&g0xa0, &&g0xa1, &&g0xa2, &&g0xa3, &&g0xa4, &&g0xa5, &&g0xa6, &&g0xa7, &&g0xa8, &&g0xa9, &&g0xaa, &&g0xab, &&g0xac, &&g0xad, &&g0xae, &&g0xaf,
		&&g0xb0, &&g0xb1, &&g0xb2, &&g0xb3, &&g0xb4, &&g0xb5, &&g0xb6, &&g0xb7, &&g0xb8, &&g0xb9, &&g0xba, &&g0xbb, &&g0xbc, &&g0xbd, &&g0xbe, &&g0xbf,
		&&g0xc0, &&g0xc1, &&g0xc2, &&g0xc3, &&g0xc4, &&g0xc5, &&g0xc6, &&g0xc7, &&g0xc8, &&g0xc9, &&g0xca, &&g0xcb, &&g0xcc, &&g0xcd, &&g0xce, &&g0xcf,
		&&g0xd0, &&g0xd1, &&g0xd2, &&g0xd3, &&g0xd4, &&g0xd5, &&g0xd6, &&g0xd7, &&g0xd8, &&g0xd9, &&g0xda, &&g0xdb, &&g0xdc, &&g0xdd, &&g0xde, &&g0xdf,
		&&g0xe0, &&g0xe1, &&g0xe2, &&g0xe3, &&g0xe4, &&g0xe5, &&g0xe6, &&g0xe7, &&g0xe8, &&g0xe9, &&g0xea, &&g0xeb, &&g0xec, &&g0xed, &&g0xee, &&g0xef,
		&&g0xf0, &&g0xf1, &&g0xf2, &&g0xf3, &&g0xf4, &&g0xf5, &&g0xf6, &&g0xf7, &&g0xf8, &&g0xf9, &&g0xfa, &&g0xfb, &&g0xfc, &&g0xfd, &&g0xfe, &&g0xff
	};


	goto *romData[lo_address];

gstore:
	*r_coreInfo.storeAddress = addr_low8;
	/*
	 * PrepareWait copies from $1E00,Y. Joystick LDA $FE00,X uses this page
	 * for the address bits, not the data, and wants a stable value (PAL 7800
	 * bios); mc_gstore_page is zero except during the boot copy.
	 */
	SET_DATA(mc_gstore_page[addr_low8]);
	EMULATE_DONE

g0x00:
	SET_DATA(0x78); // sei
	EMULATE_DONE

g0x01:
	SET_DATA(0xd8); // cld
	EMULATE_DONE

g0x02:
	SET_DATA(0xa2); // ldx #$FF
	EMULATE_DONE

g0x03:
	SET_DATA(0xff);
	EMULATE_DONE

g0x04:
	SET_DATA(0x9a); // txs
	EMULATE_DONE

g0x05:
	SET_DATA(0xa9); // lda #0	//zero memory
	EMULATE_DONE

g0x06:
	SET_DATA(0x00);
	EMULATE_DONE

g0x07:
	SET_DATA(0x95); // sta 0,X	// ClearMem
	EMULATE_DONE

g0x08:
	SET_DATA(0x00);
	EMULATE_DONE

g0x09:
	SET_DATA(0xca); // dex
	EMULATE_DONE

g0x0a:
	SET_DATA(0xd0); // bne ClearMem
	EMULATE_DONE

g0x0b:
	SET_DATA(0xfb);
	EMULATE_DONE

g0x0c:
	SET_DATA(0xa9); // lda #1
	EMULATE_DONE

g0x0d:
	SET_DATA(0x01);
	EMULATE_DONE

g0x0e:
	SET_DATA(0x85); // sta VDELP1
	EMULATE_DONE

g0x0f:
	SET_DATA(0x26);
	EMULATE_DONE

g0x10:
	SET_DATA(0xa9); // lda #$CF
	EMULATE_DONE

g0x11:
	SET_DATA(0xcf);
	EMULATE_DONE

g0x12:
	SET_DATA(0x85); // sta PF0
	EMULATE_DONE

g0x13:
	SET_DATA(0x0d);
	EMULATE_DONE

g0x14:
	SET_DATA(0xa9); // lda #$33
	EMULATE_DONE

g0x15:
	SET_DATA(0x33);
	EMULATE_DONE

g0x16:
	SET_DATA(0x85); // sta PF1
	EMULATE_DONE

g0x17:
	SET_DATA(0x0e);
	EMULATE_DONE

g0x18:
	SET_DATA(0xa9); // lda #$CC
	EMULATE_DONE

g0x19:
	SET_DATA(0xcc);
	EMULATE_DONE

g0x1a:
	SET_DATA(0x85); //sta PF2
	EMULATE_DONE

g0x1b:
	SET_DATA(0x0f);
	EMULATE_DONE

g0x1c:
	SET_DATA(0xa2); // ldx #$30
	EMULATE_DONE

g0x1d:
	SET_DATA(0x30);
	EMULATE_DONE

g0x1e:
	SET_DATA(0x85); // sta RESP0
	EMULATE_DONE

g0x1f:
	SET_DATA(0x10);
	EMULATE_DONE


g0x20:
	SET_DATA(0xea); // nop
	EMULATE_DONE

g0x21:
	SET_DATA(0x85); //sta RESP1
	EMULATE_DONE

g0x22:
	SET_DATA(0x11);
	EMULATE_DONE

g0x23:
	SET_DATA(0xa9); // lda #$06	//3 copies medium
	EMULATE_DONE

g0x24:
	SET_DATA(0x06); // lda #$06	//3 copies medium
	EMULATE_DONE

g0x25:
	SET_DATA(0x85); // sta NUSIZ0
	EMULATE_DONE

g0x26:
	SET_DATA(0x04);
	EMULATE_DONE

g0x27:
	SET_DATA(0xa9); // lda #$02	//2 copies medium
	EMULATE_DONE

g0x28:
	SET_DATA(0x02);
	EMULATE_DONE

g0x29:
	SET_DATA(0x85); // sta NUSIZ1
	EMULATE_DONE

g0x2a:
	SET_DATA(0x05);
	EMULATE_DONE

g0x2b:
	SET_DATA(0x86); // stx HMP0
	EMULATE_DONE

g0x2c:
	SET_DATA(0x20);
	EMULATE_DONE

/*
 * $FF2D: jsr PrepareWait, in place of the `lda #$20 / sta HMP1` that moved into
 * the routine. This slot is chosen for what surrounds it, not for its length.
 *
 * RESP0/RESP1 ($FF1E/$FF21) are the only writes whose column depends on where
 * the CPU is within a scanline, and ClearMem anchors them: `sta $00,x` hits
 * WSYNC when x reaches $02, so every cycle from there to RESP is fixed. A JSR
 * placed *before* RESP (the earlier $FF0C) inserted 267 cycles into that window
 * — 39 mod 76, i.e. 117 pixels of sprite displacement, which is the corrupt
 * title. Placed here, after RESP1 and before the WSYNC at $FF31, the copy is
 * bracketed by an anchor on the far side, so its length cannot be observed at
 * all: HMOVE, wait_cnt and the entry into right_line are all timed from that
 * WSYNC. Do not move it past $FF33 — after HMOVE it skips wait_cnt and jams the
 * 6507 (black, 6 flashes).
 */
g0x2d:
	SET_DATA(0x20); // jsr PrepareWait ($FFEC)
	EMULATE_DONE

g0x2e:
	SET_DATA(0xec);
	EMULATE_DONE

g0x2f:
	SET_DATA(0xff);
	EMULATE_DONE


g0x30:
	SET_DATA(0xea); // nop (HMP1 is set in PrepareWait)
	EMULATE_DONE

g0x31:
	SET_DATA(0x85); // sta WSYNC
	/* VisibleBars entry: $31 path is vended once per frame after blanking. */
	mc_visible_bars_vended = 1;
	EMULATE_DONE

g0x32:
	SET_DATA(0x02);
	EMULATE_DONE

g0x33:
	SET_DATA(0x85); // sta HMOVE
	EMULATE_DONE

g0x34:
	SET_DATA(0x2a);
	EMULATE_DONE

g0x35:
	SET_DATA(0xa2); // ldx #12
	EMULATE_DONE

g0x36:
	SET_DATA(0x0c);
	EMULATE_DONE

g0x37:
	SET_DATA(0xca); // dex ; wait_cnt
	EMULATE_DONE

g0x38:
	SET_DATA(0xd0); // bne wait_cnt
	EMULATE_DONE

g0x39:
	SET_DATA(0xfd);
	EMULATE_DONE

g0x3a:
	SET_DATA(0x85); // sta HMCLR
	EMULATE_DONE

g0x3b:
	SET_DATA(0x2b);
	EMULATE_DONE

g0x3c:
	SET_DATA(0xea); // nop
	EMULATE_DONE

g0x3d:
	SET_DATA(0xea); // nop
	EMULATE_DONE

g0x3e:
	SET_DATA(0xa9); // lda #GDATA6 	// 2	// right_line
	r_coreInfo.data = r_coreInfo.frameInfo.graphBuf[1];
	EMULATE_DONE

g0x3f:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE



g0x40:
	SET_DATA(0x85); // sta GRP1 	// 3 VDELed
	EMULATE_DONE

g0x41:
	SET_DATA(0x1c);
	EMULATE_DONE

g0x42:
	SET_DATA(0x85); // 2a sta HMOVE 	// 3 @03 +8 pixel
	EMULATE_DONE

g0x43:
	SET_DATA(0x2a);
	EMULATE_DONE

g0x44:
	SET_DATA(0xa9); // lda #AUD_DATA 	// 2
	r_coreInfo.data = r_coreInfo.audioPushed ? r_coreInfo.audioVal : *r_coreInfo.frameInfo.audioBuf++;
	EMULATE_DONE

g0x45:
	SET_DATA(r_coreInfo.data);
	r_coreInfo.audioPushed = false;
	EMULATE_DONE

g0x46:
	SET_DATA(0xa2);	// ldx #GDATA9 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.graphBuf[4];
	EMULATE_DONE

g0x47:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x48:
	SET_DATA(0x85);	// sta AUDV0 	// 3 @10
	EMULATE_DONE

g0x49:
	SET_DATA(0x19);
	EMULATE_DONE

g0x4a:
	SET_DATA(0xa0); // ldy #GCOL9 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.colorBuf[4];
	EMULATE_DONE

g0x4b:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x4c:
	SET_DATA(0xa9); // lda #GCOL6 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.colorBuf[1];
	EMULATE_DONE

g0x4d:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x4e:
	SET_DATA(0x85); // sta COLUP1 	// 3
	EMULATE_DONE

g0x4f:
	SET_DATA(0x07);
	EMULATE_DONE


g0x50:
	SET_DATA(0xa9);	// lda #GDATA5 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.graphBuf[0];
	EMULATE_DONE

g0x51:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x52:
	SET_DATA(0x85);	// sta GRP0 	// 3
	EMULATE_DONE

g0x53:
	SET_DATA(0x1b);
	EMULATE_DONE

g0x54:
	SET_DATA(0xa9); // lda #GCOL5 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.colorBuf[0];
	EMULATE_DONE

g0x55:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x56:
	SET_DATA(0x85);	// sta COLUP0 	// 3
	EMULATE_DONE

g0x57:
	SET_DATA(0x06);
	EMULATE_DONE

g0x58:
	SET_DATA(0xa9);	// lda #GDATA8 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.graphBuf[3];
	EMULATE_DONE

g0x59:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x5a:
	SET_DATA(0x85);	// sta GRP1 	// 3 VDELed
	EMULATE_DONE

g0x5b:
	SET_DATA(0x1c);
	EMULATE_DONE

g0x5c:
	SET_DATA(0xa9);	// lda #$00 	// 2 background color
	r_coreInfo.data = *r_coreInfo.frameInfo.colorBKBuf++;
	EMULATE_DONE

g0x5d:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x5e:
	SET_DATA(0x85);	// sta COLUBK 	// 3 background color
	EMULATE_DONE

g0x5f:
	SET_DATA(0x09);
	EMULATE_DONE

g0x60:
	SET_DATA(0xa9); // lda #GCOL7 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.colorBuf[2];
	EMULATE_DONE

g0x61:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x62:
	SET_DATA(0x85);	// sta COLUP0 	// 3 @42! end of GRP0a display
	EMULATE_DONE

g0x63:
	SET_DATA(0x06);
	EMULATE_DONE

g0x64:
	SET_DATA(0xa9);	// lda #GDATA7 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.graphBuf[2];
	EMULATE_DONE

g0x65:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x66:
	SET_DATA(0x85);	// sta GRP0 	// 3 @47! end of GRP1a display
	EMULATE_DONE

g0x67:
	SET_DATA(0x1b);
	EMULATE_DONE

g0x68:
	SET_DATA(0xa9); // lda #GCOL8 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.colorBuf[3];
	EMULATE_DONE

g0x69:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x6a:
	SET_DATA(0x85);	// sta COLUP1 	// 3 @52
	EMULATE_DONE

g0x6b:
	SET_DATA(0x07);
	EMULATE_DONE

g0x6c:
	SET_DATA(0x86);	// stx GRP0 	// 3 @55
	EMULATE_DONE

g0x6d:
	SET_DATA(0x1b);
	EMULATE_DONE

g0x6e:
	SET_DATA(0x84);	// sty COLUP0 	// 3 @58<=@60
	EMULATE_DONE

g0x6f:
	SET_DATA(0x06);
	EMULATE_DONE


g0x70:
	SET_DATA(0xa9);	// lda #$00 	// 2 turn off background color
	EMULATE_DONE

g0x71:
	SET_DATA(0x00);
	EMULATE_DONE

g0x72:
	SET_DATA(0x85);	// 09 sta COLUBK 	// 3 background color
	EMULATE_DONE

g0x73:
	SET_DATA(0x09);
	EMULATE_DONE

g0x74:
	SET_DATA(0x85);	// sta HMCLR 	// 3
	EMULATE_DONE

g0x75:
	SET_DATA(0x2b);
	EMULATE_DONE

g0x76:
	SET_DATA(0xa9);	// lda #00 	// 2 dummy	// left_line
	EMULATE_DONE

g0x77:
	SET_DATA(0x00);
	EMULATE_DONE

g0x78:
	SET_DATA(0xa9);	// lda #GDATA1 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.graphBuf[6];
	EMULATE_DONE

g0x79:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x7a:
	SET_DATA(0x85);	// sta HMOVE	//back 8, late hmove 	//needs to be on cycle 71
	EMULATE_DONE

g0x7b:
	SET_DATA(0x2a);
	EMULATE_DONE

g0x7c:
	SET_DATA(0x85);	// sta GRP1 	// 3 VDELed
	EMULATE_DONE

g0x7d:
	SET_DATA(0x1c);
	EMULATE_DONE

g0x7e:
	SET_DATA(0xa9);	// lda #GCOL1 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.colorBuf[6];
	EMULATE_DONE

g0x7f:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE


g0x80:
	SET_DATA(0x85);	// sta COLUP1 	// 3
	EMULATE_DONE

g0x81:
	SET_DATA(0x07);
	EMULATE_DONE

g0x82:
	SET_DATA(0xa9); // lda #AUD_DATA 	// 2
	r_coreInfo.data = *r_coreInfo.frameInfo.audioBuf++;
	EMULATE_DONE

g0x83:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x84:
	SET_DATA(0x85);	// sta AUDV0 	// 3 @10
	EMULATE_DONE

g0x85:
	SET_DATA(0x19);
	EMULATE_DONE

g0x86:
	SET_DATA(0xa2); // ldx #GDATA4 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.graphBuf[9];
	EMULATE_DONE

g0x87:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x88:
	SET_DATA(0xa0); // ldy #GCOL4 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.colorBuf[9];
	EMULATE_DONE

g0x89:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x8a:
	SET_DATA(0xa9); // lda #GDATA0 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.graphBuf[5];
	EMULATE_DONE

g0x8b:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x8c:
	SET_DATA(0x85); // sta GRP0 	// 3
	EMULATE_DONE

g0x8d:
	SET_DATA(0x1b);
	EMULATE_DONE

g0x8e:
	SET_DATA(0xa9); // lda #GCOL0 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.colorBuf[5];
	EMULATE_DONE

g0x8f:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x90:
	SET_DATA(0x85);	// sta COLUP0 	// 3
	EMULATE_DONE

g0x91:
	SET_DATA(0x06);
	EMULATE_DONE

g0x92:
	SET_DATA(0xa9); // lda #GDATA3 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.graphBuf[8];
	EMULATE_DONE

g0x93:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x94:
	SET_DATA(0x85);	// sta GRP1 	// 3 VDELed
	EMULATE_DONE

g0x95:
	SET_DATA(0x1c);
	EMULATE_DONE

g0x96:
	SET_DATA(0xa9);	// lda #$00 	// 2 playfield color
	r_coreInfo.data = *r_coreInfo.frameInfo.colorBKBuf++;
	EMULATE_DONE

g0x97:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x98:
	SET_DATA(0x85);	// sta COLUPF 	// 3 playfield color
	EMULATE_DONE

g0x99:
	SET_DATA(0x08);
	EMULATE_DONE

g0x9a:
	SET_DATA(0xa9); // lda #GCOL2 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.colorBuf[7];
	EMULATE_DONE

g0x9b:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0x9c:
	SET_DATA(0x85);	// sta COLUP0 	// 3 @39! end of GRP0a display
	EMULATE_DONE

g0x9d:
	SET_DATA(0x06);
	EMULATE_DONE

g0x9e:
	SET_DATA(0xa9); // lda #GDATA2 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.graphBuf[7];
	EMULATE_DONE

g0x9f:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0xa0:
	SET_DATA(0x85);	// sta GRP0 	// 3 @44! end of GRP1a display
	EMULATE_DONE

g0xa1:
	SET_DATA(0x1b);
	EMULATE_DONE

g0xa2:
	SET_DATA(0xa9); // lda #GCOL3 	// 2
	r_coreInfo.data = r_coreInfo.frameInfo.colorBuf[8];
	EMULATE_DONE

g0xa3:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0xa4:
	SET_DATA(0x85);	// sta COLUP1 	// 3 @49
	EMULATE_DONE

g0xa5:
	SET_DATA(0x07);
	EMULATE_DONE

g0xa6:
	SET_DATA(0x86);	// stx GRP0 	// 3 @52
	EMULATE_DONE

g0xa7:
	SET_DATA(0x1b);
	EMULATE_DONE

g0xa8:
	SET_DATA(0x84);	// sty COLUP0 	// 3 @55<=@57
	EMULATE_DONE

g0xa9:
	SET_DATA(0x06);
	EMULATE_DONE

g0xaa:
	SET_DATA(0xa9);	// lda #$00 	// 2 turn off playfield
	EMULATE_DONE

g0xab:
	SET_DATA(0x00);
	EMULATE_DONE

g0xac:
	SET_DATA(0x85);	// sta COLUPF 	// 3
	EMULATE_DONE

g0xad:
	SET_DATA(0x08);
	EMULATE_DONE

g0xae:
	SET_DATA(0xa9);	// lda #$80 	// 2
	EMULATE_DONE

g0xaf:
	SET_DATA(0x80);	// lda #$80 	// 2
	r_coreInfo.lines -= 2;
	frameLines += 2;
	EMULATE_DONE

g0xb0:
	SET_DATA(0x85);	// sta HMP0 	// 3
	if (LINES_EXHAUSTED(r_coreInfo.lines))
	{
		if (r_coreInfo.lines)	/* wrapped, not a clean zero */
			DIAG_NOTE(DIAG_WRAP_VISIBLE);

		r_coreInfo.nextLineJump = mc_jmp_after_visible;

		if (!r_coreInfo.frameInfo.odd)
			r_coreInfo.lines = r_coreInfo.frameInfo.overscanLines;
		else
			r_coreInfo.lines = r_coreInfo.frameInfo.overscanLines-1;

		r_coreInfo.vblankState = ST_ON;
	}
	else
	{
		r_coreInfo.frameInfo.graphBuf += 10;
		r_coreInfo.frameInfo.colorBuf += 10;
	}
	EMULATE_DONE

g0xb1:
	SET_DATA(0x20);
	EMULATE_DONE

g0xb2:
	SET_DATA(0x85);	// sta HMP1 	// 3 @63
	EMULATE_DONE

g0xb3:
	SET_DATA(0x21);
	EMULATE_DONE

g0xb4:
	SET_DATA(0x4c); // jmp $FFxx (kernel) or $0084 (RAM wait)
	EMULATE_DONE

g0xb5:
	SET_DATA(r_coreInfo.nextLineJump);
	EMULATE_DONE

g0xb6:
	SET_DATA(r_coreInfo.nextLineJump >> 8);
	EMULATE_DONE

g0xb7:
	SET_DATA(0xa0);	// ldy #2	// end_lines 	//@213	// end of current line
	EMULATE_DONE

g0xb8:
	SET_DATA(0x02);
	EMULATE_DONE

g0xb9:
	SET_DATA(0xa2);
	EMULATE_DONE	// ldx #0

g0xba:
	SET_DATA(0x00);
	EMULATE_DONE

g0xbb:
	SET_DATA(r_coreInfo.vsyncState);
	EMULATE_DONE	// stx VSYNC	// beginning of new line

g0xbc:
	SET_DATA(0x00);
	EMULATE_DONE

g0xbd:
	SET_DATA(r_coreInfo.vblankState);
	EMULATE_DONE	// stx VBLANK

g0xbe:
	SET_DATA(0x01);
	EMULATE_DONE

g0xbf:
	SET_DATA(0xa9); // lda #AUD_DATA 	// 2
	r_coreInfo.data = *r_coreInfo.frameInfo.audioBuf++;
	EMULATE_DONE


g0xc0:
	SET_DATA(r_coreInfo.data);
	EMULATE_DONE

g0xc1:
	SET_DATA(0x85); // sta AUDV0 	// 3 @10
	EMULATE_DONE

g0xc2:
	SET_DATA(0x19);
	EMULATE_DONE

g0xc3:
	SET_DATA(0xae); // ldx SWCHA 	// 4 ad 80 02 0x280 RLDU(1) RLDU(2) 	// 1111 1111 by default
	EMULATE_DONE

g0xc4:
	SET_DATA(0x80);
	EMULATE_DONE

g0xc5:
	SET_DATA(0x02);
	EMULATE_DONE

g0xc6:
	SET_DATA(0xbd);  // lda   $FE00,x //	 4  first 256 bytes
	EMULATE_DONE

g0xc7:
	SET_DATA(0x00);
	EMULATE_DONE

g0xc8:
	SET_DATA(0xfe);
	r_coreInfo.storeAddress = &r_coreInfo.mr_swcha;
	EMULATE_DONE

g0xc9:
	SET_DATA(0xae); // ldx SWCHB 	// 4 ad 82 02 0x282 a b - - color - select reset
	EMULATE_DONE

g0xca:
	SET_DATA(0x82);
	EMULATE_DONE

g0xcb:
	SET_DATA(0x02);
	EMULATE_DONE

g0xcc:
	SET_DATA(0xbd);  // lda   $FE00,x //	 4  first 256 bytes
	EMULATE_DONE

g0xcd:
	SET_DATA(0x00);
	EMULATE_DONE

g0xce:
	SET_DATA(0xfe);
	r_coreInfo.storeAddress = &r_coreInfo.mr_swchb;
	EMULATE_DONE

g0xcf:
	SET_DATA(0xa6); // ldx INPT4 	// 3 a5 0c 0x0c button - - - - - - -
	EMULATE_DONE


g0xd0:
	SET_DATA(0x0c);
	EMULATE_DONE

g0xd1:
	SET_DATA(0xbd);  // lda   $FE00,x //	 4  first 256 bytes
	EMULATE_DONE

g0xd2:
	SET_DATA(0x00);
	EMULATE_DONE

g0xd3:
	SET_DATA(0xfe);
	r_coreInfo.storeAddress = &r_coreInfo.mr_inpt4;
	EMULATE_DONE

g0xd4:
	SET_DATA(0xa6); // ldx INPT5 	// 3 a5 0d 0x0d button - - - - - - -
	EMULATE_DONE

g0xd5:
	SET_DATA(0x0d);
	EMULATE_DONE

g0xd6:
	SET_DATA(0xbd);  // lda   $FE00,x //	 4  first 256 bytes
	EMULATE_DONE

g0xd7:
	SET_DATA(0x00);
	EMULATE_DONE

g0xd8:
	SET_DATA(0xfe);
	r_coreInfo.storeAddress = &r_coreInfo.mr_inpt5;
	EMULATE_DONE

g0xd9:
	SET_DATA(0xa2); // ldx   #0 
	frameLines++;
	r_coreInfo.lines--;
	if (LINES_EXHAUSTED(r_coreInfo.lines))
	{
		if (r_coreInfo.lines)	/* wrapped, not a clean zero */
			DIAG_NOTE(DIAG_WRAP_ENDLINES);

		switch (r_coreInfo.endState)
		{
			case 0:
				TESTA1_HIGH
				r_coreInfo.endState++;
				r_coreInfo.lines = r_coreInfo.frameInfo.vsyncLines;
				r_coreInfo.vsyncState = ST_ON;

				EMULATE_DONE

			case 1:
				r_coreInfo.endState++;
				if (!r_coreInfo.frameInfo.odd)
					r_coreInfo.lines = r_coreInfo.frameInfo.blankLines+1;
				else
					r_coreInfo.lines = r_coreInfo.frameInfo.blankLines;

				r_coreInfo.vsyncState = ST_OFF;
				EMULATE_DONE

			case 2:
				r_coreInfo.endState++;
				r_coreInfo.vblankState = ST_OFF;
				/*
				 * Deliberately leaves lines at zero; $FFe3
				 * loads visibleLines later in this same
				 * scanline. Adding `lines = 1` here to absorb a
				 * spurious extra decrement was tried and blacked
				 * out the display: the end-of-frame cases have
				 * no room for even one more store.
				 */
				r_coreInfo.nextLineJump = mc_jmp_after_blank;
				EMULATE_DONE
		}
	}
	EMULATE_DONE

g0xda:
	SET_DATA(0x00);
	if (r_coreInfo.endState == 3)
	{
		if (r_coreInfo.frameInfo.odd)
		{
			r_coreInfo.audioVal = *r_coreInfo.frameInfo.audioBuf++;
			r_coreInfo.audioPushed = true;
		}
	}
	EMULATE_DONE

g0xdb:
	SET_DATA(0xea); // nop
	if (r_coreInfo.endState == 3)
	{
		if (r_coreInfo.mr_bufferIndex == 0)
		{
			r_coreInfo.frameInfo.colorBuf = r_coreInfo.mr_frameInfo2.colorBuf;
			r_coreInfo.frameInfo.colorBKBuf = r_coreInfo.mr_frameInfo2.colorBKBuf;
		}
		else
		{
			r_coreInfo.frameInfo.colorBuf = r_coreInfo.mr_frameInfo1.colorBuf;
			r_coreInfo.frameInfo.colorBKBuf = r_coreInfo.mr_frameInfo1.colorBKBuf;
		}
	}
	EMULATE_DONE

g0xdc:
	SET_DATA(0xea); // nop
	if (r_coreInfo.endState == 3)
	{
		if (r_coreInfo.mr_bufferIndex == 0)
		{
			r_coreInfo.frameInfo.audioBuf = r_coreInfo.mr_frameInfo2.audioBuf;
			r_coreInfo.frameInfo.graphBuf = r_coreInfo.mr_frameInfo2.graphBuf;
		}
		else
		{
			r_coreInfo.frameInfo.audioBuf = r_coreInfo.mr_frameInfo1.audioBuf;
			r_coreInfo.frameInfo.graphBuf = r_coreInfo.mr_frameInfo1.graphBuf;
		}
	}
	EMULATE_DONE

g0xdd:
	SET_DATA(0xea); // nop
	if (r_coreInfo.endState == 3)
	{
		if (r_coreInfo.mr_bufferIndex == 0)
		{
			r_coreInfo.frameInfo.visibleLines = r_coreInfo.mr_frameInfo2.visibleLines;
			r_coreInfo.frameInfo.overscanLines = r_coreInfo.mr_frameInfo2.overscanLines;
		}
		else
		{
			r_coreInfo.frameInfo.visibleLines = r_coreInfo.mr_frameInfo1.visibleLines;
			r_coreInfo.frameInfo.overscanLines = r_coreInfo.mr_frameInfo1.overscanLines;
		}
	}
	EMULATE_DONE

g0xde:
	SET_DATA(0xea); // nop
	if (r_coreInfo.endState == 3)
	{
		if (r_coreInfo.mr_bufferIndex == 0)
		{
			r_coreInfo.frameInfo.vsyncLines = r_coreInfo.mr_frameInfo2.vsyncLines;
			r_coreInfo.frameInfo.blankLines = r_coreInfo.mr_frameInfo2.blankLines;
		}
		else
		{
			r_coreInfo.frameInfo.vsyncLines = r_coreInfo.mr_frameInfo1.vsyncLines;
			r_coreInfo.frameInfo.blankLines = r_coreInfo.mr_frameInfo1.blankLines;
		}
	}
	EMULATE_DONE

g0xdf:
	SET_DATA(0xea); // nop
	if (r_coreInfo.endState == 3)
	{
		if (r_coreInfo.mr_bufferIndex == 0)
		{
			r_coreInfo.frameInfo.odd = r_coreInfo.mr_frameInfo2.odd;
		}
		else
		{
			r_coreInfo.frameInfo.odd = r_coreInfo.mr_frameInfo1.odd;
		}
	}
	EMULATE_DONE	// nop

g0xe0:
	SET_DATA(0xea); // nop
	if (r_coreInfo.endState == 3)
	{
		r_coreInfo.mr_bufferIndex = !r_coreInfo.mr_bufferIndex;
	}
	EMULATE_DONE	// nop

g0xe1:
g0xe2:
	SET_DATA(0xea); // nop
	EMULATE_DONE

g0xe3:
	SET_DATA(r_coreInfo.vsyncState);	// stx VSYNC
	if (r_coreInfo.endState == 3)
	{
		r_coreInfo.lines = r_coreInfo.frameInfo.visibleLines;

		r_coreInfo.endState = 0;
		r_coreInfo.mr_endFrame = true;
		TESTA1_LOW

		if (frameLines < 250 || frameLines > 275)
			DIAG_NOTE(DIAG_FRAME_LENGTH);
		frameLines = 0;
#if MOVIECART_STALL_TEST != 1 && MOVIECART_STALL_TEST != 2
		diagFrameTick();
#endif
	}
	EMULATE_DONE

g0xe4:
	SET_DATA(0x00);
	EMULATE_DONE

g0xe5:
	SET_DATA(r_coreInfo.vblankState);	// stx VBLANK
	EMULATE_DONE

g0xe6:
	SET_DATA(0x01);
	EMULATE_DONE

g0xe7:
g0xe8:
	SET_DATA(0xea); // nop
	EMULATE_DONE

g0xe9:
	SET_DATA(0x4c);	// jmp
	EMULATE_DONE

g0xea:
	SET_DATA(r_coreInfo.nextLineJump);	// $B7 / $3E / $84
	EMULATE_DONE

g0xeb:
	SET_DATA(r_coreInfo.nextLineJump >> 8);
	EMULATE_DONE

/*
 * $FFEC-$FFFB: PrepareWait. JSR from $FF2D, RTS to $FF30.
 *
 *   FFEC: ldy #(sizeof(mc_wait_code)-1)
 *   FFEE: lda $1E00,y
 *   FFF1: sta $0084,y
 *   FFF4: dey
 *   FFF5: bpl $FFEE
 *   FFF7: lda #$20
 *   FFF9: sta HMP1
 *   FFFB: rts          ; 6507 has no NMI; $FFFA-$FFFB are free
 *
 * The lda/sta pair is the one the JSR displaced at $FF2D, moved here verbatim.
 * It has to be a plain store; an `inc` of a write-only TIA register reads back
 * stale bus data instead of the value it is meant to increment, which is how an
 * earlier `inc VDELP1` here left VDELP1 as bit 0 of whatever the bus last held.
 *
 * Exactly 16 bytes, filling $FFEC-$FFFB. Growing mc_wait_code costs no ROM here
 * and no longer perturbs the display either, because the WSYNC at $FF31 absorbs
 * however long the copy takes.
 *
 * $FFF4 is dey during that one copy (IDLE). After INSTALLED it is the doorbell.
 */
g0xec:
	SET_DATA(0xa0);
	EMULATE_DONE

g0xed:
	SET_DATA((uint8_t)(sizeof(mc_wait_code) - 1));	/* ldy #N-1 */
	EMULATE_DONE

g0xee:
	SET_DATA(0xb9);
	EMULATE_DONE

g0xef:
	SET_DATA(0x00);
	EMULATE_DONE

g0xf0:
	SET_DATA(0x1e);
	EMULATE_DONE

g0xf1:
	SET_DATA(0x99);
	EMULATE_DONE

g0xf2:
	SET_DATA(0x84);
	EMULATE_DONE

g0xf3:
	SET_DATA(0x00);
	EMULATE_DONE

g0xf4:
	if (mc_wait_state == MC_WAIT_IDLE)
		SET_DATA(0x88);	/* dey, PrepareWait copy only */
	else
		SET_DATA(mc_poll_byte);	/* PARK or READY */
	if (mc_wait_state == MC_WAIT_ARMED) {
		mc_wait_state = MC_WAIT_RUNNING;
	} else if (mc_wait_state == MC_WAIT_RUNNING &&
		   mc_poll_byte == MC_WAIT_READY) {
		mc_wait_state = MC_WAIT_INSTALLED;
	}
	EMULATE_DONE

g0xf5:
	SET_DATA(0x10);	/* bpl */
	EMULATE_DONE

g0xf6:
	SET_DATA(0xf7);	/* bpl $FFEE: $FFF7 + (-9) */
	EMULATE_DONE

g0xf7:
	SET_DATA(0xa9);	/* lda #$20 */
	EMULATE_DONE

g0xf8:
	SET_DATA(0x20);
	EMULATE_DONE

g0xf9:
	SET_DATA(0x85);	/* sta HMP1 */
	EMULATE_DONE

g0xfa:
	SET_DATA(0x21);
	EMULATE_DONE

g0xfb:
	SET_DATA(0x60); /* rts; 6507 has no NMI pin */
	if (mc_wait_state == MC_WAIT_IDLE) {
		mc_wait_state = MC_WAIT_INSTALLED;
		mc_poll_byte = MC_WAIT_READY;
	}
	EMULATE_DONE

   // break a number of times to make sure the system is actually stable

g0xfc:
g0xfe:
	if (r_coreInfo.breakLoops)
	{
		SET_DATA(0xf0); // .word.w main_start	// RESET / IRQ
		r_coreInfo.breakLoops--;
	}
	else
	{
		SET_DATA(0x00);
	}
	EMULATE_DONE

g0xfd:
g0xff:
	SET_DATA(0xff);
	EMULATE_DONE

}
