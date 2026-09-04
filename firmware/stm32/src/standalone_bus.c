#include <stdint.h>

#include "stm32f4xx.h"
#include "bus_service.h"
#include "moviecart_yield.h"

/*
 * Standalone-test replacements for Atari bus cooperation. SDIO and DMA remain
 * real; only waits that normally serve or synchronize with the 6507 are removed.
 */
void
moviecart_bus_yield(void)
{
}

void
moviecart_wait_blanking(void)
{
}

void
moviecart_wait_blanking_start(void)
{
}

void
moviecart_sdio_gate(void)
{
}

void
moviecart_bus_pump(void (*work)(void))
{
	if (work)
		work();
}

void
moviecart_delay_ms(uint32_t ms)
{
	uint32_t start = DWT->CYCCNT;
	uint32_t limit = (SystemCoreClock / 1000u) * ms;

	while ((DWT->CYCCNT - start) < limit)
		;
}

void
bus_serve_cycle(void)
{
}

int
bus_wait_endframe(uint32_t timeout_ms)
{
	(void)timeout_ms;
	return 1;
}

void
bus_serve_ms(uint32_t ms)
{
	moviecart_delay_ms(ms);
}

void
moviecart_stage_blink(uint8_t n)
{
	(void)n;
}

void
bus_dispatch(uint16_t lo_address)
{
	(void)lo_address;
}

__attribute__((noreturn)) void
emulate_cartridge(void)
{
	for (;;)
		;
}
