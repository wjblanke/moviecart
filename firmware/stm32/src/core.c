
#include "pff.h"
#include "core.h"
#include "core_rom.h"
#include "defines.h"
#include "cartridge_io.h"
#include "bus_service.h"

#define ST_OFF					0x86	/* stx(0) */
#define ST_ON					0x84	/* sty(2) */

/* core.asm / core.bin cart layout (org $F000). */
#define MC_OFF_VISIBLE_ENTRY		0x09du
#define MC_OFF_LINE0			0x0bfu
#define MC_OFF_END_LINES		0x40eu
#define MC_OFF_VISIBLE_RTS		0x41du
#define MC_LINE_CYCLE			0x079u
#define MC_PHASE_DYNAMIC_MAX		0x070u	/* g0x3e..g0xae; g0xaf+ from ROM */

#define JMP_VISIBLE_BARS		0xf09du

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

volatile uint8_t	mc_visible_bars_vended;
volatile uint8_t	mc_blanking_window;
volatile uint16_t	mc_blanking_window_gen;
volatile uint8_t	mc_sdio_gate_relaxed;
volatile uint8_t	mc_playback_pipeline;

static uint16_t		mc_cart_off;
static volatile uint_fast8_t	mc_store_dummy;	/* gstore sink before joystick setup */

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
	mc_blanking_window = 0;
	mc_blanking_window_gen = 0;
	mc_sdio_gate_relaxed = 0;
	mc_playback_pipeline = 0;

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
	r_coreInfo.nextLineJump = JMP_VISIBLE_BARS;
	r_coreInfo.data = 0;
	r_coreInfo.storeAddress = &mc_store_dummy;

	SET_DATA_MODE_IN;
}

void
mc_field_swap_to_display(void)
{
	if (r_coreInfo.mr_bufferIndex == 0) {
		r_coreInfo.frameInfo.colorBuf = r_coreInfo.mr_frameInfo2.colorBuf;
		r_coreInfo.frameInfo.colorBKBuf = r_coreInfo.mr_frameInfo2.colorBKBuf;
		r_coreInfo.frameInfo.audioBuf = r_coreInfo.mr_frameInfo2.audioBuf;
		r_coreInfo.frameInfo.graphBuf = r_coreInfo.mr_frameInfo2.graphBuf;
		r_coreInfo.frameInfo.visibleLines = r_coreInfo.mr_frameInfo2.visibleLines;
		r_coreInfo.frameInfo.overscanLines = r_coreInfo.mr_frameInfo2.overscanLines;
		r_coreInfo.frameInfo.vsyncLines = r_coreInfo.mr_frameInfo2.vsyncLines;
		r_coreInfo.frameInfo.blankLines = r_coreInfo.mr_frameInfo2.blankLines;
		r_coreInfo.frameInfo.totalLines = r_coreInfo.mr_frameInfo2.totalLines;
		r_coreInfo.frameInfo.odd = r_coreInfo.mr_frameInfo2.odd;
	} else {
		r_coreInfo.frameInfo.colorBuf = r_coreInfo.mr_frameInfo1.colorBuf;
		r_coreInfo.frameInfo.colorBKBuf = r_coreInfo.mr_frameInfo1.colorBKBuf;
		r_coreInfo.frameInfo.audioBuf = r_coreInfo.mr_frameInfo1.audioBuf;
		r_coreInfo.frameInfo.graphBuf = r_coreInfo.mr_frameInfo1.graphBuf;
		r_coreInfo.frameInfo.visibleLines = r_coreInfo.mr_frameInfo1.visibleLines;
		r_coreInfo.frameInfo.overscanLines = r_coreInfo.mr_frameInfo1.overscanLines;
		r_coreInfo.frameInfo.vsyncLines = r_coreInfo.mr_frameInfo1.vsyncLines;
		r_coreInfo.frameInfo.blankLines = r_coreInfo.mr_frameInfo1.blankLines;
		r_coreInfo.frameInfo.totalLines = r_coreInfo.mr_frameInfo1.totalLines;
		r_coreInfo.frameInfo.odd = r_coreInfo.mr_frameInfo1.odd;
	}
	r_coreInfo.mr_bufferIndex = !r_coreInfo.mr_bufferIndex;
}

/*
 * Base of the audio array for the field currently on screen (mr_frameInfo
 * holds the stable base; frameInfo.audioBuf is the live cursor).
 */
static uint8_t *
mc_display_audio_base(void)
{
	if (r_coreInfo.mr_bufferIndex)
		return r_coreInfo.mr_frameInfo2.audioBuf;
	return r_coreInfo.mr_frameInfo1.audioBuf;
}

static void
mc_audio_begin_visible(void)
{
	/*
	 * MovieCart fields store audio in play order: visible scanlines first,
	 * then vsync+blank+overscan (see the old cart kernel's end_lines path).
	 * RamKernel runs visible from cart, blanking from RIOT with no AUDV0.
	 */
	r_coreInfo.frameInfo.audioBuf = mc_display_audio_base();
}

static void
mc_audio_skip_blanking(void)
{
	/*
	 * VisibleBars only hits sta AUDV0 on the eight dual-line kernels, not on
	 * every visible scanline or during RIOT blanking.  Advance the cursor to
	 * the end of this field's sound[] so the next load stays aligned and we
	 * do not replay the blanking tail on the following frame.
	 */
	if (!r_coreInfo.frameInfo.numBlocks)
		return;

	if (r_coreInfo.frameInfo.totalLines <= r_coreInfo.frameInfo.visibleLines)
		return;

	r_coreInfo.frameInfo.audioBuf = mc_display_audio_base()
		+ r_coreInfo.frameInfo.totalLines;
}

static void
mc_visible_line_advance(void)
{
	r_coreInfo.frameInfo.graphBuf += 10;
	r_coreInfo.frameInfo.colorBuf += 10;
	frameLines += 2;
}

static void
mc_on_visible_bars_entry(void)
{
	/* Title still swaps here. Playback commits the new buffer during
	 * blanking after DMA validation, before VisibleBars starts. */
	if (!mc_playback_pipeline)
		mc_field_swap_to_display();
	mc_audio_begin_visible();
	mc_blanking_window = 0;
	mc_visible_bars_vended = 1;
}

static void
mc_on_visible_bars_rts(void)
{
	mc_blanking_window = 1;
	mc_blanking_window_gen++;
	mc_visible_bars_vended = 0;
	mc_audio_skip_blanking();

	if (frameLines < 250 || frameLines > 275)
		DIAG_NOTE(DIAG_FRAME_LENGTH);
	frameLines = 0;
#if MOVIECART_STALL_TEST != 1 && MOVIECART_STALL_TEST != 2
	diagFrameTick();
#endif
	r_coreInfo.mr_endFrame = true;
}


HOTFUNC void
bus_dispatch(uint16_t cart_off, uint8_t addr_low8)
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
		&&g0xa0, &&g0xa1, &&g0xa2, &&g0xa3, &&g0xa4, &&g0xa5, &&g0xa6, &&g0xa7, &&g0xa8, &&g0xa9, &&g0xaa, &&g0xab, &&g0xac, &&g0xad, &&g0xae,
		&&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld,
		&&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld,
		&&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld,
		&&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld,
		&&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld,
		&&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld,
		&&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld,
		&&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld,
		&&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld,
		&&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld, &&mc_cart_rom_ld,
		&&g0xfc, &&g0xfd, &&g0xfe, &&g0xff
	};

	mc_cart_off = cart_off;

	/*
	 * VisibleBars scanlines: eight inlined kernel pairs from core.asm. Only
	 * the lda # / ldx # / ldy # slots need buffer patches; the cycle-accurate
	 * tail of each line (HMP, jmp next line) is served verbatim from core.bin.
	 */
	if (cart_off >= MC_OFF_LINE0 && cart_off < MC_OFF_END_LINES) {
		uint16_t phase = (uint16_t)((cart_off - MC_OFF_LINE0) % MC_LINE_CYCLE);

		if (phase <= MC_PHASE_DYNAMIC_MAX)
			goto *romData[0x13Eu + phase];

		SET_DATA(mc_core_rom[cart_off]);
		if (phase == 0x76u)
			mc_visible_line_advance();
		EMULATE_DONE
	}

	if (cart_off == MC_OFF_VISIBLE_ENTRY) {
		SET_DATA(mc_core_rom[cart_off]);
		mc_on_visible_bars_entry();
		EMULATE_DONE
	}

	if (cart_off == MC_OFF_VISIBLE_RTS) {
		SET_DATA(mc_core_rom[cart_off]);
		mc_on_visible_bars_rts();
		EMULATE_DONE
	}

	SET_DATA(mc_core_rom[cart_off]);
	EMULATE_DONE

gstore:
	*r_coreInfo.storeAddress = addr_low8;
	/*
	 * Joystick LDA $FE00,X uses this page for address bits, not data; serve
	 * zero (stable PAL 7800 bios behaviour).
	 */
	SET_DATA(0);
	EMULATE_DONE

mc_cart_rom_ld:
	SET_DATA(mc_core_rom[mc_cart_off]);
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
