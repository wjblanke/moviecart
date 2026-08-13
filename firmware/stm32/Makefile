# MovieCart STM32F407VGT6 (DevEBox) firmware
# Cart buses: A0-A12=PE0-PE12, D0-D7=PD8-PD15; SD via onboard SDIO.

BUILDDIR = build
DEPDIR = .depend/build

CC = arm-none-eabi-gcc
LD = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
STFLASH = st-flash

SOURCES = \
	src/startup_stm32f40xx.s \
	src/system_stm32f4xx.c \
	Libraries/tm_stm32f4_delay/tm_stm32f4_delay.c \
	Libraries/tm_stm32f4_delay/tm_stm32f4_timer_properties.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_rcc.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_gpio.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_dma.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_sdio.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_flash.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_tim.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_exti.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_syscfg.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/misc.c \
	Libraries/tm_stm32f4_gpio/tm_stm32f4_gpio.c \
	Libraries/tm_stm32f4_fatfs/fatfs/drivers/fatfs_sd_sdio.c \
	src/main.c \
	src/core.c \
	src/bus_service.c \
	src/sd_reader.c \
	src/pff.c \
	src/frame.c \
	src/update.c \
	src/title_data.c

INCLUDES = \
	-Isrc \
	-ILibraries/CMSIS/Include \
	-ILibraries/Device/STM32F4xx/Include \
	-ILibraries/STM32F4xx_StdPeriph_Driver/inc \
	-ILibraries/tm_stm32f4_fatfs \
	-ILibraries/tm_stm32f4_fatfs/fatfs \
	-ILibraries/tm_stm32f4_fatfs/fatfs/drivers \
	-ILibraries/tm_stm32f4_delay \
	-ILibraries/tm_stm32f4_gpio

OBJECTS = $(addprefix $(BUILDDIR)/, $(addsuffix .o, $(basename $(SOURCES))))

ELF = $(BUILDDIR)/firmware.elf
HEX = $(BUILDDIR)/firmware.hex
BIN = $(BUILDDIR)/firmware.bin
MAP = $(BUILDDIR)/firmware.map

# Bus stall budget measurement builds (see README):
#   make STALL_TEST=1   coarse sweep, 0-64 us
#   make STALL_TEST=2   fine sweep, 0-1000 ns (sub-cycle)
#   make STALL_TEST=3   interleave proof: 100 ns step every cart cycle
ifneq ($(STALL_TEST),)
DEFS += -DMOVIECART_STALL_TEST=$(STALL_TEST)
endif

# Bisect SD bring-up: make SD_STAGE=1|2|3 (see defines.h)
ifneq ($(SD_STAGE),)
DEFS += -DMOVIECART_SD_STAGE=$(SD_STAGE)
endif

# Bisect *inside* the mount: make MOUNT_PHASE=1|2 (see defines.h)
ifneq ($(MOUNT_PHASE),)
DEFS += -DMOVIECART_MOUNT_PHASE=$(MOUNT_PHASE)
endif

# Title-only baseline, no SD at all: make NO_SD=1. This is the reference build —
# perfect intro screen and a once-per-second heartbeat. Re-check it whenever a
# change touches the serving code, so "did I break the base?" is never a guess.
ifneq ($(NO_SD),)
DEFS += -DMOVIECART_ENABLE_SD=0
endif

# Worst serve-to-serve gap probe, reported on the LED: make GAP_PROBE=1
ifneq ($(GAP_PROBE),)
DEFS += -DMOVIECART_GAP_PROBE=$(GAP_PROBE)
endif

# DMA-free read path: make POLL_READ=1 (see defines.h). Drains the SDIO FIFO one
# word per free Atari cycle instead of using DMA. Optionally override the transfer
# clock divider: make POLL_READ=1 POLL_CLKDIV=0x4E for ~0.6 MHz.
ifneq ($(POLL_READ),)
DEFS += -DMOVIECART_SD_POLL_READ=$(POLL_READ)
endif
ifneq ($(POLL_CLKDIV),)
DEFS += -DMOVIECART_SD_POLL_CLKDIV=$(POLL_CLKDIV)
endif

# SDIO transfer clock for the DMA path: make DMA_CLKDIV=0x00 (24 MHz), 0x02 (12 MHz),
# 0x08 (4.8 MHz, the old default). Faster shortens the window in which a sector read
# can starve the Atari — see defines.h.
ifneq ($(DMA_CLKDIV),)
DEFS += -DMOVIECART_SD_DMA_CLKDIV=$(DMA_CLKDIV)
endif

# One pumped work step per Atari cycle, aligned to the cycle boundary: make PUMP_ALIGN=1
ifneq ($(PUMP_ALIGN),)
DEFS += -DMOVIECART_PUMP_ALIGN=$(PUMP_ALIGN)
endif

# Ablations for locating the per-sector cost (see defines.h). Timing probes are
# unusable here — GAP_PROBE's own per-cycle cost kills the picture — so we add and
# remove work instead of measuring it.
#   make NO_CARD_READY=1   drop the post-read CMD13 poll
#   make DOUBLE_READ=1     positive control: read every sector twice
ifneq ($(NO_CARD_READY),)
DEFS += -DMOVIECART_SD_SKIP_CARD_READY=$(NO_CARD_READY)
endif
ifneq ($(DOUBLE_READ),)
DEFS += -DMOVIECART_SD_DOUBLE_READ=$(DOUBLE_READ)
endif

# Per-sector DMA setup: make FASTPATH=1 restores the M0AR-only retarget. Default is
# the full re-init, which is slower but sets the destination unambiguously.
ifneq ($(FASTPATH),)
DEFS += -DMOVIECART_SD_DMA_FASTPATH=$(FASTPATH)
endif

# WaitCart handoff: make WAITCART=0 reverts to interleaving SD work into spare
# Atari cycles (see defines.h).
ifneq ($(WAITCART),)
DEFS += -DMOVIECART_WAITCART=$(WAITCART)
endif

# Handoff proof: 1 ms stall per frame, no SD code at all. make WAITCART_PROOF=1
ifneq ($(WAITCART_PROOF),)
DEFS += -DMOVIECART_WAITCART_PROOF=$(WAITCART_PROOF)
endif

CFLAGS = \
	-MT $@ -MMD -MP -MF $(DEPDIR)/$*.d \
	$(DEFS) \
	-O2 -fno-tree-vrp -g \
	-mcpu=cortex-m4 -mthumb \
	-mfpu=fpv4-sp-d16 -mhard-float \
	-DSTM32F4XX \
	-DSTM32F40XX \
	-DUSE_STM32F4_DISCOVERY \
	-DHSE_VALUE=8000000 \
	-DUSE_STDPERIPH_DRIVER \
	-ffunction-sections \
	-fdata-sections \
	$(INCLUDES)

LDSCRIPT = stm32f4_flash.ld
LDFLAGS += \
	-T$(LDSCRIPT) \
	-mthumb -mcpu=cortex-m4 \
	-mhard-float \
	--specs=nosys.specs \
	-Wl,--gc-sections \
	-Wl,-Map=$(MAP)

.PHONY: all bin elf hex flash clean
all: bin hex

bin: $(BIN)
hex: $(HEX)
elf: $(ELF)

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(HEX): $(ELF)
	$(OBJCOPY) -O ihex $< $@

$(ELF): $(OBJECTS) $(LDSCRIPT)
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(BUILDDIR)/src/core.o: src/core.c
	mkdir -p $(dir $@)
	mkdir -p .depend/build/src
	$(CC) -c -MT $@ -MMD -MP -MF .depend/build/src/core.d \
		$(DEFS) \
		-O2 -fno-tree-vrp -fno-gcse -fno-crossjumping -fno-toplevel-reorder -g \
		-mcpu=cortex-m4 -mthumb \
		-mfpu=fpv4-sp-d16 -mhard-float \
		-DSTM32F4XX -DSTM32F40XX -DUSE_STM32F4_DISCOVERY \
		-DHSE_VALUE=8000000 -DUSE_STDPERIPH_DRIVER \
		-ffunction-sections -fdata-sections \
		$(INCLUDES) $< -o $@

$(BUILDDIR)/%.o: %.c
	mkdir -p $(dir $@)
	mkdir -p .depend/$(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILDDIR)/%.o: %.s
	mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@

flash: $(BIN)
	$(STFLASH) write $(BIN) 0x8000000

clean:
	rm -rf $(BUILDDIR) .depend

include $(wildcard $(patsubst %,$(DEPDIR)/%.d,$(basename $(SOURCES))))
