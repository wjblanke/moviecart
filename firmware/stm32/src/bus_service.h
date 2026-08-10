#ifndef BUS_SERVICE_H
#define BUS_SERVICE_H

#include <stdint.h>

/* Infinite, IRQ-disabled UnoCart bus loop; never returns. */
void emulate_cartridge(void) __attribute__((noreturn));

/* Compatibility helper used only by currently inactive LED/frame code. */
void bus_serve_cycle(void);

/* MovieCart kernel dispatch (core.c). */
void bus_dispatch(uint16_t lo_address, uint8_t addr_low8);

#endif /* BUS_SERVICE_H */
