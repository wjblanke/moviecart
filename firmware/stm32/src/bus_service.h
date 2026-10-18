#ifndef BUS_SERVICE_H
#define BUS_SERVICE_H

#include <stdint.h>

/* Infinite UnoCart polling loop; used for boot/title lock. */
void emulate_cartridge(void) __attribute__((noreturn));

/*
 * Same tight loop as emulate_cartridge() until the first RamKernel entry after
 * $F1C1 (not ColdStart's, see mc_coldstart_entry), or timeout; swaps the title field on that entry. The
 * ColdStart entry does not count. Cart cycles have no extra work; the deadline
 * is sampled only on A12-low. Returns 1 on the first complete frame.
 */
int bus_wait_endframe(uint32_t timeout_ms);

/* Same loop for a wall-clock hold. Title stays up; no SD or other work. */
void bus_serve_ms(uint32_t ms);

/* Counted blanking-edge LED code via moviecart_wait_ram_kernel_entry(). */
void moviecart_stage_blink(uint8_t n);

/*
 * There is no bounded variant. Guarding the tristate wait costs every cycle and
 * the shortened-guard version latched itself on and cut live cycles short; the
 * measured budget (one missed cycle garbles, ~100 ns of slack per cycle) does not
 * allow paying for it. A dead console therefore silences the diagnostics.
 */

/* MovieCart kernel dispatch (core.c). */
void bus_dispatch(uint16_t lo_address);

#endif /* BUS_SERVICE_H */
