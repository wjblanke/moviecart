/*
 * Host replacement for bus_service.c: a cycle-stepped 6507 that runs the real
 * cart code (boot ROM, visible kernel, OsHead) through the real bus_dispatch()
 * and the real RAM kernel out of emulated RIOT RAM. Every A12-low bus cycle
 * goes through the real mc_track_ram_kernel(), so the firmware's waits end on
 * exactly the RAM kernel entry the hardware would produce.
 *
 * Bus timing follows the NMOS 6502, including the dummy reads of implied
 * instructions, taken branches, indexed addressing, JSR, RTS and RMW.
 */
#include <stdio.h>
#include <string.h>

#include "core.h"
#include "bus_service.h"
#include "moviecart_yield.h"
#include "cartridge_io.h"
#include "stm32f4xx_gpio.h"
#include "stm32f4xx_rcc.h"
#include "host.h"

volatile uint8_t host_data_bus;
uint64_t host_cycles;
uint16_t host_cpu_pc;
uint8_t host_swcha = 0xff, host_swchb = 0xff, host_inpt4 = 0xff, host_inpt5 = 0xff;

static uint8_t riot_ram[128];
static bool cpu_started;
static struct { uint8_t a, x, y, sp; bool n, z, c, v, i, d; uint16_t pc; } cpu;

/* Wait bookkeeping, mirroring serve_until_ram_kernel() */
static bool wait_was_out, wait_entry_hit;

/* ---- bus ---- */

static void
track(uint16_t addr)
{
	mc_track_ram_kernel(addr);
	if (!inramkernel)
		wait_was_out = true;
	else if (wait_was_out)
		wait_entry_hit = true;
}

static uint8_t
low_read(uint16_t addr)
{
	if ((addr & 0x280) == 0x080)		/* RIOT RAM (A7=1, A9=0) */
		return riot_ram[addr & 0x7f];
	if ((addr & 0x280) == 0x280) {		/* RIOT I/O */
		switch (addr & 0x7) {
		case 0: return host_swcha;
		case 2: return host_swchb;
		default: return 0;
		}
	}
	switch (addr & 0x0f) {			/* TIA reads */
	case 0x0c: return host_inpt4;
	case 0x0d: return host_inpt5;
	default: return 0;
	}
}

static uint8_t
rd(uint16_t a)
{
	uint16_t addr = a & 0x1fff;

	host_cycles++;
	if (addr & 0x1000) {
		bus_dispatch(addr & 0xfff);
		host_on_cart_read(addr & 0xfff);
		return host_data_bus;
	}
	track(addr);
	return low_read(addr);
}

static void
wr(uint16_t a, uint8_t v)
{
	uint16_t addr = a & 0x1fff;

	host_cycles++;
	if (addr & 0x1000)
		host_fail("6507 wrote %02x to cart address %04x (pc %04x)", v, addr, host_cpu_pc);
	track(addr);
	if ((addr & 0x280) == 0x080) {
		riot_ram[addr & 0x7f] = v;
	} else if (!(addr & 0x80)) {
		uint8_t reg = addr & 0x3f;

		host_on_tia_write(reg, v, host_cycles - 1);
		if (reg == 0x02 && (host_cycles % 76))	/* WSYNC: halt to line end */
			host_cycles += 76 - (host_cycles % 76);
	}
}

/* ---- CPU ---- */

static void nz(uint8_t v) { cpu.n = v & 0x80; cpu.z = !v; }

static void
branch(bool take)
{
	int8_t off = (int8_t)rd(cpu.pc++);

	if (!take)
		return;
	rd(cpu.pc);				/* dummy read of the next opcode */
	uint16_t t = cpu.pc + off;
	if ((t ^ cpu.pc) & 0xff00)
		rd((cpu.pc & 0xff00) | (t & 0xff));
	cpu.pc = t;
}

static uint16_t
abs_addr(void)
{
	uint16_t lo = rd(cpu.pc++);
	uint16_t hi = rd(cpu.pc++);
	return lo | (hi << 8);
}

static uint8_t
rd_indexed(uint8_t idx)
{
	uint16_t base = abs_addr();
	uint16_t ea = base + idx;

	if ((base ^ ea) & 0xff00)
		rd((base & 0xff00) | (ea & 0xff));
	return rd(ea);
}

static void
cpu_reset(void)
{
	uint16_t lo = rd(0x1ffc), hi = rd(0x1ffd);

	cpu.pc = lo | (hi << 8);
	cpu.sp = 0xfd;
	cpu.i = true;
	cpu_started = true;
}

static void
cpu_step(void)
{
	host_cpu_pc = cpu.pc;
	uint8_t op = rd(cpu.pc++);
	uint8_t zp, m;
	uint16_t ea;

	switch (op) {
	case 0x78: rd(cpu.pc); cpu.i = true; break;			/* sei */
	case 0xd8: rd(cpu.pc); cpu.d = false; break;			/* cld */
	case 0x18: rd(cpu.pc); cpu.c = false; break;			/* clc */
	case 0x38: rd(cpu.pc); cpu.c = true; break;			/* sec */
	case 0x9a: rd(cpu.pc); cpu.sp = cpu.x; break;			/* txs */
	case 0xca: rd(cpu.pc); nz(--cpu.x); break;			/* dex */
	case 0xe8: rd(cpu.pc); nz(++cpu.x); break;			/* inx */
	case 0x88: rd(cpu.pc); nz(--cpu.y); break;			/* dey */
	case 0xc8: rd(cpu.pc); nz(++cpu.y); break;			/* iny */
	case 0xea: rd(cpu.pc); break;					/* nop */
	case 0x4a: rd(cpu.pc); cpu.c = cpu.a & 1; cpu.a >>= 1; nz(cpu.a); break;
	case 0x0a: rd(cpu.pc); cpu.c = cpu.a & 0x80; cpu.a <<= 1; nz(cpu.a); break;
	case 0xa8: rd(cpu.pc); cpu.y = cpu.a; nz(cpu.y); break;	/* tay */
	case 0xaa: rd(cpu.pc); cpu.x = cpu.a; nz(cpu.x); break;	/* tax */
	case 0x8a: rd(cpu.pc); cpu.a = cpu.x; nz(cpu.a); break;	/* txa */
	case 0x98: rd(cpu.pc); cpu.a = cpu.y; nz(cpu.a); break;	/* tya */

	case 0xa9: cpu.a = rd(cpu.pc++); nz(cpu.a); break;		/* lda # */
	case 0xa2: cpu.x = rd(cpu.pc++); nz(cpu.x); break;		/* ldx # */
	case 0xa0: cpu.y = rd(cpu.pc++); nz(cpu.y); break;		/* ldy # */
	case 0x29: cpu.a &= rd(cpu.pc++); nz(cpu.a); break;		/* and # */
	case 0x09: cpu.a |= rd(cpu.pc++); nz(cpu.a); break;		/* ora # */
	case 0xe0: m = rd(cpu.pc++); cpu.c = cpu.x >= m; nz(cpu.x - m); break; /* cpx # */
	case 0xc9: m = rd(cpu.pc++); cpu.c = cpu.a >= m; nz(cpu.a - m); break; /* cmp # */

	case 0xa5: zp = rd(cpu.pc++); cpu.a = rd(zp); nz(cpu.a); break;	/* lda zp */
	case 0xa6: zp = rd(cpu.pc++); cpu.x = rd(zp); nz(cpu.x); break;	/* ldx zp */
	case 0xa4: zp = rd(cpu.pc++); cpu.y = rd(zp); nz(cpu.y); break;	/* ldy zp */
	case 0x24: zp = rd(cpu.pc++); m = rd(zp);				/* bit zp */
		cpu.z = !(cpu.a & m); cpu.n = m & 0x80; cpu.v = m & 0x40; break;
	case 0x85: zp = rd(cpu.pc++); wr(zp, cpu.a); break;		/* sta zp */
	case 0x86: zp = rd(cpu.pc++); wr(zp, cpu.x); break;		/* stx zp */
	case 0x84: zp = rd(cpu.pc++); wr(zp, cpu.y); break;		/* sty zp */
	case 0x95: zp = rd(cpu.pc++); rd(zp); wr((uint8_t)(zp + cpu.x), cpu.a); break; /* sta zp,x */
	case 0xb5: zp = rd(cpu.pc++); rd(zp); cpu.a = rd((uint8_t)(zp + cpu.x)); nz(cpu.a); break;
	case 0xe6: zp = rd(cpu.pc++); m = rd(zp); wr(zp, m); m++; wr(zp, m); nz(m); break; /* inc zp */
	case 0xc6: zp = rd(cpu.pc++); m = rd(zp); wr(zp, m); m--; wr(zp, m); nz(m); break; /* dec zp */

	case 0xad: ea = abs_addr(); cpu.a = rd(ea); nz(cpu.a); break;	/* lda abs */
	case 0xae: ea = abs_addr(); cpu.x = rd(ea); nz(cpu.x); break;	/* ldx abs */
	case 0xac: ea = abs_addr(); cpu.y = rd(ea); nz(cpu.y); break;	/* ldy abs */
	case 0x8d: ea = abs_addr(); wr(ea, cpu.a); break;		/* sta abs */
	case 0xbd: cpu.a = rd_indexed(cpu.x); nz(cpu.a); break;		/* lda abs,x */
	case 0xb9: cpu.a = rd_indexed(cpu.y); nz(cpu.a); break;		/* lda abs,y */

	case 0x4c: ea = rd(cpu.pc++); ea |= rd(cpu.pc) << 8; cpu.pc = ea; break; /* jmp */
	case 0x20: {							/* jsr */
		uint8_t lo = rd(cpu.pc++);
		rd(0x100 + cpu.sp);
		wr(0x100 + cpu.sp--, cpu.pc >> 8);
		wr(0x100 + cpu.sp--, cpu.pc & 0xff);
		uint8_t hi = rd(cpu.pc);
		cpu.pc = lo | (hi << 8);
		break;
	}
	case 0x60: {							/* rts */
		rd(cpu.pc);
		rd(0x100 + cpu.sp);
		uint8_t lo = rd(0x100 + (uint8_t)(++cpu.sp));
		uint8_t hi = rd(0x100 + (uint8_t)(++cpu.sp));
		cpu.pc = lo | (hi << 8);
		rd(cpu.pc);
		cpu.pc++;
		break;
	}
	case 0xd0: branch(!cpu.z); break;	/* bne */
	case 0xf0: branch(cpu.z); break;	/* beq */
	case 0x10: branch(!cpu.n); break;	/* bpl */
	case 0x30: branch(cpu.n); break;	/* bmi */
	case 0x90: branch(!cpu.c); break;	/* bcc */
	case 0xb0: branch(cpu.c); break;	/* bcs */
	default:
		host_fail("6507: unimplemented opcode %02x at %04x", op, host_cpu_pc);
	}
}

/* ---- the firmware's bus API ---- */

/* Run the 6507 until the RAM kernel entry (inramkernel false -> true).
 * Stops after the instruction holding the entry fetch. */
static bool
serve_until_entry(uint64_t limit_cycles)
{
	uint64_t end = host_cycles + limit_cycles;

	if (!cpu_started)
		cpu_reset();
	wait_was_out = !inramkernel;
	wait_entry_hit = false;
	while (!wait_entry_hit) {
		if (host_cycles >= end)
			return false;
		cpu_step();
	}
	return true;
}

#define CYCLES_PER_MS	1193u
#define WAIT_LIMIT	(20u * 19912u)	/* 20 frames without an entry = broken */

void
host_check_cart_idle(const char *what)
{
	/* SD work must happen while the 6507 runs the RAM kernel out of RIOT. */
	if (host_cpu_pc & 0x1000)
		host_fail("%s while the 6507 runs cart code (pc %04x)", what, host_cpu_pc);
}

void
moviecart_wait_ram_kernel_entry(void)
{
	if (mc_sdio_gate_relaxed)
		host_fail("frame wait inside relaxed work");
	if (!serve_until_entry(WAIT_LIMIT))
		host_fail("no RAM kernel entry within 20 frames");
	mc_blanking_swap();
}

void
moviecart_bus_yield(void)
{
	if (mc_sdio_gate_relaxed)
		return;
	moviecart_wait_ram_kernel_entry();
}

void
moviecart_delay_ms(uint32_t ms)
{
	unsigned frames = (ms * 60u + 999u) / 1000u;

	if (!frames)
		frames = 1;
	while (frames--)
		moviecart_wait_ram_kernel_entry();
}

int
bus_wait_endframe(uint32_t timeout_ms)
{
	uint64_t end = host_cycles + (uint64_t)timeout_ms * CYCLES_PER_MS;

	do {
		if (host_cycles >= end || !serve_until_entry(end - host_cycles))
			return 0;
	} while (mc_coldstart_entry);
	mc_blanking_swap();
	return 1;
}

/* Wall-clock serve. In the SD build only failure blinks use this, so decode
 * flash_led_slow()'s pattern (1200, n x [300, 400], 1200) into its code. */
void
bus_serve_ms(uint32_t ms)
{
	static int flashes = -1;
	uint64_t end = host_cycles + (uint64_t)ms * CYCLES_PER_MS;

	if (!cpu_started)
		cpu_reset();
	while (host_cycles < end) {
		wait_was_out = !inramkernel;
		cpu_step();
	}
	if (ms == 1200) {
		if (flashes > 0)
			host_fatal_blink(flashes);
		flashes = 0;
	} else if (ms == 300 && flashes >= 0) {
		flashes++;
	}
}

#define LED_REST 60u
#define LED_ON 20u
#define LED_OFF 20u

void
moviecart_stage_blink(uint8_t n)
{
	unsigned i;

	for (i = 0; i < LED_REST; i++)
		moviecart_wait_ram_kernel_entry();
	while (n--) {
		moviecart_wait_ram_kernel_entry();
		for (i = 0; i < LED_ON; i++)
			moviecart_wait_ram_kernel_entry();
		for (i = 0; i < LED_OFF; i++)
			moviecart_wait_ram_kernel_entry();
	}
	for (i = 0; i < LED_REST; i++)
		moviecart_wait_ram_kernel_entry();
}

void
emulate_cartridge(void)
{
	host_fail("emulate_cartridge() reached (a debug stop)");
}

/* Stand-ins for hardware the host does not have */
GPIO_TypeDef host_gpio[5];
DWT_Type host_dwt;
CoreDebug_Type host_coredebug;
uint32_t SystemCoreClock = 168000000u;
void GPIO_Init(GPIO_TypeDef *g, GPIO_InitTypeDef *i) { (void)g; (void)i; }
void RCC_AHB1PeriphClockCmd(uint32_t p, FunctionalState s) { (void)p; (void)s; }
