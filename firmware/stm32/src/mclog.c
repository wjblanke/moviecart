#include "mclog.h"

#if !(defined(MOVIECART_NO_LOG) && MOVIECART_NO_LOG)

struct mclog mc_log_buf __attribute__((section(".sram2"), aligned(16)));

/* Keep an existing log across a reset; start a fresh one after power-on. */
void
mc_log_boot(void)
{
	if (mc_log_buf.magic != MCLOG_MAGIC || mc_log_buf.check != ~MCLOG_MAGIC) {
		mc_log_buf.boots = 0;
		mc_log_buf.next = 0;
		mc_log_buf.check = ~MCLOG_MAGIC;
		mc_log_buf.magic = MCLOG_MAGIC;
	} else {
		mc_log_buf.boots++;
	}
	mc_log_buf.frame = 0;
	mc_log("boot", mc_log_buf.boots);
}

void
mc_log(const char *msg, uint32_t val)
{
	struct mclog_entry *e = &mc_log_buf.e[mc_log_buf.next % MCLOG_ENTRIES];

	e->frame = mc_log_buf.frame;
	e->msg = msg;
	e->val = val;
	mc_log_buf.next++;
}
#endif
