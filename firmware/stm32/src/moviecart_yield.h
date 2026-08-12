#ifndef MOVIECART_YIELD_H
#define MOVIECART_YIELD_H

#include <stdint.h>

/*
 * Serve one Atari cart cycle, then return. Call from every SDIO/FatFs busy-wait
 * and from CPU-bound loops that would otherwise miss a fetch. With IRQs masked
 * for the bus, this is also how SDIO/DMA completion is kept moving — the wait
 * sites pump the SDIO handlers, then yield.
 */
void moviecart_bus_yield(void);

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

/* DWT-timed delay that keeps yielding; replaces Delayms under IRQ-off serving. */
void moviecart_delay_ms(uint32_t ms);

#endif /* MOVIECART_YIELD_H */
