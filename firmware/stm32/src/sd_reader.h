#ifndef SD_READER_H
#define SD_READER_H

#include <stdint.h>
#include <stdbool.h>

/* SDIO pins + clocks. Call before the 6507 is served so GPIOD RMW is idle. */
void		mc_sd_pins_init(void);
bool		mc_disk_initialize(void);
bool		mc_sd_prepare_playback_read(void);
void		disk_read_invalidate(void);
uint8_t*	disk_read_block1(uint32_t sector);
bool		disk_read_block2(uint32_t sector, uint8_t *dst);
bool		disk_read_blocks(uint32_t sector, uint8_t *dst, uint8_t count);
bool		disk_read_blocks_begin(uint32_t sector, uint8_t *dst, uint8_t count);
bool		disk_read_blocks_finish(void);

#endif
