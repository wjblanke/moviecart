
#include "core.h"
#include "defines.h"
#include "cartridge_io.h"
#include "bus_service.h"

/*
 * Cart layout (12-bit, addr & 0xfff). Visible is the original MovieCart
 * looping kernel (firmware/core.c g0x3e–g0xb6). Blanking is RamKernel
 * copied to RIOT $80 at ColdStart (source image at $F200).
 *
 *   $F000–$F04A  ColdStart (copy $F200 → $80, jmp $80)
 *   $F09D        nop — VisibleBars entry (once per jsr from RIOT)
 *   $F09E–$F116  right/left pair, jmp $F09E until lines == 0
 *   $F140–$F15F  32-byte packed tail (after the 6 cart overscan samples)
 *   $F160–$F1C1  overscan head: 5×6 + 2 copy, 6 AUDV0, joystick, WSYNC, RTS
 *   $F200+       RamKernel source (78 bytes, copied to $80)
 *   $FE00–$FEFF  gstore (SWCHA/SWCHB/INPT4/INPT5)
 *   $FFFA–$FFFF  reset → $F000
 *
 * Last picture pair jmps $F160 (same 3-cycle jmp as the loop). Joystick sits
 * on the 6th cart line, then WSYNC, so RTS + first Play stay under 76 cycles.
 * After VBLANK, an in-RIOT pad (ldx #7 / nop / 2×bit $00) matches the working
 * title-kernel arrival at $F09E. A12 stays low until jsr VisibleBars.
 *
 * RIOT after copy: kernel $80–$CD, pack $D0–$EF, FIELD $FB, AUDIDX $FC.
 * $FE/$FF are the 6502 stack (jsr VisibleBars / jsr Play). Do not store
 * FIELD or AUDIDX there. No preroll.
 *
 * jmp high byte is $F0 (not original $FF): 12-bit decode does not mirror
 * $FFxx onto $F0xx.
 */
#define MC_OFF_BOOT_END		0x04au
#define MC_OFF_VISIBLE_ENTRY	0x09du
#define ADDR_RIGHT_LINE		0x9eu
#define ADDR_RIGHT_LINE_HI	0xf0u
#define ADDR_END_LINES		0x60u
#define ADDR_END_LINES_HI	0xf1u
#define MC_OFF_PACK		0x140u
#define MC_RIOT_PACK		0xd0u
#define MC_AUD_HEAD_LINES	6u
#define MC_AUD_PACK_LEN		32u
#define MC_AUD_PACK_SAMPLES	64u
#define MC_OFF_OSHEAD		0x160u
#define MC_OFF_OSHEAD_AUD6	0x16cu
#define MC_OFF_OSHEAD_AUD5	0x199u
#define MC_OFF_OSHEAD_RTS	0x1c1u
#define MC_OFF_OSHEAD_FE0	0x1aeu
#define MC_OFF_OSHEAD_FE1	0x1b4u
#define MC_OFF_OSHEAD_FE2	0x1b9u
#define MC_OFF_OSHEAD_FE3	0x1beu
#define MC_OFF_RAMKERNEL	0x200u
#define MC_OFF_STORE		0x0e00u
#define MC_OFF_VECTORS		0x0ffau

#define SET_DATA(X)     do { DATA_OUT = ((uint16_t)(uint8_t)(X)) << 8; \
			     SET_DATA_MODE_OUT } while (0)

#define EMULATE_DONE    do { return; } while (0);

#define LINES_EXHAUSTED(n)	((n) == 0 || (n) > 250)

struct coreInfo r_coreInfo;

#define DIAG_NOMINAL		1
#define DIAG_WRAP_VISIBLE	2
#define DIAG_WRAP_ENDLINES	3
#define DIAG_FRAME_LENGTH	4

#define DIAG_SLOT_FRAMES	8
#define DIAG_GAP_FRAMES		30

#define DIAG_NOTE(code)	do { \
		if ((code) > diagWorst) \
			diagWorst = (code); \
	} while (0)

static uint8_t  diagWorst;
static uint8_t  diagShowing;
static uint16_t diagTick;
static uint16_t frameLines;

volatile uint8_t mc_led_host;

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
volatile uint8_t	mc_swap_pending;

/*
 * ColdStart only ($F000–$F04A). Copies 78 bytes from $F200 → $80, jmp $80.
 * TIA/RESP/HMOVE setup is unchanged; the copy source is the RamKernel image.
 */
static const uint8_t mc_boot_rom[MC_OFF_BOOT_END + 1]
	__attribute__((section(".ccmram"))) = {
	0x78, 0xd8, 0xa2, 0xff, 0x9a, 0xa9, 0x00, 0x95, 0x00, 0xca, 0xd0, 0xfb, 0xa9, 0x01, 0x85, 0x26,
	0xa9, 0xcf, 0x85, 0x0d, 0xa9, 0x33, 0x85, 0x0e, 0xa9, 0xcc, 0x85, 0x0f, 0xa2, 0x30, 0x85, 0x10,
	0xea, 0x85, 0x11, 0xa9, 0x06, 0x85, 0x04, 0xa9, 0x02, 0x85, 0x05, 0x86, 0x20, 0xa9, 0x20, 0x85,
	0x21, 0x85, 0x02, 0x85, 0x2a, 0xa2, 0x0c, 0xca, 0xd0, 0xfd, 0x85, 0x2b, 0xea, 0xea, 0xa2, 0x4d,
	0xbd, 0x00, 0xf2, 0x95, 0x80, 0xca, 0x10, 0xf8, 0x4c, 0x80, 0x00
};

/*
 * First 6 overscan lines. Lines 1–5: AUDV0 + 6 pack bytes. Line 6: AUDV0 +
 * 2 bytes + joystick + WSYNC, then RTS (next line is the first RamKernel Play).
 */
static const uint8_t mc_oshead[] __attribute__((section(".ccmram"))) = {
	0xa9, 0x00, 0x85, 0x1b, 0x85, 0x1c, 0x85, 0x1b, 0xa2, 0x00, 0xa0, 0x05,
	0xa9, 0x00, 0x85, 0x19, 0xbd, 0x40, 0xf1, 0x95, 0xd0, 0xe8, 0xbd, 0x40,
	0xf1, 0x95, 0xd0, 0xe8, 0xbd, 0x40, 0xf1, 0x95, 0xd0, 0xe8, 0xbd, 0x40,
	0xf1, 0x95, 0xd0, 0xe8, 0xbd, 0x40, 0xf1, 0x95, 0xd0, 0xe8, 0xbd, 0x40,
	0xf1, 0x95, 0xd0, 0xe8, 0x88, 0x85, 0x02, 0xd0, 0xd3, 0xa9, 0x00, 0x85,
	0x19, 0xbd, 0x40, 0xf1, 0x95, 0xd0, 0xe8, 0xbd, 0x40, 0xf1, 0x95, 0xd0,
	0xe8, 0xae, 0x80, 0x02, 0xbd, 0x00, 0xfe, 0xae, 0x82, 0x02, 0xbd, 0x00,
	0xfe, 0xa6, 0x0c, 0xbd, 0x00, 0xfe, 0xa6, 0x0d, 0xbd, 0x00, 0xfe, 0x85,
	0x02, 0x60
};

/*
 * RamKernel at RIOT $80. Remaining OS 23/24 + VS 3 + VB 37 from pack at $D0
 * (A12 low). Play is a plain nibble unpack (no BIT-skip). The phase pad after
 * VBLANK off makes the first visible HMOVE land three cycles later, at the
 * phase expected by the visible kernel. Even: 63 nibbles (drop 64th). Odd: 64.
 */
static const uint8_t mc_ram_kernel[] __attribute__((section(".ccmram"))) = {
	0x20, 0x9d, 0xf0, 0xe6, 0xfb, 0xa5, 0xfb, 0x4a, 0xa2, 0x17, 0x90, 0x01,
	0xe8, 0x20, 0xb5, 0x00, 0xa9, 0x02, 0x85, 0x00, 0xa2, 0x03, 0x20, 0xb5,
	0x00, 0x86, 0x00, 0xa9, 0x02, 0x85, 0x01, 0xa2, 0x25, 0x20, 0xb5, 0x00,
	0x86, 0x01, 0x86, 0xfc, 0xa2, 0x07, 0xca, 0xd0, 0xfd, 0xea, 0x24, 0x00,
	0x24, 0x00, 0x4c, 0x80, 0x00, 0xa5, 0xfc, 0x4a, 0xa8, 0xb9, 0xd0, 0x00,
	0x90, 0x04, 0x4a, 0x4a, 0x4a, 0x4a, 0x29, 0x0f, 0x85, 0x19, 0xe6, 0xfc,
	0x85, 0x02, 0xca, 0xd0, 0xe8, 0x60
};

/* Writable; cannot share .ccmram with the const boot/kernel images. */
static uint8_t mc_aud_pack[MC_AUD_PACK_LEN];

static void mc_pack_blanking_audio(void);

static inline void
diagFrameTick(void)
{
	uint16_t flashing = (uint16_t)diagShowing * DIAG_SLOT_FRAMES;

	if (mc_led_host) {
		diagTick = 0;
		return;
	}

	if (diagTick < flashing && (diagTick & (DIAG_SLOT_FRAMES - 1)) <
				   (DIAG_SLOT_FRAMES / 2))
		TESTA0_LOW
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
	mc_swap_pending = 0;

	r_coreInfo.mr_swcha = 0xff;
	r_coreInfo.mr_swchb = 0xff;
	r_coreInfo.mr_inpt4 = 0xff;
	r_coreInfo.mr_inpt5 = 0xff;

	r_coreInfo.lines = 190;
	r_coreInfo.nextLineJump = ADDR_RIGHT_LINE;
	r_coreInfo.nextLineJumpHi = ADDR_RIGHT_LINE_HI;
	r_coreInfo.storeAddress = &r_coreInfo.mr_swcha;
	r_coreInfo.data = 0;

	{
		unsigned i;

		for (i = 0; i < MC_AUD_PACK_LEN; i++)
			mc_aud_pack[i] = 0;
	}

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
	mc_pack_blanking_audio();
}

static void
mc_pack_blanking_audio(void)
{
	const uint8_t *src;
	unsigned n, i;

	for (i = 0; i < MC_AUD_PACK_LEN; i++)
		mc_aud_pack[i] = 0;

	if (!r_coreInfo.frameInfo.audioBuf)
		return;
	if (r_coreInfo.frameInfo.totalLines <= r_coreInfo.frameInfo.visibleLines)
		return;

	n = (unsigned)(r_coreInfo.frameInfo.totalLines
		- r_coreInfo.frameInfo.visibleLines);
	if (n <= MC_AUD_HEAD_LINES)
		return;
	n -= MC_AUD_HEAD_LINES;
	if (n > MC_AUD_PACK_SAMPLES)
		n = MC_AUD_PACK_SAMPLES;

	src = r_coreInfo.frameInfo.audioBuf + r_coreInfo.frameInfo.visibleLines
		+ MC_AUD_HEAD_LINES;
	for (i = 0; i < n; i += 2) {
		uint8_t lo = src[i] & 0x0f;
		uint8_t hi = 0;

		if (i + 1u < n)
			hi = src[i + 1u] & 0x0f;
		mc_aud_pack[i / 2u] = (uint8_t)(lo | (uint8_t)(hi << 4));
	}
}

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
	r_coreInfo.frameInfo.audioBuf = mc_display_audio_base();
}

static void
mc_audio_skip_blanking(void)
{
	/*
	 * Visible + 6 overscan-head samples were consumed from the cursor.
	 * Snap to the end of the field. The remaining tail is packed at swap
	 * into mc_aud_pack (not read from this cursor).
	 */
	if (!r_coreInfo.frameInfo.numBlocks)
		return;

	if (r_coreInfo.frameInfo.totalLines <= r_coreInfo.frameInfo.visibleLines)
		return;

	r_coreInfo.frameInfo.audioBuf = mc_display_audio_base()
		+ r_coreInfo.frameInfo.totalLines;
}

static void
mc_on_visible_bars_entry(void)
{
	mc_audio_begin_visible();
	if (r_coreInfo.frameInfo.visibleLines)
		r_coreInfo.lines = r_coreInfo.frameInfo.visibleLines;
	r_coreInfo.nextLineJump = ADDR_RIGHT_LINE;
	r_coreInfo.nextLineJumpHi = ADDR_RIGHT_LINE_HI;
	mc_blanking_window = 0;
	mc_visible_bars_vended = 1;
}

/*
 * The title and NO_SD paths need a field swap once per frame, but the swap
 * copies pointer state and repacks blanking audio. Running that work while
 * serving $F09D overruns the next cart fetches at $F09E. Defer it until the
 * first A12-low cycle after OsHead's RTS, when the 6507 has entered the long
 * RIOT-only blanking kernel.
 */
void
mc_service_blanking_work(void)
{
	if (!mc_swap_pending)
		return;

	mc_swap_pending = 0;
	if (!mc_playback_pipeline)
		mc_field_swap_to_display();
}

static void
mc_on_visible_bars_rts(void)
{
	uint8_t expect = r_coreInfo.frameInfo.visibleLines;

	mc_blanking_window = 1;
	mc_blanking_window_gen++;
	mc_visible_bars_vended = 0;
	mc_swap_pending = 1;
	mc_audio_skip_blanking();

	/* Last pair does not increment frameLines (jmp to stub instead). */
	if (expect && (frameLines + 2 < expect - 4 || frameLines + 2 > expect + 4))
		DIAG_NOTE(DIAG_FRAME_LENGTH);
	frameLines = 0;
#if MOVIECART_STALL_TEST != 1 && MOVIECART_STALL_TEST != 2
	diagFrameTick();
#endif
	r_coreInfo.mr_endFrame = true;
	if (expect)
		r_coreInfo.lines = expect;
	r_coreInfo.nextLineJump = ADDR_RIGHT_LINE;
	r_coreInfo.nextLineJumpHi = ADDR_RIGHT_LINE_HI;
}


HOTFUNC void
bus_dispatch(uint16_t cart_off)
{
	/*
	 * $F09E–$F135: original kernel pair. Section required so
	 * this is not flash .rodata.
	 */
	static const void* const vis[0x135u - 0x09eu + 1]
		__attribute__((section(".ccmram"))) =
	{
		&&g0x3e, &&g0x3f, &&g0x40, &&g0x41, &&g0x42, &&g0x43, &&g0x44, &&g0x45, &&g0x46, &&g0x47, &&g0x48, &&g0x49, &&g0x4a, &&g0x4b, &&g0x4c, &&g0x4d,
		&&g0x4e, &&g0x4f, &&g0x50, &&g0x51, &&g0x52, &&g0x53, &&g0x54, &&g0x55, &&g0x56, &&g0x57, &&g0x58, &&g0x59, &&g0x5a, &&g0x5b, &&g0x5c, &&g0x5d,
		&&g0x5e, &&g0x5f, &&g0x60, &&g0x61, &&g0x62, &&g0x63, &&g0x64, &&g0x65, &&g0x66, &&g0x67, &&g0x68, &&g0x69, &&g0x6a, &&g0x6b, &&g0x6c, &&g0x6d,
		&&g0x6e, &&g0x6f, &&g0x70, &&g0x71, &&g0x72, &&g0x73, &&g0x74, &&g0x75, &&g0x76, &&g0x77, &&g0x78, &&g0x79, &&g0x7a, &&g0x7b, &&g0x7c, &&g0x7d,
		&&g0x7e, &&g0x7f, &&g0x80, &&g0x81, &&g0x82, &&g0x83, &&g0x84, &&g0x85, &&g0x86, &&g0x87, &&g0x88, &&g0x89, &&g0x8a, &&g0x8b, &&g0x8c, &&g0x8d,
		&&g0x8e, &&g0x8f, &&g0x90, &&g0x91, &&g0x92, &&g0x93, &&g0x94, &&g0x95, &&g0x96, &&g0x97, &&g0x98, &&g0x99, &&g0x9a, &&g0x9b, &&g0x9c, &&g0x9d,
		&&g0x9e, &&g0x9f, &&g0xa0, &&g0xa1, &&g0xa2, &&g0xa3, &&g0xa4, &&g0xa5, &&g0xa6, &&g0xa7, &&g0xa8, &&g0xa9, &&g0xaa, &&g0xab, &&g0xac, &&g0xad,
		&&g0xae, &&g0xaf, &&g0xb0, &&g0xb1, &&g0xb2, &&g0xb3, &&g0xb4, &&g0xb5, &&g0xb6,
		&&e0x117, &&e0x118, &&e0x119, &&e0x11a, &&e0x11b, &&e0x11c, &&e0x11d, &&e0x11e, &&e0x11f, &&e0x120, &&e0x121, &&e0x122, &&e0x123, &&e0x124,
		&&e0x125, &&e0x126, &&e0x127, &&e0x128, &&e0x129, &&e0x12a, &&e0x12b, &&e0x12c, &&e0x12d, &&e0x12e, &&e0x12f, &&e0x130, &&e0x131, &&e0x132,
		&&e0x133, &&e0x134, &&e0x135
	};

	if (cart_off >= MC_OFF_STORE && cart_off < MC_OFF_STORE + 0x100u)
		goto gstore;

	if (cart_off <= MC_OFF_BOOT_END) {
		SET_DATA(mc_boot_rom[cart_off]);
		EMULATE_DONE
	}

	if (cart_off == MC_OFF_VISIBLE_ENTRY) {
		SET_DATA(0xea);
		mc_on_visible_bars_entry();
		EMULATE_DONE
	}

	if (cart_off >= ADDR_RIGHT_LINE && cart_off <= 0x135u)
		goto *vis[cart_off - ADDR_RIGHT_LINE];

	if (cart_off >= MC_OFF_PACK &&
	    cart_off < MC_OFF_PACK + MC_AUD_PACK_LEN) {
		SET_DATA(mc_aud_pack[cart_off - MC_OFF_PACK]);
		EMULATE_DONE
	}

	if (cart_off >= MC_OFF_OSHEAD &&
	    cart_off < MC_OFF_OSHEAD + sizeof(mc_oshead)) {
		if (cart_off == MC_OFF_OSHEAD_AUD6 ||
		    cart_off == MC_OFF_OSHEAD_AUD5) {
			SET_DATA(0xa9);
			r_coreInfo.data = *r_coreInfo.frameInfo.audioBuf++;
			EMULATE_DONE
		}
		if (cart_off == MC_OFF_OSHEAD_AUD6 + 1u ||
		    cart_off == MC_OFF_OSHEAD_AUD5 + 1u) {
			SET_DATA(r_coreInfo.data);
			EMULATE_DONE
		}
		if (cart_off == MC_OFF_OSHEAD_FE0) {
			SET_DATA(0xfe);
			r_coreInfo.storeAddress = &r_coreInfo.mr_swcha;
			EMULATE_DONE
		}
		if (cart_off == MC_OFF_OSHEAD_FE1) {
			SET_DATA(0xfe);
			r_coreInfo.storeAddress = &r_coreInfo.mr_swchb;
			EMULATE_DONE
		}
		if (cart_off == MC_OFF_OSHEAD_FE2) {
			SET_DATA(0xfe);
			r_coreInfo.storeAddress = &r_coreInfo.mr_inpt4;
			EMULATE_DONE
		}
		if (cart_off == MC_OFF_OSHEAD_FE3) {
			SET_DATA(0xfe);
			r_coreInfo.storeAddress = &r_coreInfo.mr_inpt5;
			EMULATE_DONE
		}
		if (cart_off == MC_OFF_OSHEAD_RTS) {
			SET_DATA(0x60);
			mc_on_visible_bars_rts();
			EMULATE_DONE
		}
		SET_DATA(mc_oshead[cart_off - MC_OFF_OSHEAD]);
		EMULATE_DONE
	}

	if (cart_off >= MC_OFF_RAMKERNEL &&
	    cart_off < MC_OFF_RAMKERNEL + sizeof(mc_ram_kernel)) {
		SET_DATA(mc_ram_kernel[cart_off - MC_OFF_RAMKERNEL]);
		EMULATE_DONE
	}

	if (cart_off >= MC_OFF_VECTORS) {
		SET_DATA((cart_off & 1u) ? 0xf0 : 0x00);
		EMULATE_DONE
	}

	SET_DATA(0x00);
	EMULATE_DONE

gstore:
	*r_coreInfo.storeAddress = (uint_fast8_t)(cart_off & 0xffu);
	SET_DATA(0x00);
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
	SET_DATA(0x85); // sta HMOVE 	// 3 @03 +8 pixel
	EMULATE_DONE

g0x43:
	SET_DATA(0x2a);
	EMULATE_DONE

g0x44:
	SET_DATA(0xa9); // lda #AUD_DATA 	// 2
	r_coreInfo.data = *r_coreInfo.frameInfo.audioBuf++;
	EMULATE_DONE

g0x45:
	SET_DATA(r_coreInfo.data);
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
	SET_DATA(0x85);	// sta COLUBK 	// 3 background color
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
	SET_DATA(0x80);
	r_coreInfo.lines -= 2;
	EMULATE_DONE

g0xb0:
	SET_DATA(0x85);	// sta HMP0 	// 3
	if (r_coreInfo.lines == 0) {
		r_coreInfo.nextLineJump = ADDR_END_LINES;
		r_coreInfo.nextLineJumpHi = ADDR_END_LINES_HI;
	} else if (LINES_EXHAUSTED(r_coreInfo.lines)) {
		r_coreInfo.nextLineJump = ADDR_END_LINES;
		r_coreInfo.nextLineJumpHi = ADDR_END_LINES_HI;
		DIAG_NOTE(DIAG_WRAP_VISIBLE);
	} else {
		r_coreInfo.frameInfo.graphBuf += 10;
		r_coreInfo.frameInfo.colorBuf += 10;
		frameLines += 2;
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
	SET_DATA(0x4c); // jmp right_line / end stub
	EMULATE_DONE

g0xb5:
	SET_DATA(r_coreInfo.nextLineJump);
	EMULATE_DONE

g0xb6:
	SET_DATA(r_coreInfo.nextLineJumpHi);
	EMULATE_DONE

/* leftover $F117–$F135: unused; last pair jmps $F160 */
e0x117:
	SET_DATA(0xea);
	EMULATE_DONE
e0x118:
	SET_DATA(0xea);
	EMULATE_DONE
e0x119:
	SET_DATA(0xea);
	EMULATE_DONE
e0x11a:
	SET_DATA(0x1b);
	EMULATE_DONE
e0x11b:
	SET_DATA(0x85);
	EMULATE_DONE
e0x11c:
	SET_DATA(0x1c);
	EMULATE_DONE
e0x11d:
	SET_DATA(0x85);
	EMULATE_DONE
e0x11e:
	SET_DATA(0x1b);
	EMULATE_DONE
e0x11f:
	SET_DATA(0xae); // ldx SWCHA
	EMULATE_DONE
e0x120:
	SET_DATA(0x80);
	EMULATE_DONE
e0x121:
	SET_DATA(0x02);
	EMULATE_DONE
e0x122:
	SET_DATA(0xbd); // lda $FE00,x
	EMULATE_DONE
e0x123:
	SET_DATA(0x00);
	EMULATE_DONE
e0x124:
	SET_DATA(0xfe);
	r_coreInfo.storeAddress = &r_coreInfo.mr_swcha;
	EMULATE_DONE
e0x125:
	SET_DATA(0xae); // ldx SWCHB
	EMULATE_DONE
e0x126:
	SET_DATA(0x82);
	EMULATE_DONE
e0x127:
	SET_DATA(0x02);
	EMULATE_DONE
e0x128:
	SET_DATA(0xbd);
	EMULATE_DONE
e0x129:
	SET_DATA(0x00);
	EMULATE_DONE
e0x12a:
	SET_DATA(0xfe);
	r_coreInfo.storeAddress = &r_coreInfo.mr_swchb;
	EMULATE_DONE
e0x12b:
	SET_DATA(0xa6); // ldx INPT4
	EMULATE_DONE
e0x12c:
	SET_DATA(0x0c);
	EMULATE_DONE
e0x12d:
	SET_DATA(0xbd);
	EMULATE_DONE
e0x12e:
	SET_DATA(0x00);
	EMULATE_DONE
e0x12f:
	SET_DATA(0xfe);
	r_coreInfo.storeAddress = &r_coreInfo.mr_inpt4;
	EMULATE_DONE
e0x130:
	SET_DATA(0xa6); // ldx INPT5
	EMULATE_DONE
e0x131:
	SET_DATA(0x0d);
	EMULATE_DONE
e0x132:
	SET_DATA(0xbd);
	EMULATE_DONE
e0x133:
	SET_DATA(0x00);
	EMULATE_DONE
e0x134:
	SET_DATA(0xfe);
	r_coreInfo.storeAddress = &r_coreInfo.mr_inpt5;
	EMULATE_DONE
e0x135:
	SET_DATA(0xea);
	EMULATE_DONE
}
