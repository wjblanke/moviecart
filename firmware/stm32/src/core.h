
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
	/*
	 * Full jmp target: low byte in 0-7, high byte in 8-15. It is one value
	 * rather than a low byte plus a computed high byte because $FFxx and the
	 * RAM routine at $0084 do not share a page, and deciding the high byte
	 * inside the fetch that serves it put a volatile compare on the hottest
	 * path in the kernel.
	 */
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
 * WaitCart handoff — UnoCart's sequence, in the kernel ROM we own.
 *
 * After ClearMem the 6502 copies a wait routine from ROM into $84 and RTS.
 * nextLineJump points at $0084 only while ARMED. See core.c.
 */
#define MC_WAIT_IDLE		0u	/* boot; PrepareWait has not RTS'd */
#define MC_WAIT_INSTALLED	2u	/* routine resident in RIOT RAM */
#define MC_WAIT_ARMED		3u	/* next $FFF4 fetch parks; ARM is waiting */
#define MC_WAIT_RUNNING		4u	/* 6502 in RAM: the ARM is free */

extern volatile uint8_t		mc_wait_state;
extern volatile uint8_t		mc_wait_ready;

/*
 * Why an armed handoff failed to park. Non-zero means work() did NOT run.
 *
 *   1  2 s elapsed without the RAM routine fetching $FFF4. A live title after
 *      the LED goes solid is this case: the kernel never jumped to $84.
 *   2  The 6502 parked, work() ran, and the 6502 read READY and left RAM — but
 *      no end-of-frame arrived in the 250 ms after that. The park succeeded and
 *      the resume did not.
 *   3  work() ran but the 6502 never left RAM (never fetched READY).
 */
extern volatile uint8_t		mc_wait_fault;

extern void			mc_wait_install(void);
extern void			mc_wait_handoff(void (*work)(void));

#endif
