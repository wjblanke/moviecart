/*
 * Host replacement for the SDIO driver: sectors come from a disk image file.
 * Checks what the real hardware would require (word-aligned, non-stack DMA
 * targets; one async read at a time; SD work only while the cart is idle) and
 * can inject failures into chosen field reads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#include "fatfs_sd_sdio.h"
#include "stm32f4xx.h"
#include "core.h"
#include "host.h"

FILE *host_img;
unsigned host_fail_begin_n, host_fail_finish_n, host_corrupt_n;
unsigned host_slow_n, host_slow_frames, host_stuck_n;
unsigned host_stat_wait_windows;

static struct { BYTE *buf; DWORD sector; UINT count; bool active; bool field; bool stuck; uint64_t ready_at; uint64_t last_poll; unsigned polls; } pend;
static unsigned field_begins, field_finishes;

unsigned host_stat_sync_reads, host_stat_async_reads, host_stat_field_reads;
unsigned host_stat_aborts, host_stat_injected;
unsigned host_stat_relaxed_field_calls, host_stat_unrelaxed_field_calls;

static void
check_dma_target(const BYTE *buf, const char *what)
{
	volatile int local;
	uintptr_t sp = (uintptr_t)&local, p = (uintptr_t)buf;

	if (p & 3)
		host_fail("%s: DMA target %p is not word aligned", what, (void *)buf);
	if (p > sp - (1u << 20) && p < sp + (8u << 20))
		host_fail("%s: DMA target %p is on the stack (CCM on the board)", what, (void *)buf);
}

static bool
read_sectors(BYTE *buf, DWORD sector, UINT count)
{
	if (fseeko(host_img, (off_t)sector * 512, SEEK_SET))
		return false;
	return fread(buf, 512, count, host_img) == count;
}

DSTATUS TM_FATFS_SD_SDIO_disk_initialize(void) { return 0; }
SD_Error SD_PreparePlaybackRead(void) { return SD_OK; }
void SD_LowLevel_DeInit(void) {}
void SD_LowLevel_Init(void) {}

DRESULT
TM_FATFS_SD_SDIO_disk_read(BYTE *buff, DWORD sector, UINT count)
{
	host_check_cart_idle("synchronous SD read");
	if (pend.active)
		host_fail("synchronous read while an async read is pending");
	check_dma_target(buff, "sync read");
	host_stat_sync_reads++;
	return read_sectors(buff, sector, count) ? RES_OK : RES_ERROR;
}

DRESULT
TM_FATFS_SD_SDIO_disk_read_begin(BYTE *buff, DWORD sector, UINT count)
{
	host_check_cart_idle("CMD18 start");
	if (pend.active)
		host_fail("CMD18 started while another is pending");
	check_dma_target(buff, "async read");
	pend.field = host_is_field_buffer(buff);
	if (pend.field) {
		field_begins++;
		host_stat_field_reads++;
		if (mc_sdio_gate_relaxed)
			host_stat_relaxed_field_calls++;
		else
			host_stat_unrelaxed_field_calls++;
		if (field_begins == host_fail_begin_n) {
			host_stat_injected++;
			printf("  [inject] field read %u: CMD18 start fails\n", field_begins);
			return RES_ERROR;
		}
		host_on_field_read_begin(buff, sector, count);
	}
	pend.ready_at = 0;
	pend.stuck = false;
	if (pend.field && field_begins == host_slow_n) {
		pend.ready_at = host_cycles + (uint64_t)host_slow_frames * 262u * 76u;
		host_stat_injected++;
		printf("  [inject] field read %u: takes %u frames\n", field_begins, host_slow_frames);
	}
	if (pend.field && field_begins == host_stuck_n) {
		pend.stuck = true;
		host_stat_injected++;
		printf("  [inject] field read %u: never ends\n", field_begins);
	}
	pend.polls = 0;
	host_stat_async_reads++;
	pend.buf = buff;
	pend.sector = sector;
	pend.count = count;
	pend.active = true;
	return RES_OK;
}

/* Each poll stands for ~20 us of SDIO register reads. */
int
TM_FATFS_SD_SDIO_disk_read_done(void)
{
	host_check_cart_idle("read poll");
	if (!pend.active)
		host_fail("read poll with no read pending");
	host_dwt.CYCCNT += 168u * 20u;
	if (pend.polls++ && host_cycles - pend.last_poll > 76u * 100u)
		host_stat_wait_windows++;	/* polling again after serving a frame */
	pend.last_poll = host_cycles;
	if (pend.stuck)
		return 0;
	return host_cycles >= pend.ready_at;
}

DRESULT
TM_FATFS_SD_SDIO_disk_read_finish(void)
{
	host_check_cart_idle("read finish");
	if (!pend.active)
		host_fail("read finish with no read pending");
	pend.active = false;

	bool ok = read_sectors(pend.buf, pend.sector, pend.count);

	if (pend.field) {
		field_finishes++;
		if (mc_sdio_gate_relaxed)
			host_stat_relaxed_field_calls++;
		else
			host_stat_unrelaxed_field_calls++;
		if (field_finishes == host_fail_finish_n) {
			host_stat_injected++;
			printf("  [inject] field read %u: finish reports an error\n", field_finishes);
			ok = false;
		} else if (field_finishes == host_corrupt_n) {
			host_stat_injected++;
			printf("  [inject] field read %u: header corrupted\n", field_finishes);
			pend.buf[0] ^= 0xff;
		}
		host_on_field_read_done(pend.buf, ok);
	}
	return ok ? RES_OK : RES_ERROR;
}

void
TM_FATFS_SD_SDIO_disk_read_abort(void)
{
	host_stat_aborts++;
	pend.active = false;
}
