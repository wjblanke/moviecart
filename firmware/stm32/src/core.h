
/*

Runs an interrupt on address change that feed the buffer to the screen, and updates frameNumber
Main thread needs only maintain the buffer.

*/

#ifndef __CORE__
#define __CORE__

#include <stdint.h>
#include <stdbool.h>

#include "defines.h"
#include "frame.h"

struct coreInfo
{
	// mr_* used by both main + interrupt

	volatile bool			mr_endFrame;
	volatile bool			mr_bufferIndex;
	volatile uint_fast8_t	mr_swcha;
	volatile uint_fast8_t	mr_swchb;
	volatile uint_fast8_t	mr_inpt4;
	volatile uint_fast8_t	mr_inpt5;	// not used
	struct frameInfo		mr_frameInfo1 , mr_frameInfo2;

	// following only used by the bus loop

	volatile uint_fast8_t	lines;
	uint_fast8_t	nextLineJump;
	uint_fast8_t	nextLineJumpHi;
	uint_fast8_t	data;
	volatile uint_fast8_t	*storeAddress;

	struct frameInfo	frameInfo;
};

extern void			coreInit();

/*
 * Non-zero while main.c is blinking a code on the status LED, telling the
 * kernel's once-per-frame heartbeat to leave the LED alone. See core.c.
 */
extern volatile uint8_t		mc_led_host;

/*
 * Raise a code on the kernel's non-fatal heartbeat channel (1-4 are the kernel's
 * own; see core.c). 5 means a field load needed a retry but recovered, so a healed
 * transient fault is visible instead of silent.
 */
#define DIAG_FIELD_RETRY	5

extern void			mc_diag_note(uint8_t code);
extern void			mc_field_swap_to_display(void);

/*
 * RamKernel frame markers (see core.c):
 *
 *   mc_visible_bars_vended  set when the cart serves VisibleBars ($F09D) — the
 *                          6502 just finished blanking in RIOT and is entering
 *                          the looping visible kernel for this frame.
 *
 *   mc_blanking_window     set when the overscan-head RTS ($F1C1) is served —
 *                          the 6 cart OS lines are done, blanking runs from
 *                          RIOT, and the MCU is free for SDIO while RamKernel
 *                          plays the packed tail (A12 low during OS/VS/VB).
 *
 *   mc_blanking_window_gen increments on each $F1C1 edge. SDIO initiation and
 *                          polling wait for gen to advance (see moviecart_sdio_gate),
 *                          except during playback field loads (mc_sdio_gate_relaxed).
 */
extern volatile uint8_t		mc_visible_bars_vended;
extern volatile uint8_t		mc_blanking_window;
extern volatile uint16_t	mc_blanking_window_gen;
extern volatile uint8_t		mc_sdio_gate_relaxed;
/* Non-zero only during mount/open. Yield then waits for the next $F1C1.
 * Title copy, LED waits, and playback updates must leave this clear. */
extern volatile uint8_t		mc_sd_strict;
extern volatile uint8_t		mc_playback_pipeline;
extern volatile uint8_t		mc_swap_pending;

extern struct coreInfo		r_coreInfo;

void				mc_service_blanking_work(void);

#endif
