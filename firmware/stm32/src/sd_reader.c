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
};

static struct diskInfo dinfo;

#define DISK_READ_RETRIES	32

/*
 * Scratch sector, with the field buffers, in SRAM2. SD DMA never touches SRAM1.
 */
static uint8_t diskBuffer[512] __attribute__((aligned(16), section(".sram2")));

/*
 * UnoCart's disk_read path, one sector at a time: SD_ReadMultiBlocks,
 * SD_WaitReadOperation, then SD_GetStatus until the card is idle. Completion
 * is IRQ-driven, so this must only run inside a WaitCart handoff (IRQs on,
 * 6502 parked in RAM).
 */
static bool
disk_read_block_raw(uint32_t sector, uint8_t *buf)
{
	return TM_FATFS_SD_SDIO_disk_read(buf, sector, 1) == RES_OK;
}

void
disk_read_invalidate(void)
{
	dinfo.sector1 = 0xffffffffu;
	dinfo.sector2 = 0xffffffffu;
	dinfo.dst2 = NULL;
}

uint8_t *
disk_read_block1(uint32_t sector)
{
	if (sector != dinfo.sector1) {
		int attempt;

		for (attempt = 0; attempt < DISK_READ_RETRIES; attempt++) {
			if (disk_read_block_raw(sector, diskBuffer)) {
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
disk_read_block2(uint32_t sector, uint8_t *dst)
{
	if (sector == dinfo.sector2 && dst == dinfo.dst2)
		return true;

	for (int attempt = 0; attempt < DISK_READ_RETRIES; attempt++) {
		if (disk_read_block_raw(sector, dst)) {
			dinfo.sector2 = sector;
			dinfo.dst2 = dst;
			return true;
		}
	}

	dinfo.sector2 = 0xffffffffu;
	dinfo.dst2 = NULL;
	return false;
}

bool
mc_disk_initialize(void)
{
	dinfo.sector1 = 0xffffffffu;
	dinfo.sector2 = 0xffffffffu;
	dinfo.dst2 = NULL;

	return TM_FATFS_SD_SDIO_disk_initialize() == 0;
}
