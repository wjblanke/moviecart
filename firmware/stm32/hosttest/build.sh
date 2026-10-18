#!/bin/sh
# Build the MovieCart host test: the firmware's own sources compiled for macOS,
# with the STM32 headers and SDIO driver replaced by stand-ins (stubs/) and the
# bus by a 6507 emulator (harness/host_bus.c).
set -e
H=$(cd "$(dirname "$0")" && pwd)
FW=$H/../src
B=$H/out/build
rm -rf "$B" && mkdir -p "$B/fw"
cp "$FW"/*.c "$FW"/*.h "$B/fw/"
# Mach-O wants "segment,section"; the STM32 linker sections mean nothing here.
sed -i '' -E 's/section\("[^"]*"\)/used/g' "$B"/fw/*.c "$B"/fw/*.h
# Let the harness read file-static state (copies only, never the real sources).
sed -i '' -E 's/^__attribute__\(\(used\)\) static struct extentMap extentMap;/__attribute__((used)) struct extentMap extentMap;/' "$B/fw/pff.c"
sed -i '' -E 's/^static uint8_t mc_aud_pack\[/uint8_t mc_aud_pack[/' "$B/fw/core.c"
cp "$H/stubs/cartridge_io.h" "$B/fw/cartridge_io.h"
CFLAGS="-std=gnu11 -O2 -g -Wall -Wno-unused-function -Wno-unused-variable \
	-Wno-unused-but-set-variable -Wno-gnu-label-as-value -Wno-return-type \
	-I$B/fw -I$H/stubs -I$H/harness -DEXAMPLES_DIR=\"$H/../../../examples\""
cd "$B"
for f in core pff sd_reader frame update title_data mclog; do
	clang $CFLAGS -c "fw/$f.c" -o "$f.o"
done
for f in host_main host_bus host_driver; do
	clang $CFLAGS -c "$H/harness/$f.c" -o "$f.o"
done
clang -o "$H/out/moviecart_host" *.o
echo "built $H/out/moviecart_host"
