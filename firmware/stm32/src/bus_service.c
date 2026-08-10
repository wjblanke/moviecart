#include "bus_service.h"
#include "defines.h"
#include "cartridge_io.h"
#include "core.h"

/*
 * Serve the Atari bus when A11+A12 select the upper cart window.
 * Must be called often (including from SD wait loops) so side-effectful
 * kernel addresses are only handled once per address change.
 */
void
bus_service(void)
{
	static uint32_t last_raw = 0xffffffffu;
	uint32_t raw = ADDR_IN;

	if ((raw & CART_ADDR_MASK) != CART_ADDR_SELECT) {
		SET_DATA_MODE_IN;
		last_raw = raw;
		return;
	}

	if (raw == last_raw)
		return;

	last_raw = raw;
	SET_DATA_MODE_OUT;
	bus_dispatch((uint16_t)(raw & 0x1ffu), (uint8_t)(raw & 0xffu));
}
