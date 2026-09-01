#include "sd_reader.h"
#include "defines.h"

#include "fatfs_sd_sdio.h"
#include "diskio.h"

uint8_t
TM_FATFS_CheckCardDetectPin(void)
{
	return 1;
}

struct diskInfo {
	uint32_t sector1;
	uint32_t sector2;
	uint8_t *dst2;
	uint8_t count2;
};

static struct diskInfo dinfo;
static struct diskInfo pending;
static bool read_pending;

#define DISK_READ_RETRIES	32

/*
 * Scratch sector, with the field buffers, in SRAM2. SD DMA never touches SRAM1.
 */
static uint8_t diskBuffer[512] __attribute__((aligned(16), section(".sram2")));

/*
 * UnoCart disk_read path. SDIO/DMA completion is polled manually in the driver
 * (IRQs stay masked); every wait yields to bus_serve_cycle.
 */
static bool
disk_read_blocks_raw(uint32_t sector, uint8_t *buf, uint8_t count)
{
	return TM_FATFS_SD_SDIO_disk_read(buf, sector, count) == RES_OK;
}

void
disk_read_invalidate(void)
{
	dinfo.sector1 = 0xffffffffu;
	dinfo.sector2 = 0xffffffffu;
	dinfo.dst2 = NULL;
	dinfo.count2 = 0;
}

bool
disk_read_blocks_begin(uint32_t sector, uint8_t *dst, uint8_t count)
{
	if (read_pending || !count)
		return false;

	if (TM_FATFS_SD_SDIO_disk_read_begin(dst, sector, count) != RES_OK)
		return false;

	pending.sector2 = sector;
	pending.dst2 = dst;
	pending.count2 = count;
	read_pending = true;
	return true;
}

bool
disk_read_blocks_finish(void)
{
	if (!read_pending)
		return false;

	bool ok = TM_FATFS_SD_SDIO_disk_read_finish() == RES_OK;
	if (ok) {
		dinfo.sector2 = pending.sector2;
		dinfo.dst2 = pending.dst2;
		dinfo.count2 = pending.count2;
	}
	read_pending = false;
	return ok;
}

uint8_t *
disk_read_block1(uint32_t sector)
{
	if (sector != dinfo.sector1) {
		int attempt;

		for (attempt = 0; attempt < DISK_READ_RETRIES; attempt++) {
			if (disk_read_blocks_raw(sector, diskBuffer, 1)) {
				dinfo.sector1 = sector;
				break;
			}
		}
		if (attempt == DISK_READ_RETRIES)
			dinfo.sector1 = 0xffffffffu;
	}
	return diskBuffer;
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

bool
mc_disk_initialize(void)
{
	dinfo.sector1 = 0xffffffffu;
	dinfo.sector2 = 0xffffffffu;
	dinfo.dst2 = NULL;
	dinfo.count2 = 0;
	read_pending = false;

	return TM_FATFS_SD_SDIO_disk_initialize() == 0;
}
