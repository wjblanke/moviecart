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

The current build is deliberately **title-only** to validate the display bus
independently of SD. After initialization it enters UnoCart's infinite bus loop
and never leaves. SD initialization and movie playback are temporarily disabled,
so the only LED activity is the ~1 Hz kernel heartbeat described below.

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

The bus loop owns the CPU with interrupts off, so the status LED is the only
channel out. `core.c` drives it from the kernel's own end-of-frame, one slot per
frame, blinking the worst condition seen since the last report:

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

The bus loop runs in main with interrupts disabled, exactly like UnoCart. There
is no TIM1 or EXTI bus server in this validation build.

## Known issue: intermittent garbled display / 3-blink code

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
