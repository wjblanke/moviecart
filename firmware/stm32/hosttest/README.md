# MovieCart host test

Runs the firmware's real boot and playback code on macOS, against FAT32 disk
images and an emulated 6507, and checks every frame. No board, console or TV.

```sh
./run_tests.sh        # build, make images if needed, run both scenarios
```

## What is real and what is stand-in

| Real firmware code (compiled from `../src`) | Stand-in |
|---|---|
| `main.c` boot, title, frame loop, file select, transport | STM32 register headers (`stubs/`) |
| `core.c` cart dispatch, RAM kernel tracker, field swap, audio pack | SDIO driver: reads a disk image (`harness/host_driver.c`) |
| `pff.c` FAT32, directory walk, extent map, seek | Bus loops: a 6507 emulator runs the real boot ROM, visible kernel, OsHead and RAM kernel (`harness/host_bus.c`) |
| `sd_reader.c`, `update.c`, `frame.c`, `title_data.c` | |

The 6507 is stepped bus cycle by bus cycle with NMOS 6502 timing, including
the dummy reads of implied instructions, taken branches, indexed modes, JSR,
RTS and read-modify-write. Every cart read goes through the real
`bus_dispatch()`; every other cycle goes through the real
`mc_track_ram_kernel()`, so the firmware's waits end on exactly the RAM kernel
entries the hardware would produce. `build.sh` copies the sources to `out/`,
strips the STM32 linker-section attributes and makes three file-static objects
visible to the harness; the real sources are never modified.

## What is checked, every frame

- The field put on screen equals the original `.mvc` field, byte for byte, at
  the index the firmware meant to load (header, graphics, colour, background;
  the overlay rows and the deliberately blacked-out edge row are skipped).
- Every GRP, COLUP, COLUBK and COLUPF write the 6507 makes in the visible
  kernel matches the displayed field, in the kernel's order.
- Every AUDV0 write: 192 visible samples, 6 OsHead samples, and the RAM
  kernel's packed-tail nibbles (64 after an even field, 62 after an odd one);
  after each swap, the pack matches the new field's tail. No run of 3 frames
  may hold one level through the picture and another through blanking (a
  60 Hz buzz, as a stale pack gives after a file change).
- Joystick and switch values captured by OsHead match what the console read.
- Each VSYNC-to-VSYNC frame is 3 VSYNC + VBLANK (37 after an even field, 36
  after an odd one) + the pad line + `visibleLines` + 6 OsHead + overscan (24
  even, 23 odd): 262 lines when fields alternate.
- SD rules: reads only while the 6507 runs the RAM kernel, field reads only in
  relaxed mode, word-aligned non-stack DMA targets, one async read at a time.

## Scenarios (`run_tests.sh`)

1. `out/c4k.img`: bare FAT32, 4 KB clusters. Seven movies (NTSC, PAL, 4:3,
   both header formats), a `README.TXT`, a macOS `._lion.mvc` AppleDouble file
   and two movies written interleaved (55 cluster runs each). Pause, resume,
   fast-forward, rewind, the Reset and Select switches, auto-advance through
   every file and wrap back to file 1.
2. `out/c32k.img`: MBR-partitioned FAT32, 32 KB clusters. Five movies with a
   failed CMD18 start, a failed finish and a corrupted header injected into
   field reads 50, 120 and 200, a read that takes 3 frames (300) and one that
   never ends (400, aborted after 20 windows and retried), then the 4 KB
   `TINY.MVC`, which must stop with blink 7.

`make_images.sh` builds both images with `hdiutil`, `newfs_msdos` and
`diskutil` (macOS only); they are sparse files under `out/`. Run the binary
directly for other cases:

```sh
out/moviecart_host out/c4k.img --max 6000 [--quiet-transport]
	[--fail-begin N] [--fail-finish N] [--corrupt N]
	[--slow N FRAMES] [--stuck N] [--expect-fatal CODE]
```

## Not covered

Real timing (relaxed work runs instantly here, and a read poll stands for a
fixed 20 µs, so the RAM kernel window budget,
SD latency and DMA contention need hardware), the TIA picture itself (register
writes are checked, pixels are not), and electrical bus timing.
