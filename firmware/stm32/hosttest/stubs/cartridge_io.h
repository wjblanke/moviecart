/* Host stand-in: the data bus is a variable the 6507 emulator reads back. */
#ifndef CARTRIDGE_IO_H
#define CARTRIDGE_IO_H
#include <stdint.h>
extern volatile uint8_t host_data_bus;
#define DATA_OUT_SET(v) do { host_data_bus = (uint8_t)(((uint16_t)(v)) >> 8); } while (0)
#define SET_DATA_MODE_IN
#define SET_DATA_MODE_OUT
#endif
