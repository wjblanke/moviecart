#!/bin/sh
# Build the two FAT32 test images in out/ with macOS tools (hdiutil, newfs_msdos,
# diskutil). The images are sparse files; they take little real disk space.
#
#   c4k.img   bare volume (no partition table), 4 KB clusters: 7 movies, a
#             README.TXT, a macOS ._ AppleDouble file, and two movies written
#             interleaved so each is fragmented into many cluster runs
#   c32k.img  MBR partition, 32 KB clusters: 5 movies, then a 4 KB TINY.MVC
set -e
H=$(cd "$(dirname "$0")" && pwd)
E=$H/../../../examples
O=$H/out
mkdir -p "$O"

attach() { hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$1" | head -1 | awk '{print $1}'; }

# --- 4 KB clusters, no partition table ---
rm -f "$O/c4k.img"; truncate -s 300m "$O/c4k.img"
D=$(attach "$O/c4k.img")
newfs_msdos -F 32 -c 8 -v MOVIES4K "/dev/r${D#/dev/}" >/dev/null
diskutil mount "$D" >/dev/null
V=/Volumes/MOVIES4K
echo "not a movie" > $V/README.TXT
cp "$E/title.mvc" $V/title.mvc
cp "$E/lion.mvc" "$O/lion_x.mvc" && xattr -w com.moviecart.test yes "$O/lion_x.mvc"
cp "$O/lion_x.mvc" $V/lion.mvc && rm "$O/lion_x.mvc"	# macOS writes ._lion.mvc
cp "$E/numbers.mvc" $V/numbers.mvc
python3 - "$E" "$V" <<'PY'
import os, sys
E, V = sys.argv[1], sys.argv[2]
a = open(E + '/dancer.mvc', 'rb').read(); b = open(E + '/street.mvc', 'rb').read()
fa = open(V + '/FRAGA.MVC', 'wb'); fb = open(V + '/FRAGB.MVC', 'wb')
for i in range(0, max(len(a), len(b)), 65536):   # interleave: fragment both files
    for f, d in ((fa, a), (fb, b)):
        if i < len(d):
            f.write(d[i:i + 65536]); f.flush(); os.fsync(f.fileno())
PY
cp "$E/20centryfox_pal.mvc" $V/fox_pal.mvc
cp "$E/street_tate.mvc" $V/tate.mvc
sync; diskutil unmount "$D" >/dev/null; hdiutil detach "$D" >/dev/null

# --- 32 KB clusters, MBR partition ---
rm -f "$O/c32k.img"; truncate -s 2300m "$O/c32k.img"
D=$(attach "$O/c32k.img")
diskutil partitionDisk "$D" MBR "MS-DOS FAT32" MOVIES32 100% >/dev/null
diskutil unmount "${D}s1" >/dev/null
newfs_msdos -F 32 -c 64 -v MOVIES32 "/dev/r${D#/dev/}s1" >/dev/null
diskutil mount "${D}s1" >/dev/null
V=/Volumes/MOVIES32
for f in street.mvc cronkite_pal.mvc testpattern.mvc 20centuryfox.mvc output_cronkite.mvc; do
	cp "$E/$f" "$V/$f"
done
head -c 4096 "$E/title.mvc" > $V/TINY.MVC
sync; diskutil unmount "${D}s1" >/dev/null; hdiutil detach "$D" >/dev/null
echo "images in $O"
