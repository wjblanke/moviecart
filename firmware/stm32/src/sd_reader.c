#include "sd_reader.h"
#include "defines.h"
#include "moviecart_yield.h"

#include "stm32f4xx.h"
#include "misc.h"
#include "fatfs_sd_sdio.h"
#include "bus_service.h"	/* MC_PROBE read-path region markers */

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
/*
 * DMA target in SRAM2 (see the linker script). Keeping it out of SRAM1 lets the
 * sector transfer run on the bus matrix in parallel with bus_dispatch, which
 * reads the title/field buffers and the stack out of SRAM1 every Atari cycle.
 * Same-slave contention here was delaying the data-bus write past the 6507's
 * sample point, corrupting the picture during the one boot-sector read.
 */
static uint8_t diskBuffer[512] __attribute__((aligned(4), section(".sram2")));

#if !MOVIECART_SD_SKIP_CARD_READY
/* Bounded wait that keeps serving the Atari. Avoids SD_GetStatus's CMD13. */
static bool
sd_wait_card_ready(void)
{
	uint32_t timeout = 200000u;

	while (--timeout) {
		SDTransferState st = SD_GetStatus();
		if (st == SD_TRANSFER_OK)
			return true;
		if (st == SD_TRANSFER_ERROR)
			return false;
		moviecart_bus_yield();
	}
	return false;
}
#endif

static bool
disk_read_block_raw(uint32_t sector, uint8_t *buf)
{
	SD_Error status;

#if MOVIECART_SD_POLL_READ
	/*
	 * No DMA: the sector is drained from the FIFO one word per free Atari cycle
	 * inside SD_ReadBlock_Polled, so there is no second bus master and no
	 * SD_WaitReadOperation to follow — the data is already in buf on return.
	 */
	status = SD_ReadBlock_Polled(buf, ((uint64_t)sector) << 9, 512);
	if (status != SD_OK)
		return false;
#else
	/*
	 * DMA moves the sector; every busy-wait inside the SDIO driver yields
	 * back to bus_serve_cycle(), so the Atari keeps getting served.
	 * Single-block read avoids the multi-block CMD12 stop.
	 */
#if MOVIECART_SD_DOUBLE_READ
	/* Positive control: pay the per-sector cost twice (see defines.h). */
	for (int pass = 0; pass < 2; pass++) {
#endif
	MC_PROBE(MC_PHASE_READ_CMD);
	status = SD_ReadBlock(buf, ((uint64_t)sector) << 9, 512);
	if (status != SD_OK)
		return false;

	MC_PROBE(MC_PHASE_READ_DRAIN);
	status = SD_WaitReadOperation();
	if (status != SD_OK)
		return false;
#if MOVIECART_SD_DOUBLE_READ
	}
#endif
#endif

#if !MOVIECART_SD_SKIP_CARD_READY
	MC_PROBE(MC_PHASE_CARD_READY);
	if (!sd_wait_card_ready())
		return false;
#endif

	return true;
}

uint8_t *
disk_read_block1(uint32_t sector)
{
	if (sector != dinfo.sector1) {
		dinfo.sector1 = sector;
		while (!disk_read_block_raw(dinfo.sector1, diskBuffer))
			moviecart_bus_yield();
	}
	return diskBuffer;
}

void
disk_read_block2(uint32_t sector, uint8_t *dst)
{
	if (sector != dinfo.sector2) {
		dinfo.sector2 = sector;
		while (!disk_read_block_raw(dinfo.sector2, dst))
			moviecart_bus_yield();
	}
}

bool
mc_disk_initialize(void)
{
	dinfo.sector1 = 0xffffffffu;
	dinfo.sector2 = 0xffffffffu;

	/*
	 * NVIC is left disabled for SDIO/DMA: IRQs would preempt the bus loop
	 * mid-dispatch. Completion is pumped from SD_WaitReadOperation instead.
	 */
	for (int attempt = 0; attempt < 8; attempt++) {
		if (TM_FATFS_SD_SDIO_disk_initialize() == 0) {
			NVIC_DisableIRQ(SDIO_IRQn);
			NVIC_DisableIRQ(SD_SDIO_DMA_IRQn);
			return true;
		}
		moviecart_bus_yield();
	}
	return false;
}
