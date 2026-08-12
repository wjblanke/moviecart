/**
 * MovieCart on MCUDEV DevEBox STM32F407VGT6
 *
 * Cart buses match UnoCart-2600 DevEBox fork:
 *   A0–A12 = PE0–PE12
 *   D0–D7  = PD8–PD15
 * Onboard microSD via SDIO 4-bit (PC8–12, PD2).
 */
#ifndef MOVIECART_STM32_DEFINES_H
#define MOVIECART_STM32_DEFINES_H

#include <stdint.h>
#include <stdbool.h>

#include "movie_defines.h"

/* FatFs / SDIO (Tilen Majerle driver) */
#define FATFS_USE_SDIO			1
#define FATFS_SDIO_4BIT			1
#define FATFS_USE_DETECT_PIN		0
#define FATFS_USE_WRITEPROTECT_PIN	0

/*
 * DMA-free read path (make POLL_READ=1). The sector is drained from the SDIO RX
 * FIFO one word at a time, only in the free (A12-low) cycles of moviecart_bus_pump,
 * so there is no second bus master contending with bus_dispatch and no per-sector
 * DMA setup at all. The catch is that the card cannot be paused (HW flow control is
 * broken on the F407), so the transfer clock must be low enough that the 32-word
 * FIFO cannot overrun across the longest stretch of cart-only (A12-high) cycles.
 *
 *   SDIO_CK = 48 MHz / (div + 2)
 *   div 0x26 (38)  -> ~1.2 MHz: ~0.85 ms per 512 B sector (~5 ms for a 6-block
 *                     PAL field, well inside a frame), ~213 us full-FIFO cushion.
 *
 * Overrunning the FIFO produces a clean RXOVERR, not a corrupted picture, so this
 * clock is safe to tune down further on hardware if bursts turn out longer.
 */
#ifndef MOVIECART_SD_POLL_READ
#define MOVIECART_SD_POLL_READ		0
#endif

/*
 * Spend at most one pumped work step per Atari cycle (make PUMP_ALIGN=1), so a
 * step always begins at a cycle boundary instead of possibly straddling into the
 * next cart fetch. See moviecart_bus_pump in bus_service.c.
 */
#ifndef MOVIECART_PUMP_ALIGN
#define MOVIECART_PUMP_ALIGN		0
#endif

/*
 * Ablation knobs for locating the fixed per-sector cost.
 *
 * Timing instrumentation is not available to us: GAP_PROBE puts two DWT reads on
 * every served cycle, which alone exceeds the ~100 ns per-cycle budget the fine
 * sweep measured, so a probe build cannot keep the picture up long enough to
 * report. Removing or duplicating work costs nothing per cycle, so ablation is the
 * only non-destructive measurement left — the same bisection that SD_STAGE and
 * MOUNT_PHASE already did successfully.
 *
 *   NO_CARD_READY=1  drop the post-read CMD13 poll (one command round trip per
 *                    sector). SD_WaitReadOperation has already seen DATAEND, so a
 *                    single-block read is complete without it.
 *   DOUBLE_READ=1    positive control: read every sector twice. Hardware result
 *                    was 5/10 versus 2/10 for NO_CARD_READY, disproving the
 *                    fixed-per-read-cost model; retained only to reproduce that
 *                    ablation.
 */
#ifndef MOVIECART_SD_SKIP_CARD_READY
#define MOVIECART_SD_SKIP_CARD_READY	0
#endif
#ifndef MOVIECART_SD_DOUBLE_READ
#define MOVIECART_SD_DOUBLE_READ	0
#endif

#if MOVIECART_SD_POLL_READ
#ifndef MOVIECART_SD_POLL_CLKDIV
#define MOVIECART_SD_POLL_CLKDIV	0x26
#endif
#define SDIO_TRANSFER_CLK_DIV		((uint8_t)MOVIECART_SD_POLL_CLKDIV)
#else
/*
 * Transfer clock for the DMA path (make DMA_CLKDIV=...).
 *
 * Faster is *safer* here, which is the opposite of the intuition that guided the
 * original 0x08 (4.8 MHz). With DMA the hardware drains the FIFO while we serve the
 * Atari, so the clock does not have to match anything we do — it only decides how
 * long a sector takes, and therefore how many served cycles run inside the SD code
 * with its slightly elevated per-cycle risk:
 *
 *   div 0x08 (4.8 MHz) -> ~213 us per 512 B sector
 *   div 0x02 (12 MHz)  -> ~85 us
 *   div 0x00 (24 MHz)  -> ~43 us
 *
 * The failure signature is a jammed 6507 (vertical bars), i.e. bus starvation, not
 * an SD error — so shrinking the exposure window is the lever, and slowing the card
 * down was making it worse. 24 MHz is within the 25 MHz default-speed limit and the
 * driver's own default is 16 MHz.
 */
#ifndef MOVIECART_SD_DMA_CLKDIV
#define MOVIECART_SD_DMA_CLKDIV		0x08
#endif
#define SDIO_TRANSFER_CLK_DIV		((uint8_t)MOVIECART_SD_DMA_CLKDIV)
#endif

/* UnoCart DevEBox: A12 high selects cart; kernel indexed by A0-A8. */
#define CART_ADDR_MASK			0x1000u
#define CART_ADDR_SELECT		0x1000u

/*
 * Bus-serving code runs from FLASH, not SRAM — the same as UnoCart, which has no
 * RAM-relocated code at all and is stable on this board.
 *
 * SRAM placement was inherited from the abandoned EXTI design, where flash wait
 * states on interrupt entry were the problem. It is actively harmful for a
 * polling loop: on the STM32F407 the Cortex-M4 reaches SRAM over the system bus,
 * so instruction fetches there contend with every data access the loop makes —
 * the romData table, r_coreInfo, the frame buffers, and the GPIO registers
 * themselves. There is no instruction cache on that path either, so each fetch
 * is a fresh bus transaction whose cost depends on where the code happens to
 * land. From flash the fetches go over the I-bus through the ART cache, leaving
 * the system bus entirely to data. That decoupling is also the likely reason
 * this build has been so absurdly sensitive to adding a single instruction.
 *
 * romData stays in SRAM (see core.c): a table read over the system bus now runs
 * in parallel with instruction fetch rather than competing with it.
 */
#define RAMFUNC __attribute__((noinline))

/*
 * Keep the Atari critical path in a dedicated, 16-byte-aligned flash section.
 *
 * With ordinary -ffunction-sections placement, linker GC of an unrelated SD
 * helper shifted bus_dispatch and every serve loop by 104 bytes. On F407 flash,
 * that changes where branches and dispatch targets fall across the ART
 * accelerator's 128-bit lines, so a logically harmless SD edit can change bus
 * reliability.
 *
 * Two things are needed to make that stop. The linker places .hottext directly
 * after the fixed-size vector table, so nothing optional can precede it; and each
 * function is aligned to a 128-bit ART line, so its own position does not move
 * when a *preceding hot function* changes size either (adding one flag test to
 * bus_dispatch moved the serve loops by 24 bytes and would otherwise have been
 * the next confound).
 *
 * Line alignment is chosen on principle, not tuned: a loop starting mid-line
 * needlessly spans an extra flash line. Do not "restore" some previously lucky
 * offset — the build that appeared to prefer one changed SD behaviour at the same
 * time, so it never showed which offset was better.
 */
#define HOTFUNC __attribute__((noinline, section(".hottext"), aligned(16)))

/*
 * Bus stall budget measurement builds (make STALL_TEST=N). See README
 * "Measuring the bus stall budget":
 *   1 — coarse sweep, stall once per frame for 0–64 µs
 *   2 — fine sweep, stall once per frame for 0–1000 ns
 *   3 — interleave proof: 100 ns cooperative step every cart cycle
 *
 * Modes 1–2 compile out the kernel's end-of-frame diagnostic LED so the sweep
 * owns it. Mode 3 leaves the diagnostic active (1 flash/sec = frames closing).
 */
#ifndef MOVIECART_STALL_TEST
#define MOVIECART_STALL_TEST 0
#endif

/*
 * Stop SD bring-up after a milestone and fall back to the plain title loop
 * (make SD_STAGE=N): 1 = before mount, 2 = after mount, 3 = after file open.
 * 0 runs the full playback path.
 */
#ifndef MOVIECART_SD_STAGE
#define MOVIECART_SD_STAGE 0
#endif

/*
 * Finer bisection *inside* pf_mount (make MOUNT_PHASE=N), to split a stage-2
 * failure into its two very different halves:
 *   1 = stop right after mc_disk_initialize() — card power-on, CMD sequence and
 *       all the SDIO register (re)configuration, but no data-path DMA yet.
 *   2 = stop right after the first boot-sector read — adds one 512-byte DMA.
 * 0 runs the whole mount. If phase 1 already corrupts the picture the problem is
 * the command/register path; if phase 1 is clean and phase 2 is not, it is the
 * sector DMA. Hands the bus back to the plain loop, exactly like SD_STAGE.
 */
#ifndef MOVIECART_MOUNT_PHASE
#define MOVIECART_MOUNT_PHASE 0
#endif

/*
 * Measure the worst gap between served cycles and report which part of the SD
 * path caused it (make GAP_PROBE=1). See bus_service.h. Costs two cycle-counter
 * reads per served cycle, so it is a diagnostic build, not a shipping one.
 */
#ifndef MOVIECART_GAP_PROBE
#define MOVIECART_GAP_PROBE 0
#endif

/* Optional status LED on PA1 (DevEBox has LEDs on PA1/PA2/PA3 on many boards). */
#define STATUS_LED_GPIO			GPIOA
#define STATUS_LED_PIN			GPIO_Pin_1

#define TESTA0_LOW	do { STATUS_LED_GPIO->BSRRH = STATUS_LED_PIN; } while (0);
#define TESTA0_HIGH	do { STATUS_LED_GPIO->BSRRL = STATUS_LED_PIN; } while (0);
#define TESTA1_LOW	do { } while (0);
#define TESTA1_HIGH	do { } while (0);
#define TESTA2_LOW	do { } while (0);
#define TESTA2_HIGH	do { } while (0);

#endif
