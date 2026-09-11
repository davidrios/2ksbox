#!/usr/bin/env bash
# w98-3dmark.sh -- 3DMark 99 Max on a Win98 machine, headless, end to end
# (docs/tracks/m9-tcg-aarch64.md, "Win98 3D"): boot a raw copy of the image
# through tools/win98-game-test.sh (the user's image is never written), wait
# for 3DMark's welcome dialog by its pixels (a fixed sleep missed it once the
# boot got faster), click New Benchmark and Benchmark over a USB tablet,
# screendump the score dialog every 15 s from 150 s after the click, power off.
#
#   tools/w98-3dmark.sh <name> [perf|whole]
#     env: IMG= (default: the claude98 machine, 3DMark installed in
#     C:\ARQUIV~1\3DMARK~1), SHOTS=, DDFLAGS= (32768: vertical blank off),
#     EXTRA= (-perfmap, -d out_asm ...), QEMU_BIN= (a wrapper, e.g. the
#     memtrace preload) -- all passed through to win98-game-test.sh
#     perf:  a --call-graph dwarf profile for PERF_SECS from PERF_AT s after
#            the Benchmark click (the first-person test is ~40-56 s)
#     whole: a -F 199 profile of the whole run (PERF_SECS, default 200)
# Output: build/w98game/<name>/ -- score-*.png (read the score off the last),
# qemu.log (its `ddi: N frames/s` lines are the per-test frame rates),
# click.txt (the click's epoch, to place profile windows), perf.data.
# Settings are the image's own (the user's: 800x600x16, triple buffer,
# Pentium III optimizations). Local only: needs the image.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME=$1; MODE=${2:-}
O=$ROOT/build/w98game/$NAME
LOG=$ROOT/build/w98game/$NAME.run.log
cd "$ROOT"
GUEST_CMD=$'cd \\ARQUIV~1\\3DMARK~1\n3DMARK.EXE' TABLET=1 RUN_SECS=900 SHOTS=${SHOTS:-0} \
  OUT=$O tools/win98-game-test.sh "${IMG:-$HOME/.local/share/2ksbox/machines/claude98/disk.qcow2}" "$NAME" >"$LOG" 2>&1 &
H=$!
q() { python3 tools/qmpc.py "$O/qmp.sock" "$@"; }
until grep -q "s of run" "$LOG"; do
  kill -0 $H 2>/dev/null || { echo "harness ended before the run"; tail "$LOG"; exit 1; }
  sleep 1
done
# 3DMark's welcome dialog: the "New 3DMark Benchmark" button's face is the
# dialog grey at (300,199) and the desktop behind it is not; wait for it
up=0
for i in $(seq 60); do
  sleep 3
  q screendump "$O/wait.png" >/dev/null 2>&1 || continue
  python3 - "$O/wait.png.ppm" <<'PY' && { up=1; break; }
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3)
w = int(d[1].split()[0]); px = d[3]
def at(x, y): o = (y * w + x) * 3; return tuple(px[o:o + 3])
# the button face (Windows grey, 192 or its RGB565 round trip) and the
# dialog title bar's dark blue, right of the title text
g = at(300, 199); t = at(450, 91)
grey = all(185 <= c <= 200 for c in g) and max(g) - min(g) <= 4
# (the title's blue reads (8, 85, 181) on this desktop: blue, no red)
sys.exit(0 if grey and t[2] > 150 and t[0] < 40 else 1)
PY
done
[ $up = 1 ] || { echo "3DMark's welcome dialog never came up"; q json '{"execute":"system_powerdown"}'; wait $H; exit 1; }
sleep 5
q click 368 199 800 600; sleep 8
q screendump "$O/project.png" >/dev/null
q click 448 457 800 600
T0=$(date +%s)
date -u +%s.%N > "$O/click.txt"
if [ "$MODE" = perf ]; then
  sleep "${PERF_AT:-8}"
  P=$(ps -C qemu-system-i386 -o pid= | head -1)
  perf record -F 499 --call-graph dwarf,16384 -p $P -o "$O/perf.data" -- sleep "${PERF_SECS:-10}" 2>&1 | tail -1
elif [ "$MODE" = whole ]; then
  # the whole benchmark, frame-pointer stacks, wall-clock stamped so that
  # `perf report --time` can cut it at the tests' boundaries (qemu.log's
  # timestamped ddi: lines); in the background so the score wait still runs
  P=$(ps -C qemu-system-i386 -o pid= | head -1)
  perf record -F 199 -p $P -o "$O/perf.data" -- sleep "${PERF_SECS:-200}" \
    >"$O/perf.log" 2>&1 &
fi
# the score dialog: the benchmark took ~170 s with vsync on; shoot every 15 s
# from 150 s and keep the last few, the score is the one that stops changing
while [ $(( $(date +%s) - T0 )) -lt 420 ]; do
  sleep 15
  [ $(( $(date +%s) - T0 )) -ge 150 ] && q screendump "$O/score-$(( $(date +%s) - T0 )).png" >/dev/null
done
q json '{"execute":"system_powerdown"}' >/dev/null
wait $H
grep -E "ddi: [0-9.]+ frames" "$O/qemu.log" | sed 's/.*d3dpt-vga: //' > "$O/rates.txt"
echo "done: $O ($(wc -l < "$O/rates.txt") rate lines)"
