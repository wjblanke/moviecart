# MovieCart kernel — core.asm (RamKernel blanking + cart visible kernel)
#
#   make      → core.bin and core_fw.bin via dasm
#   make fw   → core_fw.bin only. Same ROM minus VisibleBars' top preroll, so
#               line0 is entered the way the firmware enters it. Use it to test
#               the HMOVE phase the RamKernel pad and ColdStart tail set up;
#               core.bin's preroll WSYNCs re-sync it and hide any error.
#   make run  → Stella

DASM ?= ./dasm/dasm.exe
DASM_FLAGS = -f3 -Idasm/machines/atari2600/
STELLA ?= "C:/Program Files/Stella/Stella.exe"

.PHONY: all fw run run-fw clean check-dasm

all: check-dasm core.bin core_fw.bin

fw: check-dasm core_fw.bin

check-dasm:
	@test -x $(DASM) || { \
		echo "Missing $(DASM) (dasm assembler for 6502)"; \
		exit 1; \
	}

core.bin core.lst core.sym: core.asm
	$(DASM) core.asm $(DASM_FLAGS) -ocore.bin -lcore.lst -score.sym

core_fw.bin core_fw.lst core_fw.sym: core.asm
	$(DASM) core.asm $(DASM_FLAGS) -DFW_ENTRY=1 \
		-ocore_fw.bin -lcore_fw.lst -score_fw.sym

run: core.bin
	$(STELLA) -debug core.bin &

run-fw: core_fw.bin
	$(STELLA) -debug core_fw.bin &

clean:
	rm -f core.bin core.lst core.sym riot_sprite_swcha.bin \
		core_fw.bin core_fw.lst core_fw.sym
