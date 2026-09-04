#ifndef MOVIECART_YIELD_H
#define MOVIECART_YIELD_H

#include <stdint.h>

/*
 * Give the bus back.
 *   mc_sd_strict && !relaxed  — mount/open: serve until the next $F1C1
 *   otherwise                 — one cycle; if visible, drain to this $F1C1
 * Title copy and LED waits are not strict.
 */
void moviecart_bus_yield(void);

/* Tight-serve until mc_blanking_window (may be mid-window). Playback only. */
void moviecart_wait_blanking(void);

/*
 * Tight-serve until the next $F1C1. Always a brand-new blanking period, never
 * the leftover of the current one. Use this for mount/open/init; duration
 * does not matter.
 */
void moviecart_wait_blanking_start(void);

/*
 * Block until SDIO may run:
 *   - default: moviecart_wait_blanking_start()
 *   - mc_sdio_gate_relaxed: already in this blanking (playback field load)
 */
void moviecart_sdio_gate(void);

/*
 * Serve one cycle, and on a non-cart (A12-low) cycle only — where the cart
 * drives nothing and the whole ~838 ns is free — run one step of `work`.
 *
 * Use this instead of `work(); moviecart_bus_yield();` in SD/DMA busy-waits.
 * Running the housekeeping before the serve puts it *inside* the next cycle's
 * data-valid deadline; running it only in cycles that never drive data means it
 * can never cause a missed cart fetch. `work` must still be a single short step
 * (well under one cycle), not an unbounded loop.
 */
void moviecart_bus_pump(void (*work)(void));

/* DWT-timed delay that keeps yielding. */
void moviecart_delay_ms(uint32_t ms);

#endif /* MOVIECART_YIELD_H */
