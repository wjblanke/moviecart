
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
 * Record a non-fatal diagnostic code (1-4 are the kernel's own; see core.c).
 * 5 means a field load needed a retry but recovered. Codes are kept as the
 * worst seen, but nothing reads them out since the heartbeat was removed.
 */
#define DIAG_FIELD_RETRY	5

extern void			mc_diag_note(uint8_t code);
extern void			mc_field_swap_to_display(void);

/*
 * RamKernel frame markers (see core.c):
 *
 *   inramkernel            true from the first RamKernel opcode fetch ($0080
 *                          after ColdStart, $0083 after OsHead's RTS) until the
 *                          last RamKernel cycle (jsr VisibleBars high byte,
 *                          $0082). Tracked on A12-low by mc_track_ram_kernel().
 *                          While true the MCU is free for SDIO: blanking runs
 *                          from RIOT with A12 low. moviecart_wait_ram_kernel_entry()
 *                          waits for its false → true edge. The entry also
 *                          sets up the next visible kernel
 *                          (mc_on_ram_kernel_entry()).
 *
 *   mc_ram_kernel_armed    set at $F053 (MC_ARMED_COLDSTART) / $F1C1
 *                          (MC_ARMED_FRAME); the next RIOT RAM fetch is the
 *                          RamKernel entry.
 *
 *   mc_coldstart_entry     true when the latest entry was ColdStart's
 *                          jmp $0080. That "window" is only the RamKernel's
 *                          jsr VisibleBars, about 5 bus cycles, so nothing that
 *                          follows an entry may assume the long blanking window.
 */
#define MC_ARMED_FRAME		1u
#define MC_ARMED_COLDSTART	2u

extern volatile uint8_t		mc_sdio_gate_relaxed;
extern volatile uint8_t		mc_playback_pipeline;
extern volatile bool		inramkernel;
extern volatile uint8_t		mc_ram_kernel_armed;
extern volatile bool		mc_coldstart_entry;

extern struct coreInfo		r_coreInfo;

/* Title/NO_SD field swap; call right after moviecart_wait_ram_kernel_entry(). */
void				mc_blanking_swap(void);

/* Next visible kernel's pointers and line count; see core.c. */
void				mc_on_ram_kernel_entry(void);

/* Silence the shown field and the blanking pack; call in the RamKernel window. */
void				mc_mute_display(void);

/* RIOT RAM $80–$FF with A8 low, so stack ($01xx) accesses never match. */
#define MC_RIOT_RAM_MASK	0x1f80u
#define MC_RIOT_RAM		0x0080u
/* jsr VisibleBars high byte: RamKernel's last cycle before the cart. */
#define MC_RIOT_EXIT		0x0082u

/*
 * Call on every A12-low pass of every bus loop. Idempotent, so repeat passes
 * on the same address are harmless.
 *
 * $0082 is also the RTS's final dummy read (at the return address minus one)
 * just before the $0083 fetch, so it must never count as the entry.
 *
 * The entry runs on a free cycle with the whole RIOT-only window ahead, so the
 * once-per-frame setup costs no cart deadline.
 */
static inline __attribute__((always_inline)) void
mc_track_ram_kernel(uint16_t addr)
{
	if ((addr & MC_RIOT_RAM_MASK) != MC_RIOT_RAM)
		return;
	if (addr == MC_RIOT_EXIT) {
		inramkernel = false;
	} else if (mc_ram_kernel_armed) {
		mc_coldstart_entry = (mc_ram_kernel_armed == MC_ARMED_COLDSTART);
		mc_ram_kernel_armed = 0;
		inramkernel = true;
		mc_on_ram_kernel_entry();
	}
}

#endif
