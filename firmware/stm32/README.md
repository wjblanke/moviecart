# MovieCart — STM32F407VGT6 (DevEBox)

Port of the MovieCart cart firmware to the **MCUDEV DevEBox STM32F407VGT6**, using the same cartridge pin map and onboard microSD wiring as the nearby [UnoCart-2600 DevEBox fork](../../../UnoCart-2600).

The original dsPIC33CK firmware remains under `firmware/` (parent of this directory).

## Wiring

| Atari | MCU |
|-------|-----|
| A0–A12 | **PE0–PE12** |
| D0–D7 | **PD8–PD15** |
| +5V / GND | Board 5V input / GND (MCU is 3.3 V; level-shift or series resistors as on your breakout) |

Onboard microSD (SDIO 4-bit):

| SD | MCU |
|----|-----|
| D0–D3 | PC8–PC11 |
| CK | PC12 |
| CMD | PD2 |

**Power:** DevEBox is powered from the Atari (same power-up).

The current build displays the embedded MovieCart title, then mounts the
onboard microSD and streams MovieCart fields. The first regular FAT32 root
directory file is played; the normal MovieCart controls and file selection
apply. SD work coexists with the UnoCart polling loop by yielding into
`bus_serve_cycle()` from every busy-wait (see "SD streaming" below).

The serving loop is copied in shape from UnoCart-2600's `emulate_cartridge()` (`source/STM32firmware/standalone/src/driver_4k.c`), which is known good on this exact board and wiring: converge on a stable address, write the byte to ODR **while the pins are still inputs**, then switch to output, then tristate once the address changes (not when A12 falls, so a following 6502 write cycle never meets our drivers). GPIO config for PD8-15 also matches UnoCart exactly: no pull, reset-default slew.

Do not add work between the address read and the moment the drivers turn on.
Inserting a ~60 ns settle window there — to reject the composite addresses that
appear while the lines slew, which MovieCart's stateful dispatch cannot shrug
off the way a plain ROM cart can — killed the display completely.

Do not add a "don't dispatch the same address twice in a row" filter either. It
was tried on the reasoning that the 6502 never fetches one cart address on
consecutive cycles, so a repeat must be our own data pins coupling into the
address lines. On hardware it made things strictly worse — a permanent black
screen and a constant 3-blink code — because it also suppresses dispatches the
kernel genuinely needed.

`SET_DATA()` in `core.c` enables the output drivers itself,
immediately after writing ODR, instead of leaving it to the caller. A dispatch
case runs its side effects *before* returning, so a `SET_DATA_MODE_OUT` in the
bus loop would be delayed by that case's entire workload. Light cases cost a few
cycles and survive; the end-of-frame chain at `$FFdb`–`$FFe3`, which reloads
every `frameInfo` pointer and restarts the line counter, does not — and those
are precisely the cycles that set up VSYNC.

The status LED remains the only practical diagnostic channel. `core.c` drives it
from the kernel's own end-of-frame, one slot per frame, blinking the worst
condition seen since the last report:

| Flashes | Meaning |
| --- | --- |
| 1 | nominal — frames closing normally, scanline count in range |
| 2 | line counter wrapped in the visible section (a dispatch was missed) |
| 3 | line counter wrapped in the end-lines section |
| 4 | frame was not 250–275 scanlines |

A frozen LED means the kernel stopped completing frames altogether.

Case 2 of the `$FFd9` state machine leaves `lines` at zero for the rest of that
scanline, until `$FFe3` loads `visibleLines`. Setting it to 1 there, to absorb a
spurious extra decrement, is functionally a no-op — and it still blacked out the
display. **The end-of-frame cases will not tolerate a single extra store.** Treat
`$FFd9` through `$FFe3` as closed to new work of any kind.

The kernel's line counter is stepped by the 6502's fetches, so one missed or
duplicated dispatch flips its parity, `lines -= 2` steps past zero and wraps,
and the frame never ends: buffer pointers advance forever, the picture becomes a
scroll through SRAM, and the read finally leaves SRAM and faults the CPU (which
looks like the console dying a few seconds in). `LINES_EXHAUSTED()` treats an
impossibly large counter as end-of-section, so the frame closes, every pointer
is reloaded from `mr_frameInfo1/2`, and the kernel resyncs instead of running
away.

The bus loop runs in main with interrupts disabled for the entire run, exactly
like UnoCart. There is no timer or EXTI bus server.

## SD streaming

`MOVIECART_ENABLE_SD` is on. After title lock, main mounts the card, runs the
title hold, then loads fields into the inactive buffer each frame.

### Yield-to-bus, not interrupt serving

Interrupt-driven cart serving was measured and failed (black screen, continuous
3-blink). The working design keeps the UnoCart polling loop as the only bus
server and **yields into it** whenever SD or FatFs would otherwise busy-spin:

- Every SDIO command/response wait (`CmdResp*Error`, `CmdError`) calls
  `moviecart_bus_yield()` → `bus_serve_cycle()`.
- DMA completion is pumped by hand (`SD_ProcessIRQSrc` /
  `SD_ProcessDMAIRQ`) because IRQs stay masked for the Atari; each poll yields.
- `Delayms` during card init is replaced by `moviecart_delay_ms` (DWT + yield).
- FatFs cluster hops and directory walks yield once per step.
- `updateBuffer` processes a few bytes, then yields.

SDIO/DMA NVIC lines are left disabled after init so they cannot preempt
dispatch. Sector payloads still move by DMA2 Stream 3 in the background.

### Status LED codes

PA1 has exactly one owner at a time. During boot, main blinks progress; once
playback starts the kernel's end-of-frame diagnostic owns it (1 flash/sec when
healthy). `runFrameLoop` deliberately does **not** touch the LED — a
once-per-frame write from there as well made the count unreadable.

Every blink keeps serving the bus, so leaving these in costs nothing.

| Flashes | Meaning |
| --- | --- |
| 1 | title synchronized |
| 2 | card mounted |
| 3 | movie file opened |
| 4 | first field header valid, playback starting |
| 5 | mount failed (fatal) |
| 6 | title never synchronized (fatal) |
| 7 | bad field header or size (fatal) |
| 8 | no playable file found (fatal) |

A field is loaded only into the buffer not currently displayed. The first sector
must begin with `MVC\0` and the field must occupy 1–6 sectors, otherwise
playback stops with code 7 rather than writing past the 3 KiB field buffer.

### A dead console must not be able to silence the LED

`bus_serve_cycle()` ends with `while (ADDR_IN == addr);`, waiting for the 6507 to
move on. Once the console has crashed that address never changes, so the wait
never returns — and since both the LED blinks and every SD busy-wait went
through it, a crash wedged the firmware with the LED dark. That is exactly the
"random stripes and no blinks at all" failure: the diagnostic channel died with
the console.

`bus_serve_cycle_bounded()` is the same cycle with a guard counter (~ms, versus
the 838 ns a live console needs) on that wait. LED codes and SD waits use it;
the display path keeps the unbounded loop, because a guard counter there would
delay tristate detection on the timing-critical path.

### Bisecting SD bring-up

`make clean && make SD_STAGE=N` stops after a milestone and hands the bus back to
the plain title loop, so the failing layer can be identified in one flash each
instead of guessed at:

| Stage | Stops |
| --- | --- |
| 1 | after title sync, before mount |
| 2 | after mount |
| 3 | after file open |
| 0 (default) | full playback |

Each stage blinks its milestone codes, then shows the title with the kernel's
1-flash-per-second heartbeat. A stage that displays cleanly proves every layer
up to that point coexists with bus serving.

### Every SD wait must yield — including the FIFO drains

The first SD-enabled build mostly showed vertical colour stripes, occasionally a
garbled movie, then hung. The cause was an unbounded, non-yielding FIFO drain in
`SD_FindSCR` (reached from `SD_EnableWideBusOperation` during card init): it
starved the bus for the whole SCR read, and if an end flag never arrived it spun
forever. The same shape existed in `SD_SendSDStatus` and the high-speed switch.
All three now yield per iteration and are bounded by `SD_DATATIMEOUT`.

Yielding inside those drains is safe because the payloads (8 and 64 bytes) are
far smaller than the 32-word RX FIFO, so the FIFO cannot overrun while the Atari
is being served.

### Plain CPU work starves the bus just as badly as a wait

With every SD *wait* yielding, stage 2 still failed most of the time: two blinks
(so the mount itself succeeded) followed by permanent vertical stripes. That
combination is diagnostic. `flash_led` uses the bounded server, so it keeps
blinking even after the 6507 is gone; the stripes afterwards mean the 6507 had
already died *during* the mount and no amount of correct serving afterwards can
revive it, because nothing on the cart port can reset the console. So the mount
was starving the bus somewhere that was not a wait at all.

The remaining offenders were ordinary straight-line code, which is easy to
overlook because it contains nothing that looks like blocking:

- `pf_mount` clears the 128-entry block/cluster cache — 256 stores, roughly
  5 µs, or six consecutive missed fetches. This was almost certainly the main
  cause of the intermittency, and `checkSelectVideo` has the same loop.
- `SD_GetCardInfo` unpacks CSD and CID as one long bitfield-extraction run.
- `SD_LowLevel_Init`'s `TM_GPIO_InitAlternate` calls loop over their pins.
- `SD_LowLevel_DMA_RxConfig` runs `DMA_DeInit` plus `DMA_Init` on *every*
  sector, so this one also affects playback, not just bring-up. Note the fix
  claimed below was inadequate for a long time: yields were added *between* those
  calls but not inside them, and each call is itself microseconds of register work.
  See "Per-sector DMA setup" for what it took to actually fix.
- `checkSelectVideo` zeroes both field buffers with two `memset`s — several
  kilobytes mid-playback.

All of these now yield periodically. The general rule for this port: any
uninterrupted run of more than a few hundred instructions is a bus fault
waiting to happen, whether or not it waits on hardware.

Why it was intermittent rather than always broken: a missed fetch is only fatal
depending on where the 6507 happens to be. Land in the kernel's wait loop and it
resynchronizes; land mid-instruction and it executes whatever the floating data
bus supplied, which sometimes wanders back and sometimes jams.

### One-shot peripheral-config sequences count too

Once the cache-clear and memset offenders were handled, stage 2 still jammed
intermittently *after* reporting a successful mount (blink code 2). Because
`flash_led` uses the bounded server it blinks even after the console is already
dead, so "two blinks then stripes" does not mean the 6507 survived the mount —
it means something in the mount dropped a fetch and the plain server afterward
had nothing left to serve.

The culprits were the register-configuration sequences that run only once (or
once per retry) and so are easy to dismiss: `SDIO_DeInit`, the several
`SDIO_Init` reconfigurations, `SD_LowLevel_DeInit`/`SD_LowLevel_Init`, and the
`NVIC_Init` pair in `disk_initialize`. Each is 10–40 back-to-back writes to
peripheral registers on APB2 with wait states, which can add up to more than one
~838 ns Atari cycle even though it contains no loop and no wait. They now yield
between register groups. The rule is about *elapsed time between yields*, not
about whether the code looks like it blocks.

### Pump SD work in the free cycles, not before the serve — `moviecart_bus_pump`

Even with every stretch yielding, the DMA read waits still glitched
occasionally: a garbled title, rarely a jam. Those loops ran
`sd_pump_completion(); moviecart_bus_yield();` — housekeeping *before* the
serve. `moviecart_bus_yield` returns the instant the address advances to the
next cycle, so the ~250 ns of `sd_pump_completion` that follows lands *inside*
that fresh cycle, eating into the 6507's data-valid deadline before we drive the
byte. A boot-sector read is ~20 of these iterations, each with a small,
phase-dependent chance of missing the fetch — hence intermittent and read-length
dependent.

`moviecart_bus_pump(work)` fixes the ordering by construction. It serves a
cart-read cycle (A12 high) exactly like the bounded serve with nothing inserted
before the dispatch, and runs `work` **only on non-cart cycles (A12 low)**.
During an A12-low cycle the cart drives no data at all, so the whole ~838 ns is
free and the housekeeping cannot cause a missed cart fetch no matter how the
phase falls. The 6507 issues A12-low cycles constantly (zero-page, stack, TIA,
RIOT), so completion is still pumped often enough to finish the transfer. This
is the interleave-proof result (`STALL_TEST=3`) taken to its safe limit: do
extra work only in cycles that are structurally incapable of missing a fetch.
The SDIO read/write completion waits now use it; plain CPU-bound loops that just
need to not starve the bus continue to use `moviecart_bus_yield`.

### The DMA buffer and the kernel buffers must not share a RAM slave

After every *instruction*-level stall was serving the bus, stage 2 still failed
on most cold starts: a title that came up then garbled, occasionally a clean
jam, rarely perfect. That pattern is not a missed fetch (which is all-or-nothing
per cycle) — it is *jitter* on many cycles at once, which points at the hardware
bus rather than the code.

The cause was AHB contention. The one boot-sector read DMAs 512 bytes into
`diskBuffer`, and `diskBuffer` sat in the same 112 K SRAM1 as `mr_buffer1/2` and
the stack — everything `bus_dispatch` touches to put a byte on the data pins.
The DMA runs at VeryHigh priority, so each burst that collided with a dispatch
stalled the CPU a few cycles and pushed the data-bus write past the 6507's
sample point. It happens on every mount (the boot sector is always read) and its
severity depends on how the bursts line up with the 6507 — hence "usually bad,
sometimes perfect."

The STM32F407 exposes SRAM1 (`0x20000000`, 112 K) and SRAM2 (`0x2001C000`, 16 K)
as *separate slaves* on the bus matrix. `diskBuffer` now lives in SRAM2 (linker
`.sram2` section) and the stack was pulled down to the top of SRAM1
(`_estack = 0x2001C000`). A sector DMA into SRAM2 and the per-cycle dispatch out
of SRAM1 then proceed in parallel with no arbitration between them. This is why
UnoCart never hit it: it does not stream SD DMA while serving a live bus.

Note for playback: the same care will be needed for the field buffers. When a
field is DMAed in while the other is being served, put the two in different
slaves (or the incoming one in SRAM2) so the read cannot stall the serve.

### The real one: work placed immediately *after* a yield, times 100k cycles

`MOUNT_PHASE=1` runs the whole card bring-up with **no data-path DMA at all**,
and it still failed on most cold starts. That ruled out DMA contention and the
SRAM split as the dominant cause, and pointed at the serving pattern itself.

Every long serve loop had this shape:

```c
while (!(status & flags)) {
	moviecart_bus_yield();   /* returns the instant the address advances */
	status = SDIO->STA;      /* ~60-120 ns on APB2 */
}
```

`moviecart_bus_yield` returns *exactly* when the bus steps to the next cycle, so
whatever follows it executes inside that brand-new cycle, before the dispatch has
put the byte on the pins. The STA read is only ~60-120 ns and the fine sweep says
100 ns is survivable — so any single iteration looks fine, which is why this hid
for so long. The problem is volume: card init sits in these loops for tens to
hundreds of milliseconds, i.e. **>100,000 served cycles**, each carrying a small
chance that the late data misses the 6507's sample point. At even a 0.01% fatal
rate a jam is near-certain. Hence "usually vertical bars, occasionally perfect" —
the outcome is a dice roll over a huge number of trials, not one bad code path.

The same shape was in `led_wait_serving`, which polls `DWT->CYCCNT` after each
served cycle and runs for ~600 ms of blinks *before* the mount begins. The
title-only build that was pixel-perfect went straight to `emulate_cartridge` and
never used it, which is why the LED codes themselves were part of the problem.

The fix is to keep the loop body down to a single SRAM load and move the actual
polling into the free window via `moviecart_bus_pump`:

- `sd_wait_sta(mask, iters)` samples `SDIO->STA` in A12-low cycles into
  `sd_sta_cached`; every command-response wait (`CmdError`, `CmdResp1/2/3/6/7`,
  `IsCardProgramming`) now tests only the cached copy.
- `moviecart_delay_ms` checks the DWT deadline in the free window, and
  `led_wait_serving` simply calls it instead of rolling its own loop.
- `FindSCR`'s FIFO drain reads STA and the FIFO in the free window too.
- `wait_title_sync` had it too, and was missed on the first pass because it is not
  SD code at all: its `(DWT->CYCCNT - start) < limit` timeout test sat directly
  after the serve, for up to a whole frame of cycles. Moving that test into the
  pump's free window is *not* the fix here (see the dead-hint spiral below); the
  timeout is instead sampled once every 4096 served cycles, leaving a register
  countdown in the hot path. Milliseconds of resolution on a 3 s timeout costs
  nothing, and it removes 4095/4096 of the peripheral traffic.
  `waitEndFrame`, the playback drain, was always safe — its loop body is one SRAM
  load, which is exactly the shape the pixel-perfect title build ran.

The general rule, now in its final form: **nothing may execute between a served
cycle and the next serve except a single SRAM load.** Anything else — a
peripheral read, a cycle-counter read — belongs inside the pump's free window.
This applies to every serve loop in the tree, not just the ones near SD code.

### Then: whole missed cycles inside the GPIO helper loops

Removing the after-yield gaps changed the failure *shape*, which is the useful
part. Before, a bad run was garbled-but-running; after, a run is either pixel
perfect or jammed, never garbled. Garbling is many slightly-late cycles; a jam is
one completely missed fetch. So the remaining bug was no longer jitter but a
small number of gaps longer than a whole ~838 ns cycle — at ~75% failure, about
one or two per run.

They were in the pin setup. `TM_GPIO_InitAlternate`, `TM_GPIO_DeInit` and
`TM_GPIO_SetPullResistor` each loop over all 16 pin positions doing register
read-modify-writes, so a single call runs well past one Atari cycle. Putting a
yield *between* those calls, as the first pass did, does nothing for the misses
that happen *inside* them.

`SD_LowLevel_Init`/`SD_LowLevel_DeInit` now configure the SDIO pins one pin at a
time via `sd_pin_af`/`sd_pin_release`, which write the five registers for a
single pin (~250 ns) and then serve a cycle. The shared TM_GPIO helpers are left
untouched — they are also used to bring up the Atari pins before serving starts,
where calling the bus server would be wrong.

### Work in the free window must still be guaranteed to run

Pumping `work()` only on A12-low cycles is the safest placement, but on its own it
is a liveness bug, and it wedged the firmware with the LED stuck solid. A caller
that waits on a flag `work()` sets — `moviecart_delay_ms` waiting for its
deadline check — waits forever if no free cycle ever arrives. A jammed 6507 stops
changing the address at all, so if it stops with A12 high there are no free
cycles, ever. The same starvation would stall SD status polling, which is a
correctness bug and not merely a slow one.

Two independent guards, because a dead console must never be able to silence a
diagnostic:

- `moviecart_bus_pump` also steps `work()` inside a cart-read cycle every
  `PUMP_HIGH_EVERY` (16) passes, in the post-drive window the interleave proof
  measured as safe — the byte is already on the pins, only the tristate is at
  stake. Rate-limiting keeps the amortised cost near zero rather than paying it
  every cycle.
- `moviecart_delay_ms` carries its own served-cycle ceiling, so it terminates
  even if its deadline check never runs at all.

### DMA-free reads: drain the FIFO in free cycles (`POLL_READ=1`)

The eliminate-don't-tune answer to the per-sector DMA problems. Instead of a second
bus master (DMA) that contends with `bus_dispatch` and needs per-sector setup, the
card streams the sector into the 32-word SDIO RX FIFO and `SD_ReadBlock_Polled`
drains it **one word per free (A12-low) cycle** through `moviecart_bus_pump`. No
DMA, no `SD_LowLevel_DMA_RxConfig`, no `SD_WaitReadOperation`.

Feasibility was checked against the measured budget before writing it:

- RX FIFO is 32 words; one drained word is one STA read plus one FIFO read, which
  is the ~100 ns a free cycle can safely hide. Emptying the whole FIFO per visit
  would be ~3 us and is exactly the mistake to avoid — the pump re-checks the
  address between single-word steps, so an A12-high edge is always seen within one
  word's time.
- The F407 cannot pause the card (HW flow-control silicon errata), so the transfer
  clock is lowered (`SDIO_CK = 48 MHz/(div+2)`, default div `0x26` ≈ 1.2 MHz) until
  the FIFO cannot overrun across a run of cart-only cycles: ~213 us of cushion,
  versus ~0.85 ms to read a 512 B sector and ~5 ms for a 6-block PAL field — well
  inside a frame. Playback needs ~240 KB/s and this clock still delivers ~600 KB/s.
- Overrun, if the clock is still too high for some card, is a clean `RXOVERR`
  return, not a corrupted picture. `POLL_CLKDIV` can be raised on hardware to trade
  cushion for margin without touching code.

Gated behind `MOVIECART_SD_POLL_READ` so the DMA build is bit-for-bit unchanged
(verified by hash), keeping the earlier baseline intact for comparison.

### Result: it made things much worse, and the reason inverts the clock assumption

On hardware `POLL_READ=1` at div `0x26` scored **1/10** on `SD_STAGE=2` (versus
"pretty reliable" for DMA) and **0/10** on full playback. Two changes had been
bundled — DMA→polling *and* 4.8 MHz→1.2 MHz — so the first result was uninterpretable
on its own. The failure signature broke the tie: **vertical colour bars**, i.e. a
jammed 6507 from bus starvation, *not* the clean `RXOVERR` a too-fast clock would
produce.

So the FIFO cushion the low clock was bought to protect was never the binding
constraint, and buying it cost 4× in the thing that actually matters:

> **Exposure time is the failure mode. A slower SD clock makes it worse, not better.**

Sector time scales inversely with the clock, and *every* microsecond of it is spent
serving Atari cycles from inside SD code where the per-cycle risk is elevated.
Dropping to 1.2 MHz stretched a sector from ~213 us to ~853 us and thus quadrupled
the number of at-risk cycles per sector.

This applies to the **DMA** path too, and there it is nearly free: DMA drains the
FIFO in hardware while we serve, so the clock is not coupled to anything the CPU
does — it only sets how long the window lasts. Hence `DMA_CLKDIV` (see `defines.h`):
`0x00` = 24 MHz ≈ 43 us/sector, a 5× smaller window than the 4.8 MHz we had been
using. 4.8 MHz was *our* conservative guess; the driver's own default is 16 MHz and
the part is rated for 25 MHz default-speed, so the original setting was likely making
every sector five times more dangerous than it needed to be.

### Clock results, and why they point away from the clock

| transfer clock | `SD_STAGE=2` |
| --- | --- |
| 12 MHz (`DMA_CLKDIV=0x02`) | 2/10 |
| 24 MHz (`DMA_CLKDIV=0x00`) | 4/10 |

Faster is better, as the exposure argument predicts, but 4/10 versus 2/10 at n=10 is
well inside noise, and both look worse than the ~8/10 remembered for 4.8 MHz. That
comparison was never valid though: the remembered figure came from a binary predating
the CMD16 change, and `mc_stage2.bin` was later reported as "crashing pretty
regularly". **Always re-measure the control on the current code generation** rather
than comparing against a remembered number — hence `mc_s2_c48.bin`.

The weakness of the effect is itself informative. At 24 MHz a sector's data phase is
only ~43 us (~51 Atari cycles); if 6/10 boots still fail, the damage is mostly *not*
in the data phase, so no clock setting can fix it. That moved the search to the
per-sector work that does not scale with the transfer clock — and to the pump itself.

### The real coin flip: a work step's *position* in the cycle (`PUMP_ALIGN=1`)

`moviecart_bus_pump` ran `work()` whenever the current address was A12-low, with no
notion of *where inside that cycle* it was. Since the caller re-enters the pump the
instant work returns, one 838 ns free cycle absorbed several steps back-to-back, and
the last one straddled the boundary into the next cycle. When that next cycle was a
cart fetch, `bus_dispatch` started late by a whole step (~150-250 ns: an indirect
call plus an APB2 `SDIO->STA` read).

The guidance in the code — "keep steps to a fraction of 838 ns" — bounds a step's
**duration** but says nothing about its **position**, and nothing controlled the
latter. The same step is harmless at the start of a free cycle and fatal at the end
of one. So every step was a coin flip, which explains the failure shape better than
any of the bandwidth or contention theories did: never reproducible, no single
guilty instruction, severity scaling with the number of steps executed and therefore
with sectors read (a handful for `SD_STAGE=2`, hundreds a second in playback).

`PUMP_ALIGN=1` spends at most one step per address, so a step can only begin just
after an address change was detected — at a cycle boundary, with the whole cycle
ahead of it. Throughput is essentially unchanged because the 6507 changes address
nearly every cycle.

### Result: both disproven, so stop theorising and measure

On hardware `PUMP_ALIGN=1` changed reliability not at all, and 4.8 MHz was reported
*more* reliable than 24 MHz — the opposite of the exposure prediction. Four mechanisms
have now been proposed and none moved the needle: DMA/CPU contention, transfer-clock
exposure, FIFO overrun, and pump-step position. The knobs (`DMA_CLKDIV`, `PUMP_ALIGN`,
`POLL_READ`) stay in the tree because they are cheap and isolated, but guessing a fifth
mechanism would be the same mistake a fifth time.

The one invariant across every build is that failures scale with the number of sectors
read. So there is a fixed per-sector risk none of the knobs touched, and the next move
is to *read its size and location* rather than theorise it. `GAP_PROBE=1` already
records the worst serve-to-serve gap and the region that caused it; it was only ever
reporting after card init, before any FAT sector read. Now:

- The per-sector read path is marked (regions 6 = command/arm, 7 = DMA drain,
  8 = card-ready poll), so the worst gap can be *attributed* to a specific step.
- With `SD_STAGE=2 GAP_PROBE=1` the report fires after `pf_mount`'s FAT walk, i.e.
  after real sector reads, instead of at `MOUNT_PHASE=1` before them.
- The report and all failure codes are wall-clock timed (see below), so they stay
  countable even when the picture has desynced.

The report is two counted groups: region (1-8, or 9 = never marked) then worst gap in
whole Atari cycles (1 = under one cycle = fine). A gap of 2+ in region 6/7/8 is the
first hard evidence of which read step overruns a cycle; a gap of 1 everywhere means
the overrun is not in the marked read path and the search moves elsewhere (e.g. the
FAT-layer memcpy/seek between reads).

### `GAP_PROBE` cannot measure this system at all

The probe build goes straight to a black screen and never reports. That is the probe,
not the bug. `probe_enter`/`probe_exit` are on **every served cycle** — two DWT
debug-block reads plus the loads, compare and conditional stores around them. The
fine sweep put the entire per-cycle budget at ~100 ns, which at 168 MHz is only ~17
CPU cycles; two DWT accesses alone exceed it. So the probe jams the 6507 by
construction, and the marked read-path regions above can never be read out.

This is the README's own rule — *a diagnostic that damages the system it measures is
worse than none* — broken by the diagnostic itself, twice now (it previously also
reported a phantom gap from its uninitialised first sample). **Do not reach for
`GAP_PROBE` again to chase per-cycle costs.** Any instrument that touches the serve
loop is over budget before it measures anything.

What is left is measurement by ablation, which costs zero per cycle: add or remove
whole units of work and compare success rates. `SD_STAGE` and `MOUNT_PHASE` already
localised the problem this way. `NO_CARD_READY=1` removes one command round trip per
sector; `DOUBLE_READ=1` is the matching positive control that doubles per-sector work
and must make things clearly worse if the per-sector model is right at all. A control
build must be re-measured alongside them every time, since the reference binary keeps
changing (`mc_s2_ref.bin` is hash-identical to `mc_s2_c48.bin` for exactly this
reason).

### Ablation result: source logic was not the variable; flash layout was

Hardware gave `NO_CARD_READY=1` 2/10 success and `DOUBLE_READ=1` 5/10. Doubling
the reads did not make failure more likely, so the fixed-per-read-risk premise is
false (the precise rates at n=10 are noise). The ELF layouts exposed the actual
variable:

- The reference and `DOUBLE_READ` images put `bus_dispatch` at `...1f48`,
  `bus_serve_cycle` at `...3d00`, and `moviecart_bus_pump` at `...3d60`.
- Removing CMD13 let `--gc-sections` discard its now-unused SD status helper.
  Every hot function shifted 104 bytes: dispatch to `...1ee0`, serve to
  `...3c98`, and pump to `...3cf8`.
- This changed dispatch from offset 8 in an F407 ART 128-bit line to offset 0,
  and serve from offset 0 to offset 8. Thus the ablation changed flash-fetch
  timing at the same time as SD behavior.

This is why logically unrelated SD edits have repeatedly changed reliability:
optional driver code was linked before the Atari critical path, so garbage
collection moved the latter. The fix is structural. `bus_dispatch`,
`emulate_cartridge`, `bus_serve_cycle`, and `moviecart_bus_pump` now live in a
dedicated `.hottext` section before optional code. The linker preserves the
better control image's exact addresses: dispatch `0x08001f48`,
`emulate_cartridge` `0x08003cac`, serve `0x08003d00`, and pump `0x08003d60`.

Verified across reference, `NO_CARD_READY`, and `DOUBLE_READ` builds:

- all four hot symbols have identical addresses;
- the complete `.hottext` section is byte-identical;
- `NO_SD` and full-playback builds use the same symbol addresses too.

This removes firmware layout as a confound from every subsequent SD experiment.

### Two owners of one LED: why the codes were unreadable

Wall-clock timing fixed the *rate* of the failure codes but they still came out
"overlapped or faster" and uncountable. The cause was not timing at all: there is
one status LED and two writers. `diagFrameTick()` drives TESTA0 once per frame from
inside `bus_dispatch`, and blinking any code necessarily keeps serving the bus — so
the kernel's heartbeat kept stamping on the code mid-flash. What reached the LED was
two superimposed patterns, which is why no amount of slowing the blinks helped.

`mc_led_host` now marks the LED as claimed; the heartbeat returns early while it is
set, and a `fatalBlink` leaves it set for good so repeats are never interleaved.

Worth generalising: this is the third instrument in this project to be broken by the
system it observes (the probe's phantom first sample, the probe's per-cycle cost,
and now heartbeat contention). Before trusting *any* future diagnostic here, ask what
else touches the channel it reports on.

### Making the layout invariant is not the same as pinning one address

The first attempt pinned `.hottext` to the old control image's absolute address.
That fixes only the section *start*: adding a single flag test to `bus_dispatch`
grew it and pushed the serve loops 24 bytes, changing their ART-line offsets and
re-introducing the confound in the very next measurement.

Both halves are required — `.hottext` immediately after the fixed-size vector
table so nothing optional precedes it, *and* `aligned(16)` on each hot function so
one growing does not move the others. Verified across `NO_SD`, `SD_STAGE=2`, full
playback, `NO_CARD_READY`, `DOUBLE_READ` and `PUMP_ALIGN` builds: `bus_dispatch`,
`emulate_cartridge`, `bus_serve_cycle` and `moviecart_bus_pump` sit at identical,
line-aligned addresses in all of them.

Note also what *cannot* be concluded from the earlier layout discovery: because the
`NO_CARD_READY` image changed SD behaviour and layout together, it never showed
which ART offset is preferable. Line alignment here is a principled default (a loop
starting mid-line spans an extra flash line for no reason), not a restoration of a
lucky offset. Resist the urge to tune offsets by trial — that is a search space with
no end and no theory.

### Code 7 meant three different things; split it

With the LED finally readable, playback failed as "7 blinks" — but code 7 was fired
for every `prepareNextFrame` reject, and a *read* error did not reach it at all
(`disk_read_block_raw` retried forever, wedging on the last good frame: one of the
"black screen" cases). One code was hiding three distinct layers, so they are now
separated:

- **7** field header wrong (`MVC\0` missing): the bytes are wrong but full-length.
- **9** field geometry invalid (`visibleLines`/`numBlocks` out of range): header fine,
  contents not.
- **10** sector read failed after `DISK_READ_RETRIES`: the card/read path itself, not
  the data. The unbounded retry is now bounded so this shows a code instead of hanging.
- **long 2 s strobe** (not a count) the field's bytes were found in the scratch buffer:
  the read succeeded but went to the previous DMA destination.

The next playback failure now says which layer to look at instead of collapsing all
three into one number. Header/geometry (7/9) point at *what the read returned*; 10
points at *the read*.

### Read cache keyed on the sector alone could serve stale bytes

`disk_read_block2` skipped the read when the sector matched its last one — but its
destination is the ping-pong field buffer, so the *same* sector can legitimately be
wanted in a *different* buffer on a later frame. Keyed on the sector alone, that read
was skipped and the new buffer kept whatever it held. The key now includes `dst`, so
a repeat only counts when the exact (sector, buffer) pair is already satisfied. Field
stride (`FIELD_NUM_BLOCKS` = 8) exceeds max used (`FIELD_MAX_BLOCKS` = 6), so forward
playback does not alias — but pause/replay and buffer ping-pong can, and this closes
that hole regardless.

### Code 7 persists: separating "wrong sector" from "data never landed"

Playback still reports 7 (header wrong) with a corrupted picture, and never 10, so
the read *succeeds* — DMA completes and the CRC passes — yet the bytes are wrong.
That narrows it to two shapes, which need opposite fixes:

1. **Wrong sector.** `pf_seek_block` walks the FAT chain with `get_fat`, and nothing
   validates the cluster it returns. One bad FAT sector yields a bogus cluster, and
   `clust2sect` then points at a real but wrong sector — a successful read of the
   wrong data.
2. **Right sector, data did not land.** Note the asymmetry that makes this
   plausible: mount reads always target the same `diskBuffer`, so the per-sector DMA
   fast path never has to change `M0AR`; playback targets a new address every sector.
   A destination that failed to take effect leaves the buffer holding stale bytes,
   which is what a wrong header looks like.

`prepareNextFrame` now retries a failed field up to `FIELD_LOAD_ATTEMPTS` times
after calling `disk_read_invalidate()`. The invalidation is the whole point: keyed on
(sector, dst), the caches would otherwise satisfy the retry from the same bytes that
just failed, so the retry would prove nothing. Dropping sector1 too means the retry
re-walks the FAT, covering both shapes at once.

The outcome distinguishes them without any new instrumentation:

- **playback continues, heartbeat shows 5** — transient; a re-read fixes it, so the
  bytes were at fault, not the arithmetic. Attention goes to the read path
  (`M0AR` handling in the DMA fast path first).
- **still code 7 after all attempts** — the computed sector is wrong every time: a
  deterministic seek/FAT-walk fault, and the read path is exonerated.

Retry also has standalone value: a transient bad field currently kills playback
outright, which is a harsh response to one recoverable sector.

### The buffer was read before the DMA had written it

Retrying a failed field (with the caches invalidated) never helped — still code 7,
never 10. That looked like "deterministic wrong sector", but that reading assumed a
data-landing fault would be *transient*. A **systematic** completion bug fails every
retry identically, so the retry never distinguished the two. The completion path had
two such bugs.

**1. The wait exits on the wrong completion.** The stock loop was

```c
while (!DMAEndOfTransfer && !TransferEnd && !TransferError && timeout) ...
```

which returns as soon as *either* completion appears. `TransferEnd` comes from
`DATAEND`, which only means the SDIO peripheral clocked in the last byte — up to a
full SDIO FIFO (32 words) plus the DMA's own FIFO may still be unwritten. `DATAEND`
always precedes the DMA's transfer-complete, so the wait effectively *always*
returned early. The follow-up wait only drains `RXACT`, which is again the
peripheral, not memory. Nothing ever waited for the DMA to write the buffer.

It is worse than data merely arriving late: the next `SD_ReadBlock` immediately
disables the stream to retarget `M0AR`, discarding whatever was still in flight, so
the tail of each sector was silently *dropped*.

**2. A sticky flag could skip the wait entirely.** `SD_ReadBlock` reset
`TransferError`, `TransferEnd` and `StopCondition`, but not `DMAEndOfTransfer`, which
is only cleared inside `SD_WaitReadOperation`. A leftover value makes the next wait
exit immediately, before *any* of that sector has landed, and the caller then
validates a wholly stale buffer — which presents as a corrupt field header, not as an
error. That is a direct path to code 7.

The wait is now two explicit phases — peripheral done (`DATAEND`), then memory
coherent (DMA transfer-complete, bounded by `SD_DMA_DONE_MAX` rather than the
0xFFFFFFFF `SD_DATATIMEOUT`) — and `DMAEndOfTransfer` is reset at the start of every
read.

Generalising, since this is the second instance: a "wait for completion" that ORs
together two different completions waits for neither. And the reason this was slow to
find is that both bugs corrupt data while every status check reports success, so the
read path looked innocent from the outside.

### Abandoning interleaving: the WaitCart handoff

Every approach up to here tried to fit SD work *between* Atari cycles, and the
measurements never supported it. The fine sweep put the entire per-cycle budget near
100 ns; a field is hundreds of sectors; and each of DMA clock, pump alignment, poll
draining, SRAM banking and per-sector DMA setup changed the failure rate without
fixing it. That pattern — many variables all mattering a little, none decisive — is
what a fundamentally insufficient budget looks like.

UnoCart solves the same problem from the opposite side, and it runs reliably on this
board. `PrepareWaitCartRoutine` copies a routine into Atari RAM and the 6502 JSRs it,
so the first cart fetch after that JSR is issued *from RAM*. The ARM only leaves its
serve loop when it sees that command (`LDA $1E00,X`). With nothing to serve, SD work
is ordinary blocking code. The console keeps a picture because the RAM routine *is*
a display kernel.

MovieCart owns the kernel ROM (`g0xNN` in `core.c`), so it uses that same sequence
instead of hijacking a live instruction stream:

1. **Copy from `$FF2D`, between `RESP1` and the WSYNC at `$FF31`.** `JSR $FFEC`
   replaces `lda #$20 / sta HMP1`; PrepareWait copies from `$1E00` into `$84`,
   does that displaced pair itself, and `RTS` from `$FFFB` (the 6507 has no NMI
   pin, so `$FFFA`-`$FFFB` are free). Sixteen bytes exactly.

   The slot matters, the length does not. `RESP0`/`RESP1` (`$FF1E`/`$FF21`) are
   the only writes whose column depends on where the CPU sits within a scanline,
   and they are anchored by ClearMem: `sta $00,x` walks x down from `$FF`, so
   `x = $02` writes **WSYNC**, and every cycle from there to `RESP` is fixed.
   (That anchor is why stock sprite positions are deterministic even though the
   6507's reset phase is not.) The copy therefore has to go on the *far* side of
   `RESP`, where the WSYNC at `$FF31` absorbs it and HMOVE, `wait_cnt` and the
   entry into `right_line` are all timed from that WSYNC.

   The first attempt put the `JSR` at `$FF0C`, inside the anchored window: 267
   cycles, 39 mod 76, **117 pixels** of sprite displacement — the corrupt title.
   Padding to "0 mod 76" cannot fix that placement (with a 14-cycle copy loop the
   total is always odd, and 76 is even), and adding a second WSYNC to the copy
   changed nothing because `$FF31` already provided one. Do not move it past
   `$FF33`: after HMOVE it skips `wait_cnt` and jams the 6507 (black, 6 flashes).

   Whatever displaced pair PrepareWait absorbs must be a plain store. An earlier
   version ended with `inc VDELP1`, which reads back a *write-only* TIA register,
   so VDELP1 became bit 0 of stale bus data instead of 1 and player-1 graphics
   were corrupt from boot.
2. **Enter RAM only when ARMED.** The blanking kernel is exactly 76 cycles with no
   WSYNC. Jumping to `$84` every frame while the title is up desyncs it (black
   picture) and then the park wait hangs on a solid LED because `mr_endFrame` has
   stopped. Title uses stock `$B7`/`$3E`. `nextLineJump` holds a whole 16-bit
   target (`$FF3E`, `$FFB7`, `$0084`) — `$84` with high byte `$FF` is `jmp $FF84`,
   a mid-kernel `sta AUDV0`, and gives vertical colour bars.
3. **`$FFF4` is PARK while ARMED.** Not `$5A` → stay in the WSYNC loop. `$5A` →
   clear VBLANK and VSYNC, `JMP $FF31`. Inferring park from `$FFEA`/`$FFEE` was a
   false signal: those fetches are still cart cycles. The park wait times out on
   DWT, not kernel frames.
4. **Return.** `$FF31` is the boot path (`STA WSYNC`, `HMOVE` @03, `wait_cnt`,
   `right_line`) so resume is on-cycle without counting into `end_lines`.
   Cycle-counted `JMP $FFB7` was ~70 cycles early at cycle 0, and still wrong at
   the guessed cycle 71. Both VBLANK and VSYNC are cleared on the way out:
   nothing between there and `right_line` rewrites either, so parking out of the
   vsync section would otherwise resume with VSYNC still asserted. The ARM reloads
   the visible field on READY (`graphBuf` at the start of a buffer) so the picture
   does not walk through SRAM.

   **The data bus is tri-stated for the whole of `work()`** — `SET_DATA_MODE_IN`,
   which is exactly what UnoCart does at its `got_cmd:` label before it touches
   the card. Driving a constant PARK byte instead seems safer, because it
   guarantees the parked 6502 cannot read a floating `$FFF4` as READY, and it is
   what broke every resume: the park loop executes from RIOT RAM, so everything
   except the single `$FFF4` poll is A12-low, and holding PD8-PD15 enabled means
   the STM32 and the RIOT drive the same wires on every one of the 6502's own
   opcode and operand fetches. It struggled out of the loop eventually — mount
   and open both ran and reported success — but with a corrupted instruction
   stream, so the jump back into the kernel produced no frames. A floating poll
   cannot false-match READY anyway: the bus holds the last value driven, which is
   that instruction's own `$FF` operand fetch, not `$5A`. UnoCart's `$D8` sentinel
   relies on the same property.
5. **Open: the title is not yet identical on every power-up.** With the copy at
   `$FF2D` the boot path is correct, and it comes up correct — but not every time;
   some power-ups are corrupt and some are black. Nothing in the boot path is
   data-dependent, and the copy's length is now unobservable, which leaves the
   6507 reset race as the candidate: the console's RC reset releases the CPU tens
   of ms after power-on, and if its first vector fetches land before the ARM
   reaches its first `bus_serve_cycle` they read a floating bus. `breakLoops = 0`
   means there is no recovery loop to catch that. This was always present; it only
   became *visible* once the good outcome stopped being corrupt too. To confirm,
   compare power-up variability against the `NO_SD=1` baseline, which shares the
   same init ordering but has no copy.
6. **Prove the resume, not just the unpark.** The handoff waits for one
   `mr_endFrame` (DWT-bounded) before returning, and raises fault 2 if none
   arrives. Leaving RAM only proves the 6502 read READY from `$FFF4`; it says
   nothing about whether the jump back into the kernel produced frames. Without
   this the two are indistinguishable, and a failed resume showed up as a *fully
   successful* SD sequence — every milestone blinked, mount and open both good —
   followed by a black screen and an LED that just stopped, because the next
   `waitEndFrame` never returned and no code was left to report anything.

**Nothing in the handoff may add work to a dispatch case the title fetches.** The
per-cycle budget is ~100 ns, and the first version tested `volatile mc_wait_state`
inside `gstore` (four fetches per blanking line), `g0xb0`, `g0xb6`/`g0xeb` and
`g0xd9` — enough to make the ARM late on the fetches that draw the picture, which
reads on hardware as a corrupt, left-scrolling title. Every state-dependent choice
is now precomputed into `mc_jmp_after_visible` / `mc_jmp_after_blank` (whole jmp
targets) and `mc_gstore_page` (the copy source, zeroed once installed), so those
cases cost one load, as the baseline did.

The park is still only vblank-sized. Playback's visible 192 lines are a cycle-counted
kernel that pulls colour/graph data from ARM SRAM over the cart bus; that data does
not fit in 128 bytes of RIOT, so those lines cannot run from RAM. Mount and open do
not need that kernel, but they use the same doorbell so one protocol covers boot and
playback.

`moviecart_bus_yield` and `moviecart_bus_pump` return immediately during a handoff,
which is what lets the *unmodified* SDIO driver run at full speed — its busy-waits
still call them, and they now cost nothing. `bus_serve_cycle` and
`emulate_cartridge`, the display-critical paths, are untouched.

Cost, accepted deliberately: the borrowed vblank lines push no audio samples and skip
the controller capture, so there is a 60 Hz audio artifact during playback. Getting a
stable picture and correct data first is worth an audible seam; the borrowed window
can be shortened once the read time is known.

Two builds, because the mechanism should be proven before it is trusted:

- `make WAITCART_PROOF=1` — handoff every frame with a 1 ms stall and **no SD code**.
  Under the old model 1 ms is far past the budget and would garble the picture; parked
  in RAM it should be invisible. Expect 1 flash, 2 flashes, then a clean title and the
  normal heartbeat.
- `make SD_STAGE=0` — the real thing, field reads inside the handoff.

`make WAITCART=0` skips parking. `mc_wait_handoff()` still enables IRQs around the
work function so the stock driver can complete, but the 6502 is not in RAM — that is
the old cooperative-vs-blocking conflict and will glitch the picture. WaitCart is
required for this driver.

### Stock UnoCart SDIO, run inside the handoff

The patched MovieCart SDIO driver (yields in every busy-wait, DMA fast-path,
pumped completion with IRQs off) is gone. `fatfs_sd_sdio.c` / `.h` are byte-for-byte
the files from UnoCart-2600's Atari2600Cart firmware, which already work on this
board. `sd_reader.c` calls `TM_FATFS_SD_SDIO_disk_initialize` and
`TM_FATFS_SD_SDIO_disk_read` directly — the same `SD_ReadMultiBlocks` /
`SD_WaitReadOperation` / `SD_GetStatus` sequence UnoCart uses.

That driver waits on SDIO and DMA *interrupt* flags, and `Delayms` waits on SysTick,
so a WaitCart handoff now `__enable_irq()` for the duration of the work callback
and masks again before serving resumes. `TM_DELAY_Init()` runs once at boot with
IRQs already masked, so SysTick is programmed but silent until the first handoff.
The transfer clock is UnoCart's `SDIO_TRANSFER_CLK_DIV = 0x04` (~8 MHz).

Mount, file-open, the title geometry probe, and every playback field load all run
as handoff work callbacks. LED blinks and `emulate_cartridge()` stay *outside* the
handoff: the 6502 is parked in RAM for the duration, so there is no kernel to serve.

### The failure moved to the first read that changes destination

With the completion bug fixed, the report from hardware became precise: `1, 2, 3, 7`.
That is mount and file-open passing, and the fault landing on the title's field probe
in `runTitle` — the *first* read of the run whose destination is not the scratch
`diskBuffer`. Everything before it (MBR, boot sector, FAT, directory) targets that one
buffer, so `M0AR` never has to change; everything after it, playback included, needs a
new destination on every single sector.

That asymmetry is the whole shape of the bug, across every result so far:

| build | reads needing a new `M0AR` | outcome |
| --- | --- | --- |
| baseline (`NO_SD=1`) | none | 10/10 |
| `SD_STAGE=2` (mount) | none | ~7/10 |
| full playback | every sector | fails at the first one |

The per-sector fast path in `SD_LowLevel_DMA_RxConfig` skips `DMA_DeInit`/`DMA_Init`
and rewrites `M0AR` alone, on the argument that nothing else about the stream changes
between sectors. It reads correctly — it disables the stream and waits for `EN` to
clear before touching `M0AR`, so the old transfer is flushed to the *old* address —
and it is still the only thing in the read path that distinguishes the passing reads
from the failing one.

Rather than keep arguing from the reference manual, the fast path is now behind
`MOVIECART_SD_DMA_FASTPATH`, **off by default** (`make FASTPATH=1` restores it). Full
re-init sets `M0AR`, `NDTR`, the FIFO and the flags unambiguously. It costs more per
sector, which is a timing problem; a read landing in the wrong buffer is a
correctness problem, and correctness comes first.

Two builds settle it together, since one shows *whether* and the other *why*:

- `FASTPATH=0` — if playback works, the fast path was the fault.
- `FASTPATH=1` plus a direct check: when a header fails validation, compare the
  *scratch* buffer against `MVC\0`. Finding the field's signature there proves the
  read succeeded and was simply misdirected to the previous destination. No retry
  could ever have shown this, because a misdirect is systematic and reports success.

That proof gets a **long 2 s strobe** rather than a count (`fatalStrobe`), because
distinguishing ten flashes from eleven by eye is unreliable and this particular
result decides which layer to fix.

### Failure codes must not be timed by the kernel they are reporting on

`fatalBlink` was frame-timed, which is only calibrated while the kernel is healthy —
exactly the case where nothing needs reporting. Once the section machine desyncs
`mr_endFrame` stops meaning 1/60 s and fires spuriously fast, compressing the code
into an uncountable flicker ("blinking too rapidly for me to tell what they are
reporting"). Failure codes are now wall-clock timed off DWT (`flash_led_slow`), with
the deadline sampled once per 256 serves so the hot loop stays identical to the
baseline's — the trick `wait_title_sync` already relies on.

Progress blinks (`flash_led`) were left frame-timed on the argument that they only
appear on a healthy kernel. That was wrong in the one case that matters: a milestone
is reported at the moment the thing it reports on may just have broken the kernel. A
frame-timed blink that begins with the LED lit and then loses `mr_endFrame` never
reaches its `TESTA0_HIGH`, so it reads as "two flashes and the LED remaining on" and
hides whether the *next* milestone was reached. Milestone blinks are wall-clock timed
for the same reason the failure codes are.

### Playback multiplies every per-sector cost by ~500 a second

`runFrameLoop` loads a whole field between two end-of-frames, so playback reads
several hundred sectors *per second* — a thousand or more in the two seconds of logo
before it died. Compare with `SD_STAGE=2`, which reads a handful and was "pretty
reliable, fails once in a while". If a handful of reads fails occasionally, the
per-read risk is on the order of 0.1–1%, and a thousand reads cannot survive that.
Playback therefore requires the residual per-sector risk to be *eliminated*, not
reduced; anything that merely improves the odds will still fail within seconds.

That reframes the goal usefully: look for per-sector work that can be removed
outright rather than made faster.

Attempt 1 (reverted): caching the card block length to skip CMD16 on every read,
plus two yields inside `SD_ReadBlock` around `SDIO_DataConfig`. On hardware this
made `SD_STAGE=2` *less* reliable and stopped playback from even reaching the logo
— i.e. it hurt the reads themselves, not just playback. Reverted whole to restore
the known-good state before trying again. Prime suspect is the yield inserted
between `SDIO_DataConfig` (DPSM armed) and CMD17: serving the bus while the data
state machine is armed but the read command has not yet gone out is not obviously
safe, whereas the CMD16 skip is pure deletion of work.

Attempt 2: the CMD16 skip *alone*, no added yields. `sd_cur_blocklen` records the
length last programmed into the card; a read sends CMD16 only when it differs.
It is reset to 0 in `SD_Init` and updated wherever CMD16 is issued with another
length (the multi-block reads and writes program 512; `FindSCR` programs 8 and the
SD-status path programs 64 during init), so a read can never skip CMD16 while the
card is actually set to a non-512 length. Because those short-length commands only
run during init and playback is reads-only, once the length settles at 512 every
subsequent read correctly skips the command.

### Per-sector DMA setup, not DMA bandwidth

Do **not** try to protect the Atari by de-prioritising the DMA. Measured:
`DMA_Priority_Low` with single-beat transfers took `SD_STAGE=2` from ~25% to never
working. Two reasons:

- `DMA_Priority` arbitrates between DMA *streams*. CPU-versus-DMA arbitration
  happens in the bus matrix and is not configurable through it, so the setting does
  nothing for the contention it appears to address.
- Single-beat transfers replace 32 four-beat bursts with 128 individual AHB
  transactions per sector: each shorter, but four times as many arbitration points.
  Total interference went up. Fewer, longer transactions are the better trade.

The per-sector cost was never the transfer, it was the **setup**. Every sector ran
`DMA_ClearFlag`, `DMA_Cmd(DISABLE)`, `DMA_DeInit`, `DMA_Init`, `DMA_ITConfig`,
`DMA_FlowControllerConfig`, `DMA_Cmd(ENABLE)` — dozens of register accesses plus
`DMA_DeInit`'s spin waiting for `EN` to clear — with yields only *between* the
calls and never inside them. Each call is microseconds of straight-line work with
the Atari unserved, and one missed cart fetch garbles the picture.

Nothing about the stream changes between sectors except the destination address, so
the full init now happens once and each subsequent transfer touches only what must
change (disable, wait for `EN` low *while serving*, clear flags, set `M0AR`,
enable), with a yield between individual writes. Channel, direction, burst, FIFO,
TC interrupt and peripheral flow control all survive a disable/enable untouched.

### Superseded: the DMA priority/burst experiment

The failure gradient was the clue that made this findable — measured on hardware,
same firmware, only the stopping point differing:

| build | SD work | works |
|---|---|---|
| `MOUNT_PHASE=1` | card init, no sector reads | ~80% |
| `MOUNT_PHASE=2` | + one 512-byte sector DMA | ~80% |
| `SD_STAGE=2` | + full mount (FAT walk, several reads) | ~25% |
| full playback | ~700 reads in the first two seconds | almost never |

One sector costs nothing measurable, a handful costs most of the reliability, and
hundreds are fatal. That is a fixed probability *per DMA transfer*, which points at
the transfer mechanism rather than at any code path.

The SDIO DMA was configured `DMA_Priority_VeryHigh` with `INC4` bursts on both
ports. `bus_dispatch` reads GPIOE and writes GPIOD, and DMA2 reaches the SDIO FIFO
across that same AHB1 — so moving `diskBuffer` to SRAM2 earlier only removed the
contention on the *memory* side, which is why that fix helped less than expected.
At VeryHigh priority the DMA wins arbitration against the CPU, and a 4-beat burst
holds the bus for four consecutive transfers: ample to push a data-bus write past
the 6507's sample point.

Now `DMA_Priority_Low` with single-beat transfers on both ports, for reads and
writes. The DMA never holds the bus longer than one beat and always loses to the
CPU. 512 bytes is nothing to move at 168 MHz, and if the DMA ever did fall behind
the card, the failure mode is a clean `RXOVERR` error rather than a silently
corrupted picture.

### Sector reads: two more instances of the same two mistakes

With blinks and card init cleaned up, `MOUNT_PHASE=1` became reliable most of the
time and the failure moved to the first sector DMA. Two offenders there, both
already-known patterns in code that had been missed:

- `SD_WaitReadOperation` (and `SD_WaitWriteOperation`) tested `SDIO->STA` **in the
  loop condition** while draining RXACT/TXACT — an APB2 read issued immediately
  after the pump returned, inside the cycle the 6507 had just started. This is
  exactly what `sd_wait_sta` was written to prevent; these two loops were simply
  overlooked when the response waits were converted. They now use
  `sd_wait_sta_clear`, testing only a cached copy.
- `sd_pump_completion` did the SDIO check *and* the DMA check in one step. A free
  cycle is ~838 ns and is entered partway through, while the SDIO branch alone can
  be four APB2 accesses on the DATAEND path. The two now alternate, one per free
  cycle, halving the worst-case step.

The recurring lesson: a work step must fit in a *fraction* of a free cycle, and it
is not enough to fix the pattern in the loops you happen to be looking at — every
wait in the driver has to be audited for a peripheral access in its condition.

### One serve loop, and work only in free cycles

The blink fix generalises, because card init sits in the same kind of long poll
(ACMD41 can run for hundreds of milliseconds). Measured constraints leave very
little design space:

- coarse sweep: **one missed Atari cycle garbles the picture**, even at end-of-frame
- fine sweep: the whole per-cycle budget is **~100 ns**

Anything paid on every cart cycle is therefore either free or fatal, depending only
on how many cycles it runs for — and every long operation runs for 10^5–10^6 of
them. So the serving code was cut back to a single shape:

- `moviecart_bus_yield()` is now plain `bus_serve_cycle()`. The bounded variant is
  gone: its `--guard` sat *inside* the tristate wait, delaying tristate detection
  on every cycle, and the shortened-guard optimisation latched itself on and cut
  live cycles short.
- `moviecart_bus_pump()`'s cart-read path is now byte-identical to
  `bus_serve_cycle()`. The `pump_tick` counter and the every-16th-cycle work step
  that used to live in the drive window are gone — that was a load/increment/store
  per cart cycle plus an indirect call on every 16th, well over the 100 ns budget.
- `work()` runs only in A12-low cycles, where the cart is not addressed and drives
  nothing. These are common on a running console (all TIA and RAM accesses), so
  polling still progresses briskly.

The consequence, accepted deliberately and consistent with the frame-timed blinks:
a jammed 6507 offers no free cycles and no unbounded-wait escape, so diagnostics go
silent. Not damaging a healthy console outranks reporting on a dead one.

### The LED blinks were killing the display (frame-timed blinks)

`SD_STAGE=1` runs **no SD code whatsoever** — `setupDisk` hands the bus straight
to `emulate_cartridge` before `pf_mount`. It is the `NO_SD=1` baseline plus
`wait_title_sync` and one `flash_led(1)`. On hardware it blinked once and then went
black, while the baseline was pixel-perfect. That isolates the whole fault to the
blink, with SD, DMA and FatFs entirely out of the picture.

The cause is volume, the same lesson as the SD waits and no less easy to miss:
`flash_led(1)` is 150 + 150 + 300 ms, about **700,000 cart cycles**, and it served
them through `moviecart_bus_pump`. The pump is not free — it maintains `pump_tick`
inside the drive window and the dead-bus guard state after the serve. Per cycle
that is nothing; over 700k cycles it accumulates into enough missed fetches to jam
the 6507. Every LED code was doing this, which is why builds that only *differed*
by having blinks kept losing the picture, and why the gap probe kept pointing at
the pre-SD region — it was right, and dismissing that reading as pure artifact
cost several rounds.

Blinks are now timed by counting the kernel's own frames through `waitEndFrame`
(`led_wait_frames`), which is the loop the pixel-perfect baseline runs: one SRAM
load between serves and nothing else. 9 frames is ~150 ms at 60 Hz. The blink now
costs the display exactly nothing.

The trade-off is deliberate: a blink needs a running kernel, so a fully dead
console shows no LED. Bounding the wait is what the dead-bus guard was for, and
that guard is repeatedly what breaks the display. A diagnostic that damages the
system it is measuring is worse than no diagnostic.

### Keep the baseline build one command away (`NO_SD=1`)

`make NO_SD=1` skips SD entirely: title screen plus the kernel's once-per-second
heartbeat, which is the configuration that was pixel-perfect. Two rounds of
hardware testing were wasted rewriting `wait_title_sync` on the theory that its
per-cycle cycle-counter read was the accumulating after-serve gap. Both rewrites
were worse — the pump version jammed every boot via the dead-hint latch below, and
the sampled-timeout version lost the picture before the first blink — and without a
baseline to compare against there was no way to tell "my change broke the base"
from "the SD path is still at fault". Reverted, with a comment on the function
saying not to try it again: sync lasts one frame, not the 100k cycles that made
the gap matter in the SD waits.

### The dead-bus hint must not be able to latch itself on

Shortening the tristate guard after an expiry (`bus_dead_hint`) is what makes LED
codes appear promptly on a jammed console, but the first version armed itself from
a *single* expiry and dropped to a 64-iteration (~2.3 us) guard. Two things make
that fatal:

- When the 6507 presents the same address on two consecutive cycles, the wait for
  a *different* address legitimately lasts ~1.7 us. A 2.3 us guard is not
  comfortably clear of that.
- Cutting such a wait short tristates mid-cycle, which garbles the picture, which
  jams the console, which keeps the guard expiring. Nothing can clear the hint,
  because the hint is now causing the damage that sustains it.

Routing `wait_title_sync` through the pump exposed this immediately: sync runs
before the 6507 is straight, so the guard expires as a matter of course at boot,
latched the hint, and the console never recovered — vertical bars plus a 6-blink
sync timeout, every single boot. Two fixes: entering dead mode now requires a
streak of consecutive expiries (`BUS_DEAD_STREAK`), and the dead guard is 256
iterations (~9 us), well above any real address hold time. Any wait that ends on
its own clears both. `wait_title_sync` also went back to the unbounded
`bus_serve_cycle`, which is the correct server for anything that runs while the
console is still finding its feet.

### Size the tristate guard to the bus, or failures become undiagnosable

`BUS_WAIT_GUARD` was 100000 iterations. On a live bus that number never matters —
the 6507 changes the address every ~838 ns, about 15 iterations — so it looked
harmless. It is not: the guard is only ever *reached* when the console has
jammed, and then every bounded serve pays the full ~5 ms. A wait built out of
60000 served cycles becomes minutes, so an LED report placed after card init can
never appear. The symptom is a firmware that looks hung but is only crawling, and
it cost a debugging round trip.

It is now 4000 (still hundreds of cycles of margin), plus a `bus_dead_hint`: once
a guard actually expires, later calls use a much shorter 64-iteration guard, and
any normal exit clears the hint again. 64 iterations is still several times one
Atari cycle, so a bus that comes back is served correctly. Diagnostics on a dead
console now run fast enough to report.

### Measuring the worst gap instead of guessing (`GAP_PROBE=1`)

Fixing the GPIO loops took stage-1 from ~25% to ~50% clean, still binary (perfect
or jammed), so one or two whole-cycle gaps were left. Rather than keep auditing
candidates by eye, `make MOUNT_PHASE=1 GAP_PROBE=1` measures it.

The probe timestamps every exit from a served cycle and every entry into the next
one, keeps the largest difference, and records which labelled region was running
when it happened (`MC_PROBE` markers). After card init it blinks two groups:

1. the region, 1..5 — 1 pre-SD (title sync and the LED blinks), 2 SDIO pin
   configuration, 3 SDIO register configuration, 4 card command sequences,
   5 CSD/CID unpacking. Five coarse regions, not eleven fine ones, so the count
   stays reliably countable and each answer maps to a different kind of fix.
2. the gap in whole Atari cycles, capped at 9 — **1 means under one cycle**
   (healthy); 2 or more is a real miss

Values are snapshotted before blinking, since blinking itself serves the bus. The
probe adds two cycle-counter reads per served cycle, so it perturbs slightly and
is a diagnostic build only — but the region it names is what matters.

Three mistakes this probe made first, all worth remembering:

- **It could not see one of the two servers.** Only `bus_serve_cycle_bounded` and
  `moviecart_bus_pump` were instrumented, but `wait_title_sync` and `waitEndFrame`
  serve through the unbounded `bus_serve_cycle`. The probe therefore recorded the
  whole of `wait_title_sync` — up to a full 16 ms frame — as a single "gap" in the
  pre-SD region, while the bus was in fact being served perfectly throughout.
  `bus_serve_cycle` is now instrumented too. Instrument every path that serves, or
  the unmeasured one becomes the apparent culprit.

- **It reported its own uninitialized state.** `probe_last_exit` began at zero, so
  the first sample measured time-since-reset — millions of cycles — and pinned the
  maximum forever, attributed to a phase that had never run. The first sample is
  now skipped (`probe_primed`). A probe that lies is worse than no probe.
- **It could not express "no data".** Phase 0 was reported as `phase ? phase : 1`,
  i.e. indistinguishable from a genuine region 1. It now blinks 9 for "no region
  was ever marked", so an unusable reading announces itself instead of sending the
  next fix to the wrong place.

### Stall-budget measurements (how we got here)

`make clean && make STALL_TEST=1` builds a sweep that does nothing but
that: it locks onto a frame, then repeats eight steps, stalling the bus loop for
0, 1, 2, 4, 8, 16, 32 and 64 µs once per frame — at end-of-frame, the same place
real field loading would go. `STALL_TEST=2` runs the same harness over the
sub-cycle region instead: 0, 100, 200, 300, 400, 600, 800 and 1000 ns.

Each step announces itself as *N* flashes on the status LED (step 1 = 0 µs, step
8 = 64 µs), then goes dark for a 5 second test window; the sweep repeats
forever. The blink itself keeps serving the bus, so the announcement never
disturbs the picture. The kernel's own end-of-frame diagnostic LED is compiled
out of this build so the sweep owns the LED, which also means the **display is
the verdict** — watch for the window where the picture garbles.

Step 1 is the control and must look exactly like the normal title build. The
first step that garbles bounds the budget, and the step before it is the size
every piece of restructured SD work must fit inside. The window is 5 s per step
because this failure is intermittent, so a step that survives one pass is worth
confirming across a couple of sweeps.

Run `make clean` when switching this flag on, off, or between modes; the object
files do not depend on it.

### Result: the budget is under one Atari cycle

Measured on hardware with the coarse sweep (`STALL_TEST=1`): **step 1
(0 µs) is clean and step 2 (1 µs) garbles.** One microsecond is about one
838 ns Atari cycle, so a single missed fetch — once per frame, during vertical
blank, the most forgiving moment there is — is enough to wreck the picture.

This is the mechanism already described above rather than a new one: the kernel's
scanline counter is stepped by the 6502's fetches, so one miss flips its parity
and the frame runs away. What the measurement adds is that there is no tolerance
for whole missed cycles to trade against, which rules out the whole family of
"do storage work in microsecond-sized chunks" designs, not just interrupt-driven
serving.

### Result: there is ~100 ns of usable slack inside a cycle

Measured on hardware with the fine sweep (`STALL_TEST=2`): **step 2 (100 ns) is
clean and step 3 (200 ns) garbles.** So the margin is real, and it sits inside a
single served Atari cycle — not between cycles. That also settles the older
"54 ns settle window" question: those experiments failed on timing, not on a
logic bug in how the address was resampled, but 100 ns of stall at end-of-frame
is still survivable.

### Interleave proof (`STALL_TEST=3`)

The end-of-frame budget does not automatically transfer to mid-cycle work. The
place cooperative SD steps actually run is after `SET_DATA_MODE_OUT`, while the
data pins are already driving the current byte and before the address-change
wait resumes. `make clean && make STALL_TEST=3` builds that proof: every cart
cycle burns 100 ns there, then returns to the tight wait.

Boot announces itself with three flashes, then the kernel's normal 1-flash-per-
second diagnostic owns the LED again. **Measured clean on hardware** — which is
what unlocked the yield-to-bus SD path (yield serves a full cycle rather than
burning 100 ns of slack; the proof showed the mid-cycle window itself is real).

## Earlier: intermittent garbled display / 3-blink code

The following historical notes remain useful; the SRAM-contention root cause is
resolved by keeping bus-serving code in flash.

### Symptom

The MovieCart logo renders correctly most of the time. Intermittently the
picture garbles and the status LED shows **3 flashes** (`DIAG_WRAP_ENDLINES`).
Sometimes a clean frame recovers on its own and the code drops back to 1. The
failure is timing-dependent, not deterministic: the same binary alternates
between a good display and the 3-blink garble across power cycles and within a
single run.

A second observed symptom is decisive: **during the garble, movie/logo pixels
appear outside the viewer window** (in the border / overscan region that should
be blanked). That is not "wrong pixels inside a correct frame" — it is the
kernel still drawing `right_line` (GRP/COLUP updates, VBLANK off) on scanlines
where the TIA should be blanked or in overscan.

### What the 3-blink code means

The kernel's scanline counter (`r_coreInfo.lines`) is stepped by the 6502's own
fetches — `lines -= 2` per line pair at `$FFaf`, `lines--` per line at `$FFd9`,
tested for exactly zero. If a single dispatch is **missed or doubled**, the
counter's parity flips, the `== 0` test is skipped, and the counter decrements
past zero and wraps. `LINES_EXHAUSTED()` catches the wrap and force-closes the
frame so the kernel resyncs (this is why it recovers), but the wrapped frame is
already corrupt on screen and the LED latches code 3 for that reporting window.

So code 3 is a **report that a dispatch went wrong**, not the root cause. The
root cause is why a dispatch is occasionally missed or doubled.

### What "pixels outside the viewer window" implies

The viewer window is gated by kernel state, not by the pixel buffers:

| State | `nextLineJump` | `vblankState` | Effect on the CRT |
| --- | --- | --- | --- |
| Visible section | `right_line` (`$3e`) | off | Sprites/colors update every line — the movie window |
| Overscan / blank | `end_lines` (`$b7`) | on | Drawing stopped; beam outside the window |

Pixels outside the window therefore mean at least one of:

1. **Visible section ran too long.** A missed `$FFaf` (`lines -= 2` skipped)
   leaves the counter high, so `right_line` keeps running past `visibleLines`
   into what should be overscan — with VBLANK still off. The beam is in the
   border; the kernel is still writing GRP/COLUP. That is exactly "movie pixels
   outside the viewer."
2. **Visible section restarted too early.** The `$FFd9` end-state machine
   (especially case 2) sets `nextLineJump = right_line` and clears VBLANK before
   overscan/blank has finished on the CRT. Drawing resumes while the beam is
   still outside the window. A wrap/corruption in the end-lines section (code 3)
   is a plausible path into this: `endState` and `lines` get out of phase, case 2
   fires on the wrong scanline.
3. **Wrong pixel data alone does not do this.** Corrupted `graphBuf`/`colorBuf`
   only changes what appears *inside* the window. Drawing outside requires the
   section machine (`nextLineJump` / `vblankState` / `lines`) to be wrong.

So this symptom promotes the fault from "sometimes a bad byte on the bus" to
**scanline-section desync** — the same class of bug `LINES_EXHAUSTED()` was
added to recover from, and the same class the 3-blink code reports.

### Root cause (current best understanding)

The breadboard bus has no timing margin, and MovieCart's dispatch is stateful in
a way a plain ROM cart is not. Two concrete mechanisms are known:

1. **Doubled dispatch from address-line coupling.** After driving data, the loop
   spins in `while (ADDR_IN == addr)` and tristates when the address "changes."
   Our own data pins switching couple into the nearby address lines; a glitch
   reads as a change, we tristate, then re-acquire the *same* address the 6502
   is still holding and dispatch it twice. A doubled `$FFd9` costs one extra
   `lines--`; a doubled `$FFaf` costs two.
2. **Missed `$FFaf` / end-state desync** (favoured by the outside-window
   symptom). Skipping the visible-section decrement, or advancing `$FFd9` case 2
   while still in overscan, leaves VBLANK off and `right_line` active outside
   the viewer. Code 3 (end-lines wrap) is consistent with the end-state half of
   this; a pure visible-section overrun would more often report code 2.
3. **The one-scanline zero window.** Case 2 of the `$FFd9` state machine leaves
   `lines == 0` for the remainder of that scanline until `$FFe3` reloads
   `visibleLines`. Any spurious `$FFd9` in that window decrements from zero and
   wraps. This is the single most exposed line in the frame and is consistent
   with the observed rate (≈ one vulnerable line per frame).

The dsPIC original does not see either problem: its change-notification
interrupt latency naturally debounces the address bus, and its data port is
driven through a hardware-gated transceiver so `SET_DATA` takes effect instantly
with no dependence on when the C code finishes.

### What has been ruled out (do not retry without new information)

Each of these was tried on hardware and made things **the same or worse**; the
reasoning that motivated them is recorded so it is not repeated:

- **Any settle window before the drivers turn on — at any size.** Tried twice:
  ~240 ns via a counted loop on both the acquisition and release paths, then
  ~54 ns (4 explicit NOPs plus a confirming re-read, count verified in the
  disassembly) on the acquisition path alone. **Both blacked out the display
  completely.** Delay ahead of the data drive is simply not available, so
  transients have to be rejected some other way or tolerated.
- **Repeat suppression** (`if (addr != served) dispatch;`). Intended to kill the
  doubled dispatch from mechanism 1 above. Result: permanent black screen and
  constant code 3 — it also suppresses genuine consecutive fetches the kernel
  needs, so it breaks more dispatches than it saves.
- **`lines = 1` in `$FFd9` case 2** to absorb the extra decrement from mechanism
  2. Functionally a no-op (`$FFe3` overwrites it the same scanline) yet still
  blacked out the display: **one extra store anywhere in `$FFd9`–`$FFe3` is over
  budget.** Treat the end-of-frame chain as closed to new work of any kind.

### Candidate fixes (not yet tried)

Roughly in order of expected value-to-risk. The safe direction is to *buy back
timing margin* or *increase the kernel's tolerance without adding stores to the
hot chain* — not to add filtering to the bus loop.

1. **Strip the diagnostics/counters out of the hot dispatch cases.** `frameLines++`
   at `$FFaf`/`$FFd9` and the per-frame `diagFrameTick()` add work to exactly the
   cases with no margin. Move all instrumentation behind a compile-time
   `DIAG_ENABLE` flag and confirm whether the garble rate drops with it off. This
   also tells us whether the diagnostics are themselves part of the problem.
2. **Confirm the blink actually correlates with the garble.** Restrict
   `DIAG_NOTE(DIAG_WRAP_ENDLINES)` to wraps that occur *outside* the known
   harmless zero-window, so code 3 only fires on real corruption. If the garble
   persists while code 3 goes quiet, the LED and the picture are two different
   events and we've been chasing the wrong signal.
3. **Debounce the tristate, not the dispatch.** Mechanism 1 is a false *change*
   detection, not a false address. Require the address to read changed on two
   consecutive samples before tristating (`while (ADDR_IN == addr)` → confirm the
   exit). This adds time only *after* data is already driven, where there is
   slack, instead of before it, where there is none.
4. **Reduce data→address coupling in hardware.** Series resistors (≈ 100–330 Ω)
   on PD8–PD15, tighter grounding, or shorter data leads attack mechanism 1 at
   the source and cost nothing in firmware timing.
5. ~~**Settle window, done right.**~~ Ruled out above — no size works.

### The sensitivity clue, and the current fix: execute from flash

Five separate changes blacked out the display, and one of them —
`lines = 1` in `$FFd9` case 2 — is *provably* a functional no-op that runs
**after** the data is already driven. Latency alone cannot explain that. What all
five share is that they shift where the bus code lands and how many bus
transactions it makes.

The cause is where that code was executing. `RAMFUNC` put `bus_dispatch()` and
`emulate_cartridge()` in SRAM, inherited from the abandoned EXTI design where
flash wait states on interrupt entry were the problem. For a polling loop it is
the wrong trade: on the STM32F407 the Cortex-M4 reaches SRAM over the **system
bus**, so instruction fetches there contend with every data access the loop makes
— the `romData` table, `r_coreInfo`, the frame buffers, and the GPIO registers
themselves. There is no instruction cache on that path, so every fetch is a
fresh bus transaction whose cost depends on where the code happens to land, which
is exactly the alignment sensitivity observed.

**UnoCart has no RAM-relocated code at all** and is stable on this board. So:

- `RAMFUNC` is now just `noinline`; the bus loop and dispatch execute from flash
  over the I-bus through the ART cache, leaving the system bus to data.
- `romData` stays in SRAM, so the table read now runs *in parallel* with
  instruction fetch instead of competing with it.
- `FLASH->ACR` gains `PRFTEN`; prefetch and the instruction cache are what hide
  the 5 wait states on the address-to-data path.

Verify placement after building — code in flash, table in SRAM:

```
$ arm-none-eabi-nm -n build/firmware.elf | grep -E "bus_dispatch|emulate_cartridge|romData"
08000844 T bus_dispatch
080025a8 T emulate_cartridge
20000000 d romData.0
```

If this is the real cause, the payoff is not just a stabler picture but a build
that stops rejecting every change — which is the precondition for finishing the
SD work.

### Diagnosing on hardware

- Watch the LED at the instant the picture garbles. Steady 1 = kernel healthy,
  problem is downstream (data/timing). 2/3 = counter parity broke (missed or
  doubled dispatch). 4 = frame geometry wrong. Frozen = kernel stopped closing
  frames.
- **Pixels outside the viewer window** = section machine desync (`right_line` /
  VBLANK wrong), not merely corrupt buffer contents. Prefer fixes that protect
  `$FFaf` / `$FFd9` / `nextLineJump` over ones that only harden the data bus.
- The heartbeat/diag path lives entirely inside the `endState == 3` block in
  `core.c`, runs once per frame, and is the only output channel while IRQs are
  disabled.

## Build

Needs `arm-none-eabi-gcc` (the UnoCart `tools/` toolchain works):

```bash
export PATH="/path/to/arm-none-eabi/bin:$PATH"
cd firmware/stm32
make clean
make bin hex    # build/firmware.bin and build/firmware.hex
```

Flash with ST-Link (`make flash` / `st-flash`) or DFU (BOOT0) on the DevEBox.

## Runtime notes

- System clock: HSI → PLL @ 168 MHz (same as UnoCart DevEBox fork); SDIOCLK 48 MHz.
- Bus servicing: UnoCart stable-address polling loop, main thread, IRQs disabled.
- Content: FAT/FAT32 card with MovieCart field files (same as stock). Select advances to the next file; joystick/console controls match the original.
- Status LED bit-bang uses **PA1** (optional).
- In-field FIRMWARE.FRM flash update from the dsPIC build is not ported.
