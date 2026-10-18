#include "sd_reader.h"
#include "defines.h"
#include "moviecart_yield.h"
#include "core.h"
#include "mclog.h"
#include "stm32f4xx.h"

#include "fatfs_sd_sdio.h"
#include "diskio.h"

uint8_t
TM_FATFS_CheckCardDetectPin(void)
{
	return 1;
}

struct diskInfo {
	uint32_t sector2;
	uint8_t *dst2;
	uint8_t count2;
};

static struct diskInfo dinfo;
static struct diskInfo pending;
static bool read_pending;

#define DISK_READ_RETRIES	32

/*
 * LRU cache of the single sectors read through disk_read_block1(): boot and
 * directory sectors (and the directory's FAT entries) at mount and open, and
 * the field header probes. Playback itself reads no FAT: pf_map_file() maps the
 * movie's clusters at open. A returned buffer stays valid until a later miss
 * evicts its slot. In SRAM1 because DMA cannot reach CCM.
 */
#define SECTOR_CACHE_SLOTS	8
#define SECTOR_NONE		0xffffffffu

static uint8_t sectorCache[SECTOR_CACHE_SLOTS][512]
	__attribute__((aligned(16), section(".sram1")));
static uint32_t sectorCacheTag[SECTOR_CACHE_SLOTS] =
	{ [0 ... SECTOR_CACHE_SLOTS - 1] = SECTOR_NONE };
static uint32_t sectorCacheUsed[SECTOR_CACHE_SLOTS];	/* 0 = free */
static uint32_t sectorCacheClock;

void
disk_sector_cache_clear(void)
{
	for (unsigned i = 0; i < SECTOR_CACHE_SLOTS; i++) {
		sectorCacheTag[i] = SECTOR_NONE;
		sectorCacheUsed[i] = 0;
	}
}

/*
 * UnoCart disk_read path. SDIO/DMA completion is polled manually in the driver
 * (IRQs stay masked). Each read starts with a yield: outside relaxed mode it
 * waits for a fresh RamKernel entry; in relaxed mode (playback field loads) it
 * does nothing, so begin, finish and any FAT read share the current window.
 */
static bool
disk_read_blocks_raw(uint32_t sector, uint8_t *buf, uint8_t count)
{
	moviecart_bus_yield();
	mc_log("sync read: sector", sector);
	bool ok = TM_FATFS_SD_SDIO_disk_read(buf, sector, count) == RES_OK;
	if (!ok)
		mc_log("sync read: FAILED, sector", sector);
	return ok;
}

/*
 * Forget the last multi-block read so the next disk_read_blocks() really reads.
 * Leaves the sector cache alone: a failed field read says nothing about the
 * FAT sectors, and dropping them would make the retry's seek reread them in
 * the same RamKernel window. disk_sector_cache_clear() forces those rereads.
 */
void
disk_read_invalidate(void)
{
	dinfo.sector2 = 0xffffffffu;
	dinfo.dst2 = NULL;
	dinfo.count2 = 0;
}

bool
disk_read_blocks_begin(uint32_t sector, uint8_t *dst, uint8_t count)
{
	if (read_pending || !count)
		return false;

	moviecart_bus_yield();
	if (TM_FATFS_SD_SDIO_disk_read_begin(dst, sector, count) != RES_OK) {
		mc_log("async read start FAILED, sector", sector);
		/* Leave no armed DMA or half-started CMD18 behind for the retry. */
		TM_FATFS_SD_SDIO_disk_read_abort();
		return false;
	}

	pending.sector2 = sector;
	pending.dst2 = dst;
	pending.count2 = count;
	read_pending = true;
	return true;
}

/*
 * How long a pending read may take to end. Each window polls for up to
 * READ_POLL_US, which leaves the rest of the ~4 ms RamKernel window for the
 * work after the finish; a read still going then waits for the next window
 * (the DMA carries on while the cart is served, and the shown field repeats).
 * The card's own data timeout (SD_READ_DATATIMEOUT, 250 ms) normally ends a
 * stuck read first; READ_WAIT_WINDOWS (~330 ms) is the backstop.
 */
#define READ_POLL_US		1000u
#define READ_WAIT_WINDOWS	20u

static bool
read_wait_done(void)
{
	const uint32_t poll = (SystemCoreClock / 1000000u) * READ_POLL_US;

	for (unsigned w = 0;; w++) {
		uint32_t t0 = DWT->CYCCNT;

		do {
			if (TM_FATFS_SD_SDIO_disk_read_done())
				return true;
		} while (DWT->CYCCNT - t0 < poll);

		if (w + 1u >= READ_WAIT_WINDOWS)
			return false;

		/* Never poll past the window: the 6507 needs the cart back. */
		uint8_t relaxed = mc_sdio_gate_relaxed;
		mc_sdio_gate_relaxed = 0;
		moviecart_wait_ram_kernel_entry();
		mc_sdio_gate_relaxed = relaxed;
	}
}

bool
disk_read_blocks_finish(void)
{
	if (!read_pending)
		return false;

	moviecart_bus_yield();
	if (!read_wait_done()) {
		mc_log("async read STUCK, aborted; sector", pending.sector2);
		/* Stuck: stop the card and the data path; the caller retries. */
		TM_FATFS_SD_SDIO_disk_read_abort();
		read_pending = false;
		return false;
	}
	bool ok = TM_FATFS_SD_SDIO_disk_read_finish() == RES_OK;
	if (!ok) {
		mc_log("async read finish FAILED, sector", pending.sector2);
		TM_FATFS_SD_SDIO_disk_read_abort();
	}
	if (ok) {
		dinfo.sector2 = pending.sector2;
		dinfo.dst2 = pending.dst2;
		dinfo.count2 = pending.count2;
	}
	read_pending = false;
	return ok;
}

/*
 * Cached single-sector read. Always returns the slot the sector is (or was
 * being) read into; *ok says whether that read succeeded. On failure the slot
 * is left free and holds garbage, never another sector's valid data.
 */
static uint8_t *
sector_cache_read(uint32_t sector, bool *ok)
{
	unsigned i, slot = 0;

	sectorCacheClock++;
	for (i = 0; i < SECTOR_CACHE_SLOTS; i++) {
		if (sectorCacheTag[i] == sector) {
			sectorCacheUsed[i] = sectorCacheClock;
			*ok = true;
			return sectorCache[i];
		}
		if (sectorCacheUsed[i] < sectorCacheUsed[slot])
			slot = i;
	}

	/* Miss: refill the least recently used (or a free) slot. */
	sectorCacheTag[slot] = SECTOR_NONE;
	sectorCacheUsed[slot] = 0;
	for (int attempt = 0; attempt < DISK_READ_RETRIES; attempt++) {
		if (disk_read_blocks_raw(sector, sectorCache[slot], 1)) {
			sectorCacheTag[slot] = sector;
			sectorCacheUsed[slot] = sectorCacheClock;
			break;
		}
	}
	*ok = (sectorCacheTag[slot] == sector);
	return sectorCache[slot];
}

/* Cached single-sector read; NULL if every attempt failed. */
uint8_t *
disk_try_read_block1(uint32_t sector)
{
	bool ok;
	uint8_t *buf = sector_cache_read(sector, &ok);

	return ok ? buf : NULL;
}

/* As disk_try_read_block1(), but on failure returns the garbage slot. */
uint8_t *
disk_read_block1(uint32_t sector)
{
	bool ok;

	return sector_cache_read(sector, &ok);
}

bool
disk_read_blocks(uint32_t sector, uint8_t *dst, uint8_t count)
{
	if (sector == dinfo.sector2 && dst == dinfo.dst2 && count == dinfo.count2)
		return true;

	for (int attempt = 0; attempt < DISK_READ_RETRIES; attempt++) {
		if (disk_read_blocks_raw(sector, dst, count)) {
			dinfo.sector2 = sector;
			dinfo.dst2 = dst;
			dinfo.count2 = count;
			return true;
		}
	}

	dinfo.sector2 = 0xffffffffu;
	dinfo.dst2 = NULL;
	dinfo.count2 = 0;
	return false;
}

bool
disk_read_block2(uint32_t sector, uint8_t *dst)
{
	return disk_read_blocks(sector, dst, 1);
}

bool
mc_sd_prepare_playback_read(void)
{
	return SD_PreparePlaybackRead() == SD_OK;
}

void
mc_sd_pins_init(void)
{
	SD_LowLevel_DeInit();
	SD_LowLevel_Init();
}

bool
mc_disk_initialize(void)
{
	mc_log("disk init: start", 0);
	disk_sector_cache_clear();
	dinfo.sector2 = 0xffffffffu;
	dinfo.dst2 = NULL;
	dinfo.count2 = 0;
	read_pending = false;

	uint8_t st = TM_FATFS_SD_SDIO_disk_initialize();
	mc_log("disk init: status (0 ok)", st);
	return st == 0;
}
