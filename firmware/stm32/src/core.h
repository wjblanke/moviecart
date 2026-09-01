
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

	// following only used by interrupt code

	uint_fast8_t	peekBus;
	volatile uint_fast8_t*	storeAddress;
	uint_fast8_t	breakLoops;
	volatile uint_fast8_t	lines;

	uint_fast8_t	hiAddress;
	uint_fast8_t	vblankState;
	uint_fast8_t	vsyncState;
	volatile uint_fast8_t	endState;
	volatile uint_fast16_t	nextLineJump;
	uint_fast8_t	data;

	bool			audioPushed;
	uint_fast8_t	audioVal;

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

/*
 * RamKernel frame markers (see core.asm / core.bin):
 *
 *   mc_visible_bars_vended  set when the cart serves VisibleBars ($F09D) — the
 *                          6502 just finished blanking in RIOT and is entering
 *                          the cart-visible section for this frame.
 *
 *   mc_blanking_window     set when VisibleBars RTS ($F41D) is served — visible
 *                          is done, blanking runs from RIOT, and the MCU is
 *                          free for SDIO while RamKernel runs from RIOT $80.
 *
 *   mc_blanking_window_gen increments on each $F41D edge. SDIO initiation and
 *                          polling wait for gen to advance (see moviecart_sdio_gate),
 *                          except during playback field loads (mc_sdio_gate_relaxed).
 */
extern volatile uint8_t		mc_visible_bars_vended;
extern volatile uint8_t		mc_blanking_window;
extern volatile uint16_t	mc_blanking_window_gen;
extern volatile uint8_t		mc_sdio_gate_relaxed;
extern volatile uint8_t		mc_playback_pipeline;
extern volatile uint8_t		mc_buffer_swap_ready;

#endif
