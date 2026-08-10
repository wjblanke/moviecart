#include "sd_reader.h"
#include "defines.h"

#include "stm32f4xx.h"
#include "misc.h"
#include "fatfs_sd_sdio.h"

uint8_t
TM_FATFS_CheckCardDetectPin(void)
{
	return 1;
}

struct diskInfo {
	uint32_t sector1;
	uint32_t sector2;
};

static struct diskInfo dinfo;
static uint8_t diskBuffer[512] __attribute__((aligned(4)));

/* Bounded — an errored transfer must not hang forever with no LED code. */
static bool
sd_wait_transfer_done(void)
{
	uint32_t timeout = 20000000u;

	while ((SD_GetStatus() == SD_TRANSFER_BUSY) && --timeout)
		;
	return timeout != 0;
}

static bool
disk_read_block_raw(uint32_t sector, uint8_t *buf)
{
	SD_Error status;

	/* The A12 EXTI keeps the cart alive; it preempts everything here. */
	status = SD_ReadMultiBlocks(buf, ((uint64_t)sector) << 9, 512, 1);
	if (status != SD_OK)
		return false;

	status = SD_WaitReadOperation();
	if (!sd_wait_transfer_done())
		return false;

	if (status != SD_OK || SD_GetStatus() == SD_TRANSFER_ERROR)
		return false;

	return true;
}

uint8_t *
disk_read_block1(uint32_t sector)
{
	if (sector != dinfo.sector1) {
		dinfo.sector1 = sector;
		while (!disk_read_block_raw(dinfo.sector1, diskBuffer))
			;
	}
	return diskBuffer;
}

void
disk_read_block2(uint32_t sector, uint8_t *dst)
{
	if (sector != dinfo.sector2) {
		dinfo.sector2 = sector;
		while (!disk_read_block_raw(dinfo.sector2, dst))
			;
	}
}

bool
mc_disk_initialize(void)
{
	dinfo.sector1 = 0xffffffffu;
	dinfo.sector2 = 0xffffffffu;

	for (int attempt = 0; attempt < 8; attempt++) {
		if (TM_FATFS_SD_SDIO_disk_initialize() == 0) {
			NVIC_InitTypeDef nvic;
			nvic.NVIC_IRQChannel = SDIO_IRQn;
			nvic.NVIC_IRQChannelPreemptionPriority = 3;
			nvic.NVIC_IRQChannelSubPriority = 0;
			nvic.NVIC_IRQChannelCmd = ENABLE;
			NVIC_Init(&nvic);
			nvic.NVIC_IRQChannel = SD_SDIO_DMA_IRQn;
			NVIC_Init(&nvic);
			return true;
		}
	}
	return false;
}
