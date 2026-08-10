#include "sd_reader.h"
#include "defines.h"
#include "bus_service.h"

#include "stm32f4xx.h"
#include "misc.h"
#include "fatfs_sd_sdio.h"

/* DevEBox card-detect is NC — always present. */
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

static void
sd_wait_with_bus(void)
{
	SDTransferState state;
	do {
		bus_service();
		state = SD_GetStatus();
	} while (state == SD_TRANSFER_BUSY);
}

static bool
disk_read_block_raw(uint32_t sector, uint8_t *buf)
{
	/* Driver expects byte address; HC cards convert internally. */
	SD_Error status = SD_ReadMultiBlocks(buf, ((uint64_t)sector) << 9, 512, 1);
	if (status != SD_OK)
		return false;

	status = SD_WaitReadOperation();
	sd_wait_with_bus();

	if (status != SD_OK || SD_GetStatus() == SD_TRANSFER_ERROR)
		return false;

	return true;
}

uint8_t *
disk_read_block1(uint32_t sector)
{
	if (sector != dinfo.sector1) {
		dinfo.sector1 = sector;
		while (!disk_read_block_raw(dinfo.sector1, diskBuffer)) {
			bus_service();
		}
	}
	return diskBuffer;
}

void
disk_read_block2(uint32_t sector, uint8_t *dst)
{
	if (sector != dinfo.sector2) {
		dinfo.sector2 = sector;
		while (!disk_read_block_raw(dinfo.sector2, dst)) {
			bus_service();
		}
	}
}

bool
mc_disk_initialize(void)
{
	dinfo.sector1 = 0xffffffffu;
	dinfo.sector2 = 0xffffffffu;

	for (int attempt = 0; attempt < 8; attempt++) {
		bus_service();
		if (TM_FATFS_SD_SDIO_disk_initialize() == 0) {
			/* Keep cart EXTI (priority 0) above SDIO/DMA so bus wins during reads. */
			NVIC_InitTypeDef nvic;
			nvic.NVIC_IRQChannel = SDIO_IRQn;
			nvic.NVIC_IRQChannelPreemptionPriority = 1;
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
