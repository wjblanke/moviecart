#ifndef BUS_SERVICE_H
#define BUS_SERVICE_H

#include <stdint.h>

void coreInit(void);
void bus_dispatch(uint16_t lo_address, uint8_t addr_low8);
void bus_service(void);

#endif
