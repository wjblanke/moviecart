#ifndef MOVIECART_YIELD_H
#define MOVIECART_YIELD_H

#include <stdint.h>

/*
 * Step boundary in a list of operations.
 *   mc_sdio_gate_relaxed — no-op: the whole list runs in this RamKernel window
 *   otherwise            — wait for the next RamKernel entry and swap the title
 *                          field, so the next step starts a fresh window
 */
void moviecart_bus_yield(void);

/*
 * Wait for the next RamKernel entry and swap the title field. Always a
 * brand-new blanking period, never the leftover of the current one. Use this
 * for mount/open/init; duration does not matter.
 */
void moviecart_wait_ram_kernel_entry(void);

/* DWT-timed delay, checked once per RamKernel entry; always serves the bus. */
void moviecart_delay_ms(uint32_t ms);

#endif /* MOVIECART_YIELD_H */
