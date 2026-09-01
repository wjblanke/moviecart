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
 */
HOTFUNC __attribute__((noreturn)) void
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
			bus_dispatch((uint16_t)(addr & 0xfffu));
			SET_DATA_MODE_OUT
#if MOVIECART_STALL_TEST == 3
			bus_interleave_step();
#endif
			/* wait for address bus to change */
			while (ADDR_IN == addr) ;
			SET_DATA_MODE_IN
		}
	}
}

static uint16_t serve_addr_prev;

#if MOVIECART_GAP_PROBE
volatile uint8_t mc_probe_phase;
uint32_t mc_gap_max_cycles;
uint8_t mc_gap_worst_phase;

static uint32_t probe_last_exit;
static uint8_t probe_primed;

static inline __attribute__((always_inline)) void
probe_enter(void)
{
	uint32_t gap;

	/*
	 * Skip the first sample. probe_last_exit starts at zero, so the first
	 * measurement would be "time since reset" — millions of cycles — and would
	 * pin the maximum forever against a phase that never ran. That artifact
	 * cost a measurement round trip; a probe that lies is worse than none.
	 */
	if (!probe_primed)
		return;

	gap = DWT->CYCCNT - probe_last_exit;

	if (gap > mc_gap_max_cycles) {
		mc_gap_max_cycles = gap;
		mc_gap_worst_phase = mc_probe_phase;
	}
}

static inline __attribute__((always_inline)) void
probe_exit(void)
{
	probe_last_exit = DWT->CYCCNT;
	probe_primed = 1;
}
#else
#define probe_enter()	do { } while (0)
#define probe_exit()	do { } while (0)
#endif

HOTFUNC void
bus_serve_cycle(void)
{
	uint16_t addr;

	probe_enter();
	while ((addr = ADDR_IN) != serve_addr_prev)
		serve_addr_prev = addr;
	if (addr & 0x1000) {
		bus_dispatch((uint16_t)(addr & 0xfffu));
		SET_DATA_MODE_OUT
#if MOVIECART_STALL_TEST == 3
		bus_interleave_step();
#endif
		while (ADDR_IN == addr) ;
		SET_DATA_MODE_IN
	}
	probe_exit();
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
 * During visible, always serve. During RIOT blanking skip A12-low cycles (TIA/
 * RIOT/WSYNC only) so SDIO polls run faster — but still serve A12-high cart
 * fetches. VisibleBars entry ($F09D) is a cart fetch that clears
 * mc_blanking_window; missing it leaves the flag stuck at 1 and visible dead.
 */
void
moviecart_bus_yield(void)
{
	if (!mc_blanking_window || (ADDR_IN & 0x1000))
		bus_serve_cycle();
}

/*
 * First call snapshots the current gen so we always wait for the *next* $F1C1
 * edge, even if the kernel has already completed many frames before mount.
 */
void
moviecart_sdio_gate(void)
{
	static uint16_t seen;
	static uint8_t primed;

	if (mc_sdio_gate_relaxed) {
		while (!mc_blanking_window)
			bus_serve_cycle();
		/* A later strict operation must wait for an edge newer than the
		 * relaxed window, not consume this already-active one. */
		seen = mc_blanking_window_gen;
		primed = 1;
		return;
	}

	if (!primed) {
		seen = mc_blanking_window_gen;
		primed = 1;
	}
	while (mc_blanking_window_gen == seen)
		bus_serve_cycle();
	seen = mc_blanking_window_gen;
}

/*
 * Serve one cycle; do SD/DMA housekeeping only in cycles that can never miss a
 * cart fetch. A cart-read cycle (A12 high) is served exactly like the bounded
 * serve, with nothing added before the dispatch. A non-cart cycle (A12 low)
 * drives no data at all, so its entire ~838 ns is free for one `work` step —
 * this is the interleave-proof result taken to its safe limit. See the header.
 */
/*
 * A cart-read cycle (A12 high) is served *byte-identically* to bus_serve_cycle:
 * nothing is added anywhere in it. Work happens only in cycles where the cart is
 * not addressed and drives nothing, so its whole ~838 ns is ours.
 *
 * The previous version also ran work every 16th cart cycle, inside the drive
 * window, to guarantee progress if free cycles never came. That cost the drive
 * window a `pump_tick` load/increment/store on *every* cart cycle plus an
 * indirect call on every 16th — comfortably over the ~100 ns per-cycle budget the
 * fine sweep measured. It went unnoticed because per cycle it is tiny; over the
 * ~700k cycles of a single LED blink it reliably jammed the 6507. SD_STAGE=1,
 * which executes no SD code at all, went black right after its one blink while
 * the NO_SD baseline was pixel-perfect. That is the whole proof.
 *
 * Free cycles are not rare on a running console — the kernel's TIA and RAM
 * accesses are all A12-low — so polling still makes brisk progress. If the 6507
 * is jammed there are no free cycles and no progress, which is the accepted
 * trade: not damaging a healthy console outranks reporting on a dead one.
 *
 * work() must stay short. It runs in a cycle the cart is not driving, but the
 * next cycle's address still has to be caught, so keep steps to a fraction of
 * 838 ns (a status-register read, a counter compare) — never a loop.
 */
#if MOVIECART_PUMP_ALIGN
/*
 * Which A12-low address a work step has already been spent on.
 *
 * Bounding a work step's *duration* is not enough; its *position* in the cycle
 * matters just as much, and nothing above controlled it. The caller re-enters the
 * pump the moment work() returns, so a single 838 ns free cycle absorbed several
 * steps in a row — and the last one straddled the boundary into the next cycle. If
 * that next cycle was a cart fetch, the dispatch started late by a whole work step
 * (~150-250 ns for an indirect call plus an APB2 read). The identical step is
 * harmless at the start of a free cycle and fatal at the end of one, so every step
 * was a coin flip, which is exactly the shape of the observed failures: never
 * reproducible, always scaling with the number of sectors read.
 *
 * Spending at most one step per address means a step only ever begins just after
 * an address change was detected, i.e. at a cycle boundary with the full ~838 ns
 * ahead of it. The drain rate barely changes (the 6507 changes address nearly
 * every cycle, so free cycles still yield about one step each) but the straddle
 * disappears.
 *
 * If the console is halted on a static A12-low address no step ever runs, so
 * callers make no progress and fall back on their iteration ceilings. That is the
 * same trade already made everywhere else here: never damage a healthy console to
 * report on a dead one.
 */
static uint16_t pump_work_addr = 0xffffu;
#endif

HOTFUNC void
moviecart_bus_pump(void (*work)(void))
{
	uint16_t addr;

	probe_enter();
	while ((addr = ADDR_IN) != serve_addr_prev)
		serve_addr_prev = addr;
	if (addr & 0x1000) {
		bus_dispatch((uint16_t)(addr & 0xfffu));
		SET_DATA_MODE_OUT
		while (ADDR_IN == addr)
			;
		SET_DATA_MODE_IN
	} else if (work) {
#if MOVIECART_PUMP_ALIGN
		if (addr != pump_work_addr) {
			pump_work_addr = addr;
			work();
		}
#else
		work();
#endif
	}
	probe_exit();
}

/*
 * Timed serving with no per-cycle gap.
 *
 * The obvious form — `while (DWT->CYCCNT - start < limit) yield();` — reads the
 * cycle counter immediately after the yield returns, and the yield returns the
 * instant the bus steps to the next cycle. That read therefore lands inside the
 * new cycle and eats into the 6507's data-valid deadline. One such gap is
 * survivable; a 150 ms blink or a card-init wait is >100k cycles of them, and
 * the misses accumulate into a jam. So the deadline test runs in the free
 * (A12-low) window via the pump, and the loop body is a single SRAM load.
 */
static volatile uint32_t delay_expired;
static uint32_t delay_start, delay_limit;

static void
delay_check(void)
{
	if ((DWT->CYCCNT - delay_start) >= delay_limit)
		delay_expired = 1;
}

void
moviecart_delay_ms(uint32_t ms)
{
	/*
	 * Loose escape hatch for the case where free cycles are scarce enough that
	 * the deadline check is starved. It cannot rescue a fully jammed console —
	 * the pump's tristate wait is unbounded again, by design — so this is only
	 * a backstop against a pathologically A12-high-heavy stretch. It is a
	 * register decrement, so it costs the serve loop nothing.
	 */
	uint32_t guard = ms * 4000u + 4000u;

	delay_start = DWT->CYCCNT;
	delay_limit = (SystemCoreClock / 1000u) * ms;
	delay_expired = 0;

	while (!delay_expired && --guard)
		moviecart_bus_pump(delay_check);
}

/*
 * There is deliberately no interrupt-driven bus server here. Serving PE0-PE12
 * address-change EXTI at priority 0 was measured on hardware: black display and
 * continuous end-line wraps. Main must stay inside the polling loop. SDIO work
 * yields back here via moviecart_bus_yield() from every busy-wait.
 */
