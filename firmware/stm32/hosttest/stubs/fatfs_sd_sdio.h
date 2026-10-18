/* Host stand-in for the SDIO driver: reads a disk image file instead. */
#ifndef HOST_FATFS_SD_SDIO_H
#define HOST_FATFS_SD_SDIO_H
#include "diskio.h"
typedef enum { SD_OK = 0, SD_ERROR = 1 } SD_Error;
DSTATUS TM_FATFS_SD_SDIO_disk_initialize(void);
DRESULT TM_FATFS_SD_SDIO_disk_read(BYTE *buff, DWORD sector, UINT count);
DRESULT TM_FATFS_SD_SDIO_disk_read_begin(BYTE *buff, DWORD sector, UINT count);
DRESULT TM_FATFS_SD_SDIO_disk_read_finish(void);
void TM_FATFS_SD_SDIO_disk_read_abort(void);
int TM_FATFS_SD_SDIO_disk_read_done(void);
SD_Error SD_PreparePlaybackRead(void);
void SD_LowLevel_DeInit(void);
void SD_LowLevel_Init(void);
#endif
