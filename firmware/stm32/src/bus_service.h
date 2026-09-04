#ifndef BUS_SERVICE_H
#define BUS_SERVICE_H

#include <stdint.h>

/* Infinite UnoCart polling loop; used for boot/title lock. */
void emulate_cartridge(void) __attribute__((noreturn));

/* Serve one cart cycle synchronously from the main polling path. */
void bus_serve_cycle(void);

/*
 * Same tight loop as emulate_cartridge() until $F1C1 (mr_endFrame) or timeout.
 * Cart cycles have no extra work; end-frame and the deadline are sampled only
 * on A12-low. Returns 1 on the first complete frame.
 */
int bus_wait_endframe(uint32_t timeout_ms);

/* Same loop for a wall-clock hold. Title stays up; no SD or other work. */
void bus_serve_ms(uint32_t ms);

/* Counted blanking-edge LED code via moviecart_wait_blanking_start(). */
void moviecart_stage_blink(uint8_t n);

/*
 * There is no bounded variant. Guarding the tristate wait costs every cycle and
 * the shortened-guard version latched itself on and cut live cycles short; the
 * measured budget (one missed cycle garbles, ~100 ns of slack per cycle) does not
 * allow paying for it. A dead console therefore silences the diagnostics.
 */

/* MovieCart kernel dispatch (core.c). */
void bus_dispatch(uint16_t lo_address);

#include "defines.h"

#if MOVIECART_GAP_PROBE
/*
 * Worst-gap probe (make GAP_PROBE=1).
 *
 * Measures the elapsed time between leaving one served cycle and entering the
 * next, keeps the maximum, and records which labelled region of the SD path was
 * executing at the time. That turns "something is still too slow" into a
 * specific answer, reported over the status LED, instead of another guess.
 *
 * Regions are deliberately coarse — five, so the blink count stays countable,
 * and each one maps to a different kind of fix:
 *   1 pre-SD (title sync and the LED blinks themselves)
 *   2 SDIO pin configuration (SD_LowLevel_DeInit / SD_LowLevel_Init)
 *   3 SDIO peripheral register configuration (SDIO_DeInit / SDIO_Init)
 *   4 card command sequences (PowerON, InitializeCards, Select, wide bus)
 *   5 CSD/CID unpacking (SD_GetCardInfo)
 */
#define MC_PHASE_PRE_SD		1
#define MC_PHASE_PINS		2
#define MC_PHASE_SDIO_REGS	3
#define MC_PHASE_COMMANDS	4
#define MC_PHASE_CARDINFO	5
/*
 * Per-sector read path (6..8). Init happens once; these run once per sector, so
 * they are the regions whose gap scales with the number of reads — which is the
 * one thing every failure so far has scaled with.
 *   6 read command + DMA arm (SD_ReadBlock)
 *   7 DMA drain wait      (SD_WaitReadOperation)
 *   8 post-read card-ready poll (sd_wait_card_ready / SD_GetStatus CMD13)
 */
#define MC_PHASE_READ_CMD	6
#define MC_PHASE_READ_DRAIN	7
#define MC_PHASE_CARD_READY	8

extern volatile uint8_t mc_probe_phase;
extern uint32_t mc_gap_max_cycles;
extern uint8_t mc_gap_worst_phase;

#define MC_PROBE(n)	do { mc_probe_phase = (n); } while (0)

/* Blink the worst region and its gap size, then hand over to the plain loop. */
void mc_probe_report(void) __attribute__((noreturn));
#else
#define MC_PROBE(n)	do { } while (0)
#endif

#endif /* BUS_SERVICE_H */
