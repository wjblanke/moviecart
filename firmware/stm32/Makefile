# MovieCart STM32F407VGT6 (DevEBox) firmware
# Cart buses: A0-A12=PE0-PE12, D0-D7=PD8-PD15; SD via onboard SDIO.
#
# Toolchain (pick one):
#   ./scripts/setup-toolchain.sh     # downloads ARM GNU 14.2 into tools/
#   brew install --cask gcc-arm-embedded
#   brew install arm-none-eabi-gcc arm-none-eabi-binutils  # compiler only; needs setup script
#
#   make TOOLCHAIN=/path/to/arm-none-eabi/bin   # override

BUILDDIR = build
DEPDIR = .depend/build

# Prefer a complete toolchain (compiler + newlib headers/libs).
TOOLCHAIN ?= $(firstword $(wildcard tools/arm-gnu-toolchain-*-arm-none-eabi/bin) $(wildcard /Applications/ArmGNUToolchain/*/arm-none-eabi/bin))
ifeq ($(TOOLCHAIN),)
  TOOLCHAIN := $(shell command -v arm-none-eabi-gcc 2>/dev/null | xargs dirname 2>/dev/null)
endif

CC = $(TOOLCHAIN)/arm-none-eabi-gcc
LD = $(TOOLCHAIN)/arm-none-eabi-gcc
OBJCOPY = $(TOOLCHAIN)/arm-none-eabi-objcopy
STFLASH = st-flash

FREESTANDING = -ffreestanding

SOURCES = \
	src/startup_stm32f40xx.s \
	src/system_stm32f4xx.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_rcc.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_gpio.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_dma.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_sdio.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_flash.c \
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

STANDALONE_SOURCES = \
	src/startup_stm32f40xx.s \
	src/system_stm32f4xx.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_rcc.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_gpio.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_dma.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_sdio.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_flash.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_exti.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/stm32f4xx_syscfg.c \
	Libraries/STM32F4xx_StdPeriph_Driver/src/misc.c \
	Libraries/tm_stm32f4_gpio/tm_stm32f4_gpio.c \
	Libraries/tm_stm32f4_fatfs/fatfs/drivers/fatfs_sd_sdio.c \
	src/standalone_test.c \
	src/standalone_bus.c \
	src/sd_reader.c \
	src/pff.c \
	src/frame.c \
	src/update.c

INCLUDES = \
	-Isrc \
	-ILibraries/CMSIS/Include \
	-ILibraries/Device/STM32F4xx/Include \
	-ILibraries/STM32F4xx_StdPeriph_Driver/inc \
	-ILibraries/tm_stm32f4_fatfs \
	-ILibraries/tm_stm32f4_fatfs/fatfs \
	-ILibraries/tm_stm32f4_fatfs/fatfs/drivers \
	-ILibraries/tm_stm32f4_gpio

OBJECTS = $(addprefix $(BUILDDIR)/, $(addsuffix .o, $(basename $(SOURCES))))
STANDALONE_BUILDDIR = build/standalone
STANDALONE_DEPDIR = .depend/standalone
STANDALONE_OBJECTS = $(addprefix $(STANDALONE_BUILDDIR)/, $(addsuffix .o, $(basename $(STANDALONE_SOURCES))))

ELF = $(BUILDDIR)/firmware.elf
HEX = $(BUILDDIR)/firmware.hex
BIN = $(BUILDDIR)/firmware.bin
MAP = $(BUILDDIR)/firmware.map
STANDALONE_ELF = $(BUILDDIR)/standalone_test.elf
STANDALONE_HEX = $(BUILDDIR)/standalone_test.hex
STANDALONE_BIN = $(BUILDDIR)/standalone_test.bin
STANDALONE_MAP = $(BUILDDIR)/standalone_test.map

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

# Bisect SD bring-up: make INIT_PHASE=1..17 (see defines.h)
ifneq ($(INIT_PHASE),)
DEFS += -DMOVIECART_INIT_PHASE=$(INIT_PHASE)
endif

# Drop milestone LED blinks (keep fatalBlink): make NO_LED=1
ifneq ($(NO_LED),)
DEFS += -DMOVIECART_NO_LED=1
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

CFLAGS = \
	-MT $@ -MMD -MP -MF $(DEPDIR)/$*.d \
	$(DEFS) \
	$(FREESTANDING) \
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

STANDALONE_CFLAGS = \
	-MT $@ -MMD -MP -MF $(STANDALONE_DEPDIR)/$*.d \
	$(DEFS) \
	-DMOVIECART_ENABLE_SD=1 \
	-DMOVIECART_STANDALONE_TEST=1 \
	$(FREESTANDING) \
	-Og -g3 -fno-inline \
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

.PHONY: all bin elf hex standalone-test flash flash-standalone-test clean check-toolchain
all: check-toolchain bin hex

standalone-test: check-toolchain $(STANDALONE_BIN) $(STANDALONE_HEX)

check-toolchain:
	@test -x $(CC) || { \
		echo "Missing $(CC)."; \
		echo "Run: ./scripts/setup-toolchain.sh"; \
		echo "  or: brew install --cask gcc-arm-embedded"; \
		exit 1; \
	}
	@$(CC) -print-file-name=include/stdint.h | grep -q stdint.h || { \
		echo "$(CC) has no newlib headers (Homebrew gcc-only install)."; \
		echo "Run: ./scripts/setup-toolchain.sh"; \
		exit 1; \
	}

bin: $(BIN)
hex: $(HEX)
elf: $(ELF)

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(HEX): $(ELF)
	$(OBJCOPY) -O ihex $< $@

$(ELF): $(OBJECTS) $(LDSCRIPT)
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(STANDALONE_BIN): $(STANDALONE_ELF)
	$(OBJCOPY) -O binary $< $@

$(STANDALONE_HEX): $(STANDALONE_ELF)
	$(OBJCOPY) -O ihex $< $@

$(STANDALONE_ELF): $(STANDALONE_OBJECTS) $(LDSCRIPT)
	$(LD) -T$(LDSCRIPT) -mthumb -mcpu=cortex-m4 -mhard-float \
		--specs=nosys.specs -Wl,--gc-sections -Wl,-Map=$(STANDALONE_MAP) \
		-o $@ $(STANDALONE_OBJECTS) $(LDLIBS)

$(BUILDDIR)/src/core.o: src/core.c
	mkdir -p $(dir $@)
	mkdir -p .depend/build/src
	$(CC) -c -MT $@ -MMD -MP -MF .depend/build/src/core.d \
		$(DEFS) \
		$(FREESTANDING) \
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

$(STANDALONE_BUILDDIR)/%.o: %.c
	mkdir -p $(dir $@)
	mkdir -p $(STANDALONE_DEPDIR)/$(dir $*)
	$(CC) -c $(STANDALONE_CFLAGS) $< -o $@

$(STANDALONE_BUILDDIR)/%.o: %.s
	mkdir -p $(dir $@)
	mkdir -p $(STANDALONE_DEPDIR)/$(dir $*)
	$(CC) -c $(STANDALONE_CFLAGS) $< -o $@

flash: $(BIN)
	$(STFLASH) write $(BIN) 0x8000000

flash-standalone-test: $(STANDALONE_BIN)
	$(STFLASH) write $(STANDALONE_BIN) 0x8000000

clean:
	rm -rf $(BUILDDIR) .depend

include $(wildcard $(patsubst %,$(DEPDIR)/%.d,$(basename $(SOURCES))))
include $(wildcard $(patsubst %,$(STANDALONE_DEPDIR)/%.d,$(basename $(STANDALONE_SOURCES))))
