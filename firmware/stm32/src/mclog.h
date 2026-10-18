/*
 * Event log in RAM, read over SWD with scripts/read_log.py (st-flash read).
 *
 * A ring of entries in SRAM2, in a section startup never clears, so it
 * survives a reset (not a power-off) and a re-attach of the ST-Link. Each entry
 * is the RamKernel frame count, a message and a 32-bit value. The message is a
 * pointer to a string literal in flash; the reader looks the text up in
 * build/firmware.bin, so read with the binary that is on the board.
 *
 * mc_log() is a handful of stores. Never call it from bus_dispatch() or the
 * A12-high path; anywhere else is fine.
 */
#ifndef MCLOG_H
#define MCLOG_H

#include <stdint.h>

#define MCLOG_MAGIC	0x4d434c47u	/* "MCLG" */
#define MCLOG_ENTRIES	512u

struct mclog_entry {
	uint32_t	frame;
	const char	*msg;
	uint32_t	val;
};

struct mclog {
	uint32_t	magic;
	uint32_t	check;		/* ~magic */
	uint32_t	boots;		/* resets seen since power-on */
	uint32_t	next;		/* total entries written; slot = next % MCLOG_ENTRIES */
	volatile uint32_t frame;	/* RamKernel entries since this boot */
	struct mclog_entry e[MCLOG_ENTRIES];
};

#if defined(MOVIECART_NO_LOG) && MOVIECART_NO_LOG
/* make NO_LOG=1: every log call compiles to nothing. */
static inline void mc_log_boot(void) {}
static inline void mc_log(const char *msg, uint32_t val) { (void)msg; (void)val; }
static inline void mc_log_frame_tick(void) {}
#else
extern struct mclog mc_log_buf;

void	mc_log_boot(void);
void	mc_log(const char *msg, uint32_t val);

/* Once per RamKernel entry (from mc_on_ram_kernel_entry). */
static inline __attribute__((always_inline)) void
mc_log_frame_tick(void)
{
	mc_log_buf.frame++;
}
#endif

#endif
