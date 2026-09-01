#ifndef SD_READER_H
#define SD_READER_H

#include <stdint.h>
#include <stdbool.h>

bool		mc_disk_initialize(void);
bool		mc_sd_prepare_playback_read(void);
void		disk_read_invalidate(void);
uint8_t*	disk_read_block1(uint32_t sector);
bool		disk_read_block2(uint32_t sector, uint8_t *dst);

#endif
