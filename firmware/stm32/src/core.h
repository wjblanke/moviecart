
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
	volatile uint_fast8_t	nextLineJump;
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
 * WaitCart handoff — borrowed from UnoCart-2600 (PrepareWaitCartRoutine).
 *
 * A short 6502 routine is copied into Atari zero page and the kernel's
 * end-of-line JMP is diverted into it for part of the vertical blank. While the
 * 6502 executes from RAM it makes no cartridge fetches at all, so the ARM owes
 * the bus nothing and can run a whole field read as ordinary blocking code.
 * See core.c for the routine and the protocol.
 */
#define MC_WAIT_IDLE		0u	/* normal kernel; ARM serves every cycle */
#define MC_WAIT_COPY		1u	/* streaming the routine into Atari RAM */
#define MC_WAIT_INSTALLED	2u	/* routine resident, no handoff pending */
#define MC_WAIT_ARMED		3u	/* divert into the trampoline this VBLANK */
#define MC_WAIT_RUNNING		4u	/* 6502 in RAM: the ARM is free */

extern volatile uint8_t		mc_wait_state;
extern volatile uint8_t		mc_wait_ready;

extern void			mc_wait_install(void);
extern void			mc_wait_handoff(void (*work)(void));

#endif
