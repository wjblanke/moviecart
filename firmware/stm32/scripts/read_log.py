#!/usr/bin/env python3
"""Read the MovieCart event log (src/mclog.c) from the STM32 over the ST-Link.

    scripts/read_log.py                 read from the board and print
    scripts/read_log.py --save log.bin  also keep the raw dump
    scripts/read_log.py --file log.bin  decode a dump read earlier

The log lives in RAM: it survives a reset but not a power-off, so read it
before power-cycling the 2600. Messages are looked up in build/firmware.bin,
so run this with the build that is on the board.
"""
import argparse
import glob
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
MAGIC = 0x4D434C47
ENTRIES = 512
HEADER = 20
ENTRY = 12
SIZE = HEADER + ENTRIES * ENTRY
FLASH = 0x08000000


def tool(name):
    hits = glob.glob(os.path.join(FW, "tools", "arm-gnu-toolchain-*", "bin", name))
    return hits[0] if hits else name


def log_address(elf):
    out = subprocess.run([tool("arm-none-eabi-nm"), elf], check=True,
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "mc_log_buf":
            return int(parts[0], 16)
    sys.exit(f"mc_log_buf not found in {elf}: is this a build with the log?")


def read_board(addr, save):
    path = save or tempfile.mktemp(suffix=".bin")
    r = subprocess.run(["st-flash", "read", path, hex(addr), str(SIZE)],
                       capture_output=True, text=True)
    if r.returncode or not os.path.exists(path) or os.path.getsize(path) < SIZE:
        sys.stderr.write(r.stdout + r.stderr)
        sys.exit("st-flash read failed (is the ST-Link connected?)")
    with open(path, "rb") as f:
        data = f.read()
    if not save:
        os.unlink(path)
    return data


def message(image, ptr):
    off = ptr - FLASH
    if 0 <= off < len(image):
        end = image.find(b"\0", off)
        if end > off:
            text = image[off:end].decode("ascii", "replace").strip()
            if text and all(32 <= ord(c) < 127 for c in text):
                return text
    return f"<message at {ptr:#010x}: not in this firmware.bin>"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", default=os.path.join(FW, "build", "firmware.elf"))
    ap.add_argument("--bin", default=os.path.join(FW, "build", "firmware.bin"))
    ap.add_argument("--file", help="decode this dump instead of reading the board")
    ap.add_argument("--save", help="keep the raw dump read from the board here")
    a = ap.parse_args()

    if a.file:
        with open(a.file, "rb") as f:
            data = f.read()
    else:
        data = read_board(log_address(a.elf), a.save)
    with open(a.bin, "rb") as f:
        image = f.read()

    magic, check, boots, nxt, frame = struct.unpack_from("<5I", data, 0)
    if magic != MAGIC or check != (~MAGIC & 0xFFFFFFFF):
        sys.exit("no log in RAM (board power-cycled since boot, or an older build)")

    first = max(0, nxt - ENTRIES)
    print(f"resets since power-on: {boots}   entries: {nxt}"
          f"{f' (oldest {first} overwritten)' if first else ''}   frames this boot: {frame}")
    print(f"{'#':>5} {'frame':>7}  message")
    for n in range(first, nxt):
        fr, ptr, val = struct.unpack_from("<3I", data, HEADER + (n % ENTRIES) * ENTRY)
        print(f"{n:5d} {fr:7d}  {message(image, ptr)}: {val} ({val:#x})")


if __name__ == "__main__":
    main()
