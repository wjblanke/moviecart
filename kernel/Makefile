# MovieCart kernel — core.asm (RamKernel blanking + cart visible kernel)
#
#   make      → core.bin via dasm
#   make run  → Stella

DASM ?= ./dasm/dasm.exe
DASM_FLAGS = -f3 -Idasm/machines/atari2600/
STELLA ?= "C:/Program Files/Stella/Stella.exe"

.PHONY: all run clean check-dasm

all: check-dasm core.bin

check-dasm:
	@test -x $(DASM) || { \
		echo "Missing $(DASM) (dasm assembler for 6502)"; \
		exit 1; \
	}

core.bin core.lst core.sym: core.asm
	$(DASM) core.asm $(DASM_FLAGS) -ocore.bin -lcore.lst -score.sym

run: core.bin
	$(STELLA) -debug core.bin &

clean:
	rm -f core.bin core.lst core.sym riot_sprite_swcha.bin
