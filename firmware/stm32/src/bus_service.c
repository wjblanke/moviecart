#include "bus_service.h"
#include "moviecart_yield.h"
#include "defines.h"
#include "cartridge_io.h"
#include "core.h"

#include "stm32f4xx.h"

/*
 * No address settle here. Tried twice: ~240 ns via a counted loop applied on
 * both the acquisition and release paths, and ~54 ns (4 NOPs plus a confirming
 * re-read, verified in the disassembly) on the acquisition path alone. Both
 * blacked out the display completely. Delay before the drivers turn on is not
 * available at any size — reject transients some other way, or not at all.
 */

#if MOVIECART_STALL_TEST == 3
/*
 * One cooperative work step while the data pins are already driving. This is
 * the shape of the interleave proof: after SET_DATA_MODE_OUT, burn ≤100 ns,
 * then return to the tight address-change wait.
 */
#define INTERLEAVE_CYCLES	17u	/* 100 ns @ 168 MHz */

static inline __attribute__((always_inline)) void
bus_interleave_step(void)
{
	uint32_t start = DWT->CYCCNT;

	while ((DWT->CYCCNT - start) < INTERLEAVE_CYCLES)
		;
}
#endif

/*
 * Verbatim UnoCart-2600 driver_4k.c control flow. The only substitution is
 * MovieCart's dynamic dispatch in place of cart_rom[addr & 0xfff].
 *
 * Nothing may be added between the address read and the ODR write: an earlier
 * experiment that inserted a ~60 ns settle window here to reject transient
 * addresses killed the display outright, so the path is already close to the
 * 6502's data-valid deadline.
 *
 * Serves until inramkernel goes false → true: the 6507 has just fetched
 * RamKernel's first opcode and has the long RIOT-only blanking ahead. Called
 * while already in RamKernel, it waits for the next frame's entry. limit is a
 * DWT cycle budget from start, checked on A12-low only; 0 means no timeout and
 * folds away. Returns 1 on entry, 0 on timeout.
 */
static inline __attribute__((always_inline)) int
serve_until_ram_kernel(uint32_t start, uint32_t limit)
{
	/*
	 * Do not try to suppress a repeated address here. It looks free and it
	 * is not: skipping the dispatch on a repeat means skipping a dispatch
	 * the kernel needed, which breaks the line counter far more often than
	 * the doubling it was meant to prevent. Measured on hardware — it turned
	 * an occasional glitch into a permanent one.
	 */
	uint16_t addr, addr_prev = 0;
	bool was_out = !inramkernel;

	while (1)
	{
		while ((addr = ADDR_IN) != addr_prev)
			addr_prev = addr;

		/* got a stable address */
		if (addr & 0x1000)
		{ /* A12 high */
			bus_dispatch((uint16_t)(addr & 0xfffu));
			SET_DATA_MODE_OUT
#if MOVIECART_STALL_TEST == 3
			bus_interleave_step();
#endif
			/* wait for address bus to change */
			while (ADDR_IN == addr) ;
			SET_DATA_MODE_IN
		}
		else
		{
			mc_track_ram_kernel(addr);
			if (!inramkernel)
				was_out = true;
			else if (was_out)
				return 1;
			if (limit && (DWT->CYCCNT - start) >= limit)
				return 0;
		}
	}
}

/* Serve until the next RamKernel entry; no swap. Public callers use
 * moviecart_wait_ram_kernel_entry(). */
static HOTFUNC void
wait_ram_kernel(void)
{
	serve_until_ram_kernel(0, 0);
}

/* One frame per pass: wait for RamKernel, then swap the title field. */
HOTFUNC __attribute__((noreturn)) void
emulate_cartridge(void)
{
	while (1)
	{
		wait_ram_kernel();
		mc_blanking_swap();
	}
}

#define LED_REST_BLANKINGS	60u
#define LED_ON_BLANKINGS	20u
#define LED_OFF_BLANKINGS	20u

void
moviecart_stage_blink(uint8_t n)
{
	uint8_t i;

	for (i = 0; i < LED_REST_BLANKINGS; i++)
		moviecart_wait_ram_kernel_entry();

	while (n--) {
		moviecart_wait_ram_kernel_entry();
		TESTA0_LOW
		for (i = 0; i < LED_ON_BLANKINGS; i++)
			moviecart_wait_ram_kernel_entry();
		TESTA0_HIGH
		for (i = 0; i < LED_OFF_BLANKINGS; i++)
			moviecart_wait_ram_kernel_entry();
	}

	for (i = 0; i < LED_REST_BLANKINGS; i++)
		moviecart_wait_ram_kernel_entry();
}

static uint16_t serve_addr_prev;

/*
 * One serve, identical to emulate_cartridge(). on_free runs only on A12-low.
 * Do not put a function call in the A12-high path.
 */
#define BUS_SERVE_ONCE(on_free)						\
	do {								\
		uint16_t _addr;						\
		while ((_addr = ADDR_IN) != serve_addr_prev)		\
			serve_addr_prev = _addr;			\
		if (_addr & 0x1000) {					\
			bus_dispatch((uint16_t)(_addr & 0xfffu));	\
			SET_DATA_MODE_OUT;				\
			while (ADDR_IN == _addr)			\
				;					\
			SET_DATA_MODE_IN;				\
		} else {						\
			mc_track_ram_kernel(_addr);			\
			on_free;					\
		}							\
	} while (0)

int
bus_wait_endframe(uint32_t timeout_ms)
{
	uint32_t start = DWT->CYCCNT;
	uint32_t limit = (SystemCoreClock / 1000u) * timeout_ms;

	if (!limit)
		limit = 1;	/* 0 means "no timeout" to serve_until_ram_kernel */

	/*
	 * Only an entry that follows $F1C1 (OsHead's RTS back to $0083) proves a
	 * whole frame ran. ColdStart's jmp $0080 is also an entry, so skip it.
	 */
	do {
		if (!serve_until_ram_kernel(start, limit))
			return 0;
	} while (mc_coldstart_entry);

	mc_blanking_swap();
	return 1;
}

void
bus_serve_ms(uint32_t ms)
{
	uint32_t start = DWT->CYCCNT;
	uint32_t limit = (SystemCoreClock / 1000u) * ms;

	while (1) {
		int done = 0;

		BUS_SERVE_ONCE({
			if ((DWT->CYCCNT - start) >= limit)
				done = 1;
		});
		if (done)
			return;
	}
}

/*
 * There is no bounded variant any more, and no dead-bus guard.
 *
 * The idea was sound — a halted 6507 stops changing the address, the unbounded
 * wait above never returns, and every LED code is lost exactly when it is needed.
 * The implementation was a net loss twice over. The guard's `--guard` sits *in the
 * tristate wait*, so it delays tristate detection on every single cycle; the
 * shortened-guard optimisation then latched itself on and cut live cycles short
 * (see the README). And the measurements leave no room to pay for any of it: the
 * coarse sweep says a single missed Atari cycle garbles the picture, and the fine
 * sweep puts the whole per-cycle budget at ~100 ns.
 *
 * So every serve is now the one loop shape proven to run indefinitely without
 * damage — the one the NO_SD baseline runs. The cost is that a dead console
 * silences the diagnostics, which is the same trade already made for the LED
 * blinks: a diagnostic that damages the system it measures is worse than none.
 */
/*
 * Frame waits. Each one ends at a RamKernel entry and then runs the title field
 * swap there, so the title keeps alternating through SD init. bus_serve_ms()
 * (wall-clock LED codes) only tracks inramkernel; a frame spent entirely in it
 * shows the last field again.
 *
 * INIT_PHASE=2 (hold, then emulate) is clean; INIT_PHASE=3 (wait for a fresh
 * blanking, then emulate) failed once, so INIT_PHASE=3 never returns.
 */
void
moviecart_wait_ram_kernel_entry(void)
{
#if MOVIECART_INIT_PHASE == 3
	emulate_cartridge();
#endif
	wait_ram_kernel();
	mc_blanking_swap();
}

/*
 * Relaxed: the caller is part way through a list of steps that all run inside
 * the current RamKernel window, so leave the bus alone. Otherwise start the
 * next step at the beginning of a fresh RamKernel window.
 */
void
moviecart_bus_yield(void)
{
	if (mc_sdio_gate_relaxed)
		return;
	moviecart_wait_ram_kernel_entry();
}

/*
 * Frame-granular delay: the deadline is checked once per RamKernel entry, inside
 * the window. Waits for a fresh entry every pass, relaxed or not, so it always
 * serves the bus. At least one frame.
 */
void
moviecart_delay_ms(uint32_t ms)
{
	uint32_t start = DWT->CYCCNT;
	uint32_t limit = (SystemCoreClock / 1000u) * ms;

	do
		moviecart_wait_ram_kernel_entry();
	while ((DWT->CYCCNT - start) < limit);
}

/*
 * There is deliberately no interrupt-driven bus server here. Serving PE0-PE12
 * address-change EXTI at priority 0 was measured on hardware: black display and
 * continuous end-line wraps. Main must stay inside the polling loop. SDIO work
 * yields back here via moviecart_bus_yield() from every busy-wait.
 */
