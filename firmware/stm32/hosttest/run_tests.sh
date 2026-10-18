#!/bin/sh
# Build the host test, make the disk images if needed, and run both scenarios.
# Exits non-zero if either scenario fails.
set -e
H=$(cd "$(dirname "$0")" && pwd)
"$H/build.sh"
[ -f "$H/out/c4k.img" ] && [ -f "$H/out/c32k.img" ] || "$H/make_images.sh"
T=$H/out/moviecart_host
status=0

run() {
	rc=0
	"$T" "$@" > "$H/out/last.log" || rc=$?
	grep -v '^    pf' "$H/out/last.log"	# per-field timeline is in out/last.log
	return $rc
}

echo "== 4 KB clusters: transport (pause, fast-forward, rewind, Reset, Select, auto-advance)"
run "$H/out/c4k.img" --max 6000 || status=1

echo "== 32 KB clusters, MBR: injected read faults (start, finish, header, a read"
echo "   taking 3 frames, a read that never ends), then TINY.MVC (expects blink 7)"
run "$H/out/c32k.img" --max 20000 --fail-begin 50 --fail-finish 120 --corrupt 200 \
	--slow 300 3 --stuck 400 \
	--expect-fatal 7 || status=1

exit $status
