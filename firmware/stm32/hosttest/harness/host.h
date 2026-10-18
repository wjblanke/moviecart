/* Shared declarations for the MovieCart host test harness. */
#ifndef HOST_H
#define HOST_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/* 6507 + bus (host_bus.c) */
extern uint64_t host_cycles;		/* 6507 bus cycles since reset */
extern uint16_t host_cpu_pc;		/* current 6507 PC */
extern uint8_t host_swcha, host_swchb, host_inpt4, host_inpt5;

/* Callbacks into the test (host_main.c) */
void host_on_cart_read(uint16_t cart_off);
void host_on_tia_write(uint8_t reg, uint8_t value, uint64_t cycle);
void host_on_field_read_begin(uint8_t *dst, uint32_t sector, unsigned count);
void host_on_field_read_done(uint8_t *dst, bool ok);
bool host_is_field_buffer(const uint8_t *p);

/* Failure reporting */
void host_fail(const char *fmt, ...) __attribute__((noreturn));
void host_fatal_blink(int code) __attribute__((noreturn));
void host_check_cart_idle(const char *what);

/* Fault injection (host_driver.c): 1-based index of the field read to break, 0 = never */
extern unsigned host_fail_begin_n, host_fail_finish_n, host_corrupt_n;
/* Field read that ends only after host_slow_frames frames, and one that never ends */
extern unsigned host_slow_n, host_slow_frames, host_stuck_n;
extern unsigned host_stat_wait_windows;
extern FILE *host_img;

#endif
