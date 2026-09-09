#!/usr/bin/env bash
# win98-game-test.sh — a real game on the Win98 display driver, headless
# (doc 19, M10). The 9x counterpart of tools/xp-game-test.sh.
#
#   GUEST_CMD='C:\CAVEDOG\TOTALA\TOTALA.EXE' \
#     tools/win98-game-test.sh ~/.local/share/2ksbox/machines/claude98/disk.qcow2 ta
#
# Why this is not tools/win98-driver-test.sh with more flags: that script's
# job is the *install* — it stages three driver binaries and an INF and asks
# whether the desktop comes up. This one starts from an image where all that
# already happened and the user has installed games into it, attaches the
# discs the games want, lets one run for minutes rather than seconds, and
# takes a screendump every few seconds so that "glitched" can be turned into
# a particular frame. It shares the RUN.BAT mechanism and nothing else.
#
# **The user's image is never written.** It is converted to a raw copy once
# (`RAW=`, kept between runs) and every run works on that: mtools cannot
# write into a qcow2, and RUN.BAT has to be staged from outside because this
# guest has no serial line and nothing to type at until the desktop is up.
#
# How a game is started: WIN.INI's `[windows] run=` names `C:\RUN.BAT`, and
# GUEST_CMD is its body. `run=` itself takes a program and drops every
# argument after it (measured 2026-09-08, doc 19), which is why the batch
# exists at all; it is also what lets a run start a game that is *installed*
# rather than staged, since an installed game needs its own directory.
# COMMAND.COM has no use for long file names, so write 8.3 (`ARQUIV~1`).
#
# Env:
#   GUEST_CMD="lines"   the batch body (required). `&` separates nothing here
#                       — one command per line, embedded \n if you need two.
#   CDS="a.cue:b.mds"   discs after the disk, colon-separated. .cue/.mds/.ccd
#                       go through our own cdimage driver (doc 17).
#   RUN_SECS=n          how long to let it run after the desktop is up (180)
#   SETTLE=n            seconds to let the desktop paint after the driver
#                       programs the mode, before the run clock starts (30).
#                       KEYS and CLICKS are timed from the end of it.
#   SHOTS=n             screendump every n s into shots/ (10; 0 turns it off)
#   KEYS="60:ret,90:esc"  QMP keys at t seconds after the desktop is up
#   CLICKS="70:320,240" left click at t seconds, in screen coordinates.
#                       Without TABLET=1 it is done with the PS/2 mouse,
#                       walked there in paced steps with the position read
#                       back from the adapter's cursor registers (qmpc.py
#                       relclick) — the machine the user plays on has no
#                       tablet either
#   JIGGLE=1            move the mouse every second (relative events, like a
#                       hand on it): the reported cursor glitches only show
#                       up while the pointer is moving, and a screendump of a
#                       still pointer says nothing about them
#   TABLET=1            add -usb -device usb-tablet, so CLICKS can aim at an
#                       absolute position. Off by default: it is a hardware
#                       change the guest will find and want a driver for, and
#                       the machine the user plays on has no tablet either.
#   PULL="A.LOG B.TXT"  files to fetch off C:\ afterwards (deleted first, so
#                       what comes back is this run's or nothing). A path
#                       with \ in it is read from that directory.
#   DUMP_EVERY=n        the executor writes every n-th presented frame to
#                       frames/ — what the *game* draws, which a screendump
#                       cannot see while 3D is presenting
#   TRACE=1             D3DPT_DP2_TRACE: one whole frame of DP2 tokens per
#                       touch of frames/trace.on
#   DDFLAGS=n           -device d3dpt-vga,ddflags=N (the bisection knob)
#   VGA=cirrus          the control: the same game on Windows' own inbox
#                       driver. A glitch that is there too is not ours.
#   RAW=path            the raw working copy (default build/w98game/guest.raw)
#   FRESH=1             re-convert it from the image before staging
#   BOOT_WAIT=s         cap on waiting for the desktop (150)
#   OUT=dir             default build/w98game/<name>; also where the QMP
#                       socket lives (qmp.sock), for driving a run by hand
#
# Output: OUT/qemu.log (the device and the driver's own lines), OUT/dbg.log
# (port 0xE9 — the .drv and the VxD, which speak before the register page is
# mapped), shots/*.png, frames/, and whatever PULL named. The summary counts
# the mode programmes, the page flips and the `ddi:` frame lines, because a
# game that renders nothing and a game that renders wrongly look identical
# in a log that only says the driver loaded.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

IMG="${1:?usage: win98-game-test.sh <image.qcow2> <name>}"
NAME="${2:?usage: win98-game-test.sh <image.qcow2> <name>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/guestwait.sh"
OUT="${OUT:-$ROOT/build/w98game/$NAME}"
RAW="${RAW:-$ROOT/build/w98game/guest.raw}"
QEMU="${QEMU_BIN:-$ROOT/build/qemu/qemu-system-i386}"
QIMG="${QEMU_IMG:-$ROOT/build/qemu/qemu-img}"
DRV="$ROOT/guest-tools/out/driver9x"
# In OUT, not a fixed path: two checkouts running this at once shared one
# socket name, and when the other's QEMU exited it unlinked *this* run's
# socket (2026-09-09 — every later QMP verb failed silently, the run could
# not even be powered off). Keep OUT short: AF_UNIX paths are 108 bytes.
SOCK="$OUT/qmp.sock"
BOOT_WAIT="${BOOT_WAIT:-150}"
RUN_SECS="${RUN_SECS:-180}"
SHOTS="${SHOTS:-10}"
export MTOOLS_SKIP_CHECK=1
export D3DPT_EXEC_LIB="${D3DPT_EXEC_LIB:-$ROOT/build/d3dpt/libd3dpt_exec.so}"

[ -x "$QEMU" ] || { echo "no QEMU at $QEMU (QEMU_BIN= to point elsewhere)"; exit 1; }
[ -n "${GUEST_CMD:-}" ] || { echo "GUEST_CMD= is required (the RUN.BAT body)"; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/shots" "$OUT/frames" "$(dirname "$SOCK")" "$(dirname "$RAW")"

if [ ! -f "$RAW" ] || [ "${FRESH:-0}" = 1 ]; then
  echo "==> raw copy of $IMG (the user's image is never written)"
  rm -f "$RAW"; "$QIMG" convert -O raw "$IMG" "$RAW"
fi

# the FAT partition, from the MBR
OFF=$(python3 -c "
import struct
m=open('$RAW','rb').read(512)
for i in range(4):
    e=m[446+i*16:446+(i+1)*16]
    if e[4]: print(struct.unpack_from('<I',e,8)[0]*512); break")
[ -n "$OFF" ] || { echo "no partition in $RAW"; exit 1; }
M="$RAW@@$OFF"

# **Re-stage the driver every run.** The image the user plays on has
# whatever build was installed into it; a run that is testing a change to the
# driver has to carry that change in, and copying the three binaries over the
# installed ones is what `win98-driver-test.sh boot` does for the same
# reason. `NO_DRIVER=1` leaves the image's own build alone, which is how to
# ask "is this the driver or the game".
if [ "${NO_DRIVER:-0}" != 1 ] && [ -f "$DRV/d3dpt9x.drv" ]; then
  echo "==> staging the driver from $DRV"
  mattrib -i "$M" -r ::/WINDOWS/SYSTEM/D3DPT9* 2>/dev/null || true
  mcopy -i "$M" -o "$DRV/d3dpt9x.drv" ::/WINDOWS/SYSTEM/D3DPT9X.DRV
  mcopy -i "$M" -o "$DRV/d3dpt9v.vxd" ::/WINDOWS/SYSTEM/D3DPT9V.VXD
  [ -f "$DRV/d3dpt9hl.dll" ] &&
    mcopy -i "$M" -o "$DRV/d3dpt9hl.dll" ::/WINDOWS/SYSTEM/D3DPT9HL.DLL
fi

# RUN.BAT, and WIN.INI naming it. `exit` closes the DOS box the batch runs
# in: without it COMMAND.COM sits there after the game is launched and the
# machine will not power off, which leaves the FAT dirty and makes the *next*
# boot a ScanDisk — i.e. it looks exactly like the thing under test failing.
# One CRLF line per line of GUEST_CMD: a batch file with bare LF endings is
# read by COMMAND.COM as one long line. A game that lives in its own
# directory usually needs two lines — `cd` and then the EXE — because a DOS
# program's own loader looks for its parts in the *current* directory
# (Blood's `blood.exe` is a DOS/4GW stub, and from C:\ it says
# "Stub exec failed: dos4gw.exe").
{ printf '@echo off\r\n'
  printf '%s\r\n' "$GUEST_CMD" | sed 's/\r$//' | while IFS= read -r l; do printf '%s\r\n' "$l"; done
  printf 'exit\r\n'; } > "$OUT/run.bat"
echo "==> RUN.BAT:"; sed 's/\r$//; s/^/      /' "$OUT/run.bat"
mattrib -i "$M" -r ::/RUN.BAT 2>/dev/null || true
mcopy -i "$M" -o "$OUT/run.bat" ::/RUN.BAT
mcopy -i "$M" -n ::/WINDOWS/WIN.INI "$OUT/win.ini"
python3 - "$OUT/win.ini" <<'PYWIN'
import re, sys
p = sys.argv[1]
b = open(p, 'rb').read()          # binary: WIN.INI is CRLF and text mode eats it
line = b'run=C:\\RUN.BAT'
m = re.search(br'^run=[^\r\n]*', b, re.M | re.I)
if m:
    b = b[:m.start()] + line + b[m.end():]
else:
    m = re.search(br'^\[windows\]\r?\n', b, re.M | re.I)
    if not m:
        sys.exit("WIN.INI has no [windows] section")
    b = b[:m.end()] + line + b'\r\n' + b[m.end():]
open(p, 'wb').write(b)
PYWIN
mcopy -i "$M" -o "$OUT/win.ini" ::/WINDOWS/WIN.INI

# A stale log read back after a run that never wrote one is a session spent
# on the wrong evidence.
for f in ${PULL:-}; do mdel -i "$M" "::/${f//\\//}" 2>/dev/null || true; done

# **The same machine the player builds**, because a difference here is a
# difference in what the run is testing: `launcherx --print-args` on the
# user's own claude98 gives -cpu pentium3, an SB16 on the embed audiodev and
# the disc as an ide-cd on ide.1 with that audiodev on it too. A run with no
# sound card is not a quieter run — Total Annihilation put up "Sound system
# initialization failed" and quit before it drew a frame, which read exactly
# like the display driver failing. The audiodev is `none` here (there is no
# player to play into) but the *device* has to be there.
DRIVES=(-cpu "${CPU:-pentium3}" -audiodev "none,id=snd0" -device "sb16,audiodev=snd0"
        -drive "file=$RAW,format=raw,if=ide,index=0,media=disk")
n=0
IFS=: read -ra CDLIST <<< "${CDS:-}"
for cd in "${CDLIST[@]}"; do
  [ -n "$cd" ] || continue
  [ -f "$cd" ] || { echo "CDS: no such disc $cd"; exit 1; }
  DRIVES+=(-drive "if=none,id=cd$n,media=cdrom,file=$cd"
           -device "ide-cd,bus=ide.1,unit=$n,id=ide-cd$n,drive=cd$n,audiodev=snd0")
  n=$((n + 1))
done
VGAARGS=(-vga none -device "d3dpt-vga,addr=0x02${DDFLAGS:+,ddflags=$DDFLAGS}")
[ "${VGA:-d3dpt}" = d3dpt ] || VGAARGS=(-vga "${VGA}")
USBARGS=(); [ "${TABLET:-0}" = 1 ] && USBARGS=(-usb -device usb-tablet)
[ -n "${DUMP_EVERY:-}" ] && { export D3DPT_DUMP_DIR="$OUT/frames" D3DPT_DUMP_EVERY="$DUMP_EVERY"; }
[ "${TRACE:-0}" = 1 ] && export D3DPT_DP2_TRACE="$OUT/frames/trace.on"

echo "==> booting ${VGA:-d3dpt}, discs: ${CDS:-none}, ${RUN_SECS}s of run -> $OUT"
"$QEMU" -L "$ROOT/qemu/pc-bios" -machine pc -m 256 -accel tcg \
  "${DRIVES[@]}" "${VGAARGS[@]}" "${USBARGS[@]}" \
  -net none -display none -rtc base=localtime \
  -debugcon file:"$OUT/dbg.log" -qmp unix:"$SOCK",server,nowait \
  > "$OUT/qemu.log" 2>&1 &
VM=$!
GW_PID=$VM
trap 'kill $VM 2>/dev/null || true' EXIT

qmp()  { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@" >/dev/null 2>&1 || true; }
shot() { [ "$SHOTS" = 0 ] || qmp screendump "$OUT/shots/$(printf '%s' "$1").png"; }

# **The milestone is the driver programming the mode, not a fixed sleep.**
# `d3dpt-vga: linear mode on` is the moment the desktop exists; on the inbox
# Cirrus there is no such line, so that path falls back to the cap.
echo "==> waiting for the desktop"
t=0
while [ $t -lt "$BOOT_WAIT" ]; do
  sleep 5; t=$((t+5))
  gw_dead && { echo "==> the guest exited after ${t}s"; break; }
  if [ "${VGA:-d3dpt}" = d3dpt ]; then
    grep -q "linear mode on" "$OUT/qemu.log" 2>/dev/null && break
  elif [ $t -ge 40 ]; then break; fi
done
# `linear mode on` is the driver programming the mode, which is minutes
# before the shell is up under TCG — the desktop still has to paint, and
# SETTLE is the only part of this that is a guess. KEYS and CLICKS are timed
# from the end of it, so a run that types at the desktop instead of at the
# game is a SETTLE that was too short.
echo "==> mode after ${t}s, ${SETTLE:-30}s to paint"
sleep "${SETTLE:-30}"
echo "==> ${RUN_SECS}s of run"
shot t000

# The run: one tick a second, so KEYS and CLICKS land near their times and
# JIGGLE looks like a hand on the mouse rather than one teleport.
r=0; prev=-1; r0=$(date +%s); last_shot=0
while [ $r -lt "$RUN_SECS" ]; do
  sleep 1; prev=$r; r=$(( $(date +%s) - r0 ))
  gw_dead && { echo "==> the guest exited ${r}s into the run"; break; }
  for spec in $(printf '%s' "${KEYS:-}" | tr ',' ' '); do
    at=${spec%%:*}
    [ "$at" -le "$r" ] && [ "$at" -gt "$prev" ] && { echo "    t+${r}s keys ${spec#*:}"; qmp keys "${spec#*:}"; }
  done
  if [ -n "${CLICKS:-}" ]; then
    for spec in $(printf '%s' "$CLICKS" | tr ' ' '\n'); do
      at=${spec%%:*}
      if [ "$at" -le "$r" ] && [ "$at" -gt "$prev" ]; then
        xy="${spec#*:}"; echo "    t+${r}s click $xy"
        if [ "${TABLET:-0}" = 1 ]; then qmp click "${xy%%,*}" "${xy##*,}"
        else qmp relclick "${xy%%,*}" "${xy##*,}"; fi
      fi
    done
  fi
  if [ "${JIGGLE:-0}" = 1 ]; then
    dx=$(( (r % 7) * 9 - 27 )); dy=$(( (r % 5) * 11 - 22 ))
    qmp json "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"rel\",\"data\":{\"axis\":\"x\",\"value\":$dx}},{\"type\":\"rel\",\"data\":{\"axis\":\"y\",\"value\":$dy}}]}}"
  fi
  if [ "$SHOTS" != 0 ] && [ $((r - last_shot)) -ge "$SHOTS" ]; then
    last_shot=$r; shot "t$(printf '%03d' $r)"
  fi
done
shot zfinal

# **End with the ACPI power button, never a kill.** A machine that does not
# power off leaves the FAT dirty and the next boot comes up in safe mode with
# no driver — which reads exactly like the driver having failed.
echo "==> power button"
qmp json '{"execute":"system_powerdown"}'
gw_wait_exit "$VM" 90 || { echo "==> did not power off in 90s (a modal dialog swallows the button); killing"; kill $VM 2>/dev/null || true; }
wait $VM 2>/dev/null || true
trap - EXIT

for f in ${PULL:-}; do
  if mcopy -i "$M" -n "::/${f//\\//}" "$OUT/$(basename "${f//\\//}")" 2>/dev/null; then
    echo "pulled    $f"
  else
    echo "pulled    $f — not there (the game wrote none)"
  fi
done

# The summary must not be able to end the run: `grep -c` exits 1 when it
# counts none, and under `set -e` that would throw away everything the run
# just produced.
set +e
echo
echo "=== the driver, through the adapter's DEBUG register"
grep -c "linear mode on" "$OUT/qemu.log" 2>/dev/null | sed 's/^/mode programmed  x/'
grep -oE "linear mode on \([0-9]+x[0-9]+x[0-9]+" "$OUT/qemu.log" 2>/dev/null | sort | uniq -c | sed 's/^/   /'
echo "page flips       $(grep -c 'page flips' "$OUT/qemu.log" 2>/dev/null || echo 0) reports"
grep -oE '[0-9]+ page flips in [0-9.]+ s' "$OUT/qemu.log" 2>/dev/null | tail -5 | sed 's/^/   /'
echo "ddi: frames      $(grep -c '^ddi:' "$OUT/qemu.log" 2>/dev/null || echo 0) reports"
grep -E '^ddi:' "$OUT/qemu.log" 2>/dev/null | tail -5 | sed 's/^/   /'
echo "untracked pixels $(grep -c 'untracked guest pixels' "$OUT/qemu.log" 2>/dev/null || echo 0)"
echo
echo "=== anything the device or the executor complained about"
grep -iE 'refus|reject|invalid|unsupported|unknown|assert|error|fail|out of range|bad ' "$OUT/qemu.log" 2>/dev/null |
  sed 's/^\(.\{0,160\}\).*/\1/' | sort | uniq -c | sort -rn | head -25
echo
echo "=== the 16-bit driver and the VxD (port 0xE9), last 20"
tail -20 "$OUT/dbg.log" 2>/dev/null | sed 's/^/   /'
echo
echo "shots: $(ls "$OUT/shots"/*.png 2>/dev/null | wc -l) in $OUT/shots"
ls "$OUT/frames"/*.ppm 2>/dev/null | wc -l | sed 's/^/frames: /'
