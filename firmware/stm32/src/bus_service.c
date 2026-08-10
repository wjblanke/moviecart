#include "bus_service.h"
#include "defines.h"
#include "cartridge_io.h"
#include "core.h"

#include "stm32f4xx.h"

/*
 * Verbatim UnoCart-2600 driver_4k.c control flow. The only substitution is
 * MovieCart's dynamic dispatch in place of cart_rom[addr & 0xfff].
 *
 * Nothing may be added between the address read and the ODR write: an earlier
 * experiment that inserted a ~60 ns settle window here to reject transient
 * addresses killed the display outright, so the path is already close to the
 * 6502's data-valid deadline.
 */
RAMFUNC __attribute__((noreturn)) void
emulate_cartridge(void)
{
	__disable_irq();

	/*
	 * Do not try to suppress a repeated address here. It looks free and it
	 * is not: skipping the dispatch on a repeat means skipping a dispatch
	 * the kernel needed, which breaks the line counter far more often than
	 * the doubling it was meant to prevent. Measured on hardware — it turned
	 * an occasional glitch into a permanent one.
	 */
	uint16_t addr, addr_prev = 0;
	while (1)
	{
		while ((addr = ADDR_IN) != addr_prev)
			addr_prev = addr;
		/* got a stable address */
		if (addr & 0x1000)
		{ /* A12 high */
			bus_dispatch((uint16_t)(addr & 0x1ffu),
				     (uint8_t)(addr & 0xffu));
			SET_DATA_MODE_OUT
			/* wait for address bus to change */
			while (ADDR_IN == addr) ;
			SET_DATA_MODE_IN
		}
	}
}

RAMFUNC void
bus_serve_cycle(void)
{
	static uint16_t addr_prev;
	uint16_t addr;

	while ((addr = ADDR_IN) != addr_prev)
		addr_prev = addr;
	if (addr & 0x1000) {
		bus_dispatch((uint16_t)(addr & 0x1ffu),
			     (uint8_t)(addr & 0xffu));
		SET_DATA_MODE_OUT
		while (ADDR_IN == addr) ;
		SET_DATA_MODE_IN
	}
}
