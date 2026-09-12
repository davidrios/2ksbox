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
#   TEXT_AT=n           read the VGA text page out of VRAM at t seconds too
#                       (always done at the end): a blue screen a key will
#                       continue from has gone by then
#   JIGGLE=1            move the mouse every second (relative events, like a
#                       hand on it): the reported cursor glitches only show
#                       up while the pointer is moving, and a screendump of a
#                       still pointer says nothing about them
#   TABLET=1            add -usb -device usb-tablet, so CLICKS can aim at an
#                       absolute position. Off by default: it is a hardware
#                       change the guest will find and want a driver for, and
#                       the machine the user plays on has no tablet either.
#   STAGE="a.exe b.dat" host files copied to C:\ before the boot (8.3 names
#                       as given; a probe RUN.BAT then starts by name)
#   PULL="A.LOG B.TXT"  files to fetch off C:\ afterwards (deleted first, so
#                       what comes back is this run's or nothing). A path
#                       with \ in it is read from that directory.
#   DUMP_EVERY=n        the executor writes every n-th presented frame to
#                       frames/ — what the *game* draws, which a screendump
#                       cannot see while 3D is presenting
#   TRACE=1             D3DPT_DP2_TRACE: one whole frame of DP2 tokens per
#                       touch of frames/trace.on
#   DDFLAGS=n           -device d3dpt-vga,ddflags=N (the bisection knob)
#   MUSIC=gm|mt32|none  the MPU-401's synth (gm, the launcher's default for
#                       a Win98 machine), or no MPU-401 at all
#   QEMU_TCG_OPTS=a=off,b=on  accelerator switches for an A/B (the convention of
#                       the Python guest tools): -accel tcg,<them>
#   EXTRA="args"        more QEMU arguments, word-split (-perfmap, say, for
#                       `perf report` to name the vCPU's generated code —
#                       but the map is /tmp/perf-<pid>.map, a line per
#                       translated guest instruction, never trimmed and never
#                       deleted: a game that retranslates all the time wrote
#                       7.8 GB of it in five minutes into a tmpfs, i.e. RAM,
#                       and the next job was killed for memory. Delete it
#                       after the `perf report`)
#   VGA=cirrus          the control: the same game on Windows' own inbox
#                       driver. A glitch that is there too is not ours.
#   PLAYER=1            run the machine inside the player instead of a bare
#                       QEMU. Only the player carries a 3D context provider
#                       (doc 12, patches 30/33): a bare qemu-system-i386
#                       registers none, so a Glide or OpenGL title's
#                       grSstWinOpen fails by design and the guest falls back
#                       to software — a Glide run under the plain harness
#                       tests nothing (tools/glide-guest-test.sh has the same
#                       rule). The player opens a real window on this desktop;
#                       the wrapper's own log goes to OUT/wrapper.log and the
#                       player shoots the guest's frame every PLAYER_SHOT_EVERY
#                       guest frames (300) into shots/2ksbox-NNNN.png — the
#                       only way to see a 3D frame headless, since a QMP
#                       screendump shows the VGA surface, frozen while the 3D
#                       device presents. The sound card stays on the `none`
#                       audiodev, so a run makes no noise on the host.
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
# The executor and its DXVK, and on macOS the run environment DXVK needs --
# the same block as scripts/test.sh (see the reasons there: a DYLD_* variable
# given to this script is stripped by SIP at the #!/usr/bin/env exec, and
# never all of /opt/homebrew/lib).
case "$(uname -s)" in Darwin) SO=dylib;; *) SO=so;; esac
export D3DPT_EXEC_LIB="${D3DPT_EXEC_LIB:-$ROOT/build/d3dpt/libd3dpt_exec.$SO}"
export D3DPT_DXVK_LIB="${D3DPT_DXVK_LIB:-$ROOT/build/dxvk/src/d3d9/libdxvk_d3d9.$SO$([ "$SO" = so ] && echo .0)}"
if [ "$SO" = dylib ]; then
  VKLIB=/opt/homebrew/opt/vulkan-loader/lib
  [ -d "$VKLIB" ] || VKLIB=/opt/homebrew/lib
  export DYLD_LIBRARY_PATH="$VKLIB${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
  if [ -z "${VK_ICD_FILENAMES:-}" ]; then
    for f in "$HOME"/VulkanSDK/*/macOS/share/vulkan/icd.d/libkosmickrisp_icd.json; do
      [ -f "$f" ] && export VK_ICD_FILENAMES="$f"
    done
  fi
fi

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

# Programs and data a run wants on C:\ — a probe of ours, a game's config.
for f in ${STAGE:-}; do
  [ -f "$f" ] || { echo "STAGE: no such file $f"; exit 1; }
  mattrib -i "$M" -r "::/$(basename "$f")" 2>/dev/null || true
  mcopy -i "$M" -o "$f" "::/$(basename "$f")"
done

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
# The music devices come with it (doc 20): the OPL3 at the Sound Blaster's
# base and the MPU-401, which is where a DOS game's General MIDI goes (Blood's
# BLOOD.CFG says MidiPort = 0x330). A game configured for a device the machine
# does not have waits on it or plays to nobody. MUSIC=none drops the MPU-401,
# MUSIC=mt32 asks for the other synth.
DRIVES=(-cpu "${CPU:-pentium3}" -audiodev "none,id=snd0" -device "sb16,audiodev=snd0"
        -device "opl3,audiodev=snd0,sbbase=0x220"
        -drive "file=$RAW,format=raw,if=ide,index=0,media=disk")
[ "${MUSIC:-gm}" = none ] || DRIVES+=(-device "mpu401,audiodev=snd0,synth=${MUSIC:-gm}")
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

echo "==> booting ${VGA:-d3dpt}, discs: ${CDS:-none}, ${RUN_SECS}s of run -> $OUT${PLAYER:+ (in the player)}"
# hpet=off is the launcher's Win98 machine too: 98 has no HPET driver.
# The MPU-401 finds the bank relative to the cwd unless told, so tell it.
export LIBSYNTH_SF2="${LIBSYNTH_SF2:-$ROOT/soundfonts/TimGM6mb.sf2}"
read -ra EXTRA_ARGS <<< "${EXTRA:-}"
MACHINE=(-L "$ROOT/qemu/pc-bios" -machine pc,hpet=off -m 256 -accel "tcg${QEMU_TCG_OPTS:+,$QEMU_TCG_OPTS}"
         "${DRIVES[@]}" "${VGAARGS[@]}" "${USBARGS[@]}"
         -net none -rtc base=localtime -msg timestamp=on
         -debugcon file:"$OUT/dbg.log" -qmp unix:"$SOCK",server,nowait
         "${EXTRA_ARGS[@]}")
if [ "${PLAYER:-0}" = 1 ]; then
  # This checkout's player and this checkout's wrapper (CLAUDE.md: a build
  # is never borrowed). The embed library appends -display none itself.
  PLAYER_BIN="$ROOT/target/release/player"
  [ -x "$PLAYER_BIN" ] || { echo "no player at $PLAYER_BIN (cargo build --release)"; exit 1; }
  [ -f "$ROOT/build/glide/libglide2x.so" ] || echo "note: no build/glide/libglide2x.so (scripts/build-glide.sh) — Glide will be refused"
  export QEMU_GLIDE_LIB="${QEMU_GLIDE_LIB:-$ROOT/build/glide/libglide2x.so}"
  export GLIDE_HOST_LOG="${GLIDE_HOST_LOG:-$OUT/wrapper.log}"
  export PLAYER_SHOT_DIR="$OUT/shots" PLAYER_SHOT_EVERY="${PLAYER_SHOT_EVERY:-300}" PLAYER_AUDIO_NULL=1
  "$PLAYER_BIN" -- "${MACHINE[@]}" > "$OUT/qemu.log" 2>&1 &
else
  "$QEMU" "${MACHINE[@]}" -display none > "$OUT/qemu.log" 2>&1 &
fi
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
# The run's own clock is printed in UTC beside every event because QEMU's
# log lines carry `-msg timestamp=on` (UTC too): that is how "linear mode
# on" is placed between two screendumps, or a screen switch that lasted less
# than one screendump interval is placed at all.
ts() { date -u +%T; }
echo "==> ${RUN_SECS}s of run (t+0 = $(ts) UTC)"
shot t000

# **What the screen shows is not all Windows is saying.** A fatal exception
# is written in VGA *text* mode behind the linear frame buffer (doc 19 §15),
# and a guest that "hangs with the screen glitched" at a game's exit is the
# case this exists for: read the text page out of VRAM before the power
# button, the same way tools/win98-driver-test.sh does after every run.
hmp() { python3 "$ROOT/tools/qmpc.py" "$SOCK" json "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"$1\"}}" 2>/dev/null; }
text_screen() {
  local bar0 tag="${1:-final}"
  bar0=$(hmp "info pci" | python3 -c "
import json,sys,re
o = json.load(sys.stdin).get('return','').splitlines()
for i,l in enumerate(o):
    if '1234:3d00' in l:
        for m in o[i+1:i+4]:
            g = re.search(r'prefetchable memory at 0x([0-9a-f]+)', m)
            if g: print(int(g.group(1), 16)); break
        break" 2>/dev/null)
  [ -n "$bar0" ] || return 0
  python3 "$ROOT/tools/qmpc.py" "$SOCK" json \
    "{\"execute\":\"pmemsave\",\"arguments\":{\"val\":$bar0,\"size\":32768,\"filename\":\"$OUT/vram-$tag.bin\"}}" >/dev/null 2>&1 || return 0
  python3 - "$OUT/vram-$tag.bin" <<'PYTXT'
import sys
v = open(sys.argv[1], 'rb').read()
page = v[:80 * 25 * 4]
def plane(n):
    return page[n::4]
# a real text page: printable characters, few attributes, mostly spaces.
# (Not "plane 3 untouched": a text screen written over a 16 bpp desktop
# leaves the desktop's bytes in the planes it does not write — the blue
# screen of tools/win98-bsod-test.sh was refused on that rule, 2026-09-09.)
if not (sum(1 for b in plane(0) if b == 0 or 0x20 <= b < 0x7f) >= 0.9 * len(plane(0))
        and len(set(plane(1))) <= 16
        and sum(1 for b in plane(0) if b == 0x20) >= 0.5 * len(plane(0))):
    sys.exit(0)
rows = []
for r in range(25):
    line = ''.join(chr(v[(r * 80 + c) * 4]) if 32 <= v[(r * 80 + c) * 4] < 127 else ' '
                   for c in range(80))
    rows.append(line.rstrip())
if not any(rows):
    sys.exit(0)
print("==> text   Windows has a VGA text screen up (behind the frame buffer, if that is on):")
for line in rows:
    if line: print("          | " + line)
PYTXT
}
# TEXT_AT=<s> reads it during the run as well — a blue screen that a key
# will continue from is gone by the end.

# The run: one tick a second, so KEYS and CLICKS land near their times and
# JIGGLE looks like a hand on the mouse rather than one teleport.
r=0; prev=-1; r0=$(date +%s); last_shot=0
while [ $r -lt "$RUN_SECS" ]; do
  sleep 1; prev=$r; r=$(( $(date +%s) - r0 ))
  gw_dead && { echo "==> the guest exited ${r}s into the run"; break; }
  for spec in $(printf '%s' "${KEYS:-}" | tr ',' ' '); do
    at=${spec%%:*}
    [ "$at" -le "$r" ] && [ "$at" -gt "$prev" ] && { echo "    t+${r}s $(ts) keys ${spec#*:}"; qmp keys "${spec#*:}"; }
  done
  if [ -n "${TEXT_AT:-}" ] && [ "$TEXT_AT" -le "$r" ] && [ "$TEXT_AT" -gt "$prev" ]; then
    echo "    t+${r}s $(ts) the VGA text page:"; text_screen "t$(printf '%03d' $r)"
  fi
  if [ -n "${CLICKS:-}" ]; then
    for spec in $(printf '%s' "$CLICKS" | tr ' ' '\n'); do
      at=${spec%%:*}
      if [ "$at" -le "$r" ] && [ "$at" -gt "$prev" ]; then
        xy="${spec#*:}"; echo "    t+${r}s $(ts) click $xy"
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

text_screen final

# **End with the ACPI power button, never a kill.** A machine that does not
# power off leaves the FAT dirty and the next boot comes up in safe mode with
# no driver — which reads exactly like the driver having failed. A machine
# that does not answer it is asked, before it is killed, whether its vCPU is
# moving at all: `info registers` twice (the same EIP = it is not), and the
# PIC and APIC (an unmasked irr bit with isr=00 = an interrupt pending and
# never taken) — the CLAUDE.md recipe for a frozen guest, in OUT/hang.txt.
echo "==> power button ($(ts) UTC)"
qmp json '{"execute":"system_powerdown"}'
gw_wait_exit "$VM" 90 || {
  echo "==> did not power off in 90s (a modal dialog swallows the button, or the machine is dead)"
  { echo "=== info registers"; hmp "info registers"; sleep 2; echo "=== info registers, 2 s later"; hmp "info registers"
    echo "=== info pic"; hmp "info pic"; echo "=== info lapic"; hmp "info lapic"; } 2>/dev/null |
    python3 -c "
import sys, json
for l in sys.stdin:
    l = l.rstrip()
    try: print(json.loads(l).get('return', l), end='')
    except Exception: print(l)" > "$OUT/hang.txt" || true
  grep -E "^===|EIP=|irr=|LVT0" "$OUT/hang.txt" | sed 's/^/    /'
  echo "    killing (the rest is in $OUT/hang.txt)"
  kill $VM 2>/dev/null || true
}
wait $VM 2>/dev/null || true
trap - EXIT

for f in ${PULL:-}; do
  if mcopy -i "$M" -n "::/${f//\\//}" "$OUT/$(basename "${f//\\//}")" 2>/dev/null; then
    echo "pulled    $f"
  else
    echo "pulled    $f — not there (the game wrote none)"
  fi
done

# The summary must not be able to end the run, or decide its exit status:
# `grep -c` exits 1 when it counts none, and under `set -e` that would throw
# away everything the run just produced; and with `pipefail` still on, the
# last `ls | wc -l` of an empty frames/ made the whole script exit 2, which
# a wrapper under its own `set -e` (tools/win98-bsod-test.sh) took as a
# failed run and stopped before its verdicts.
set +e +o pipefail
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
if [ "${PLAYER:-0}" = 1 ]; then
  echo
  echo "=== Glide, through the player's 3D provider (glidept: = the dispatcher, wrapper.log = OpenGLide)"
  grep -a -i -E "glidept|glide2x|3dfx" "$OUT/qemu.log" 2>/dev/null | grep -v -i d3dpt | head -10 | sed 's/^/   /'
  if [ -s "$OUT/wrapper.log" ]; then
    echo "   wrapper.log: $(wc -l < "$OUT/wrapper.log") lines; $(grep -a -c -i "grSstWinOpen" "$OUT/wrapper.log") grSstWinOpen"
    grep -a -i -E "error|fail|unsupported|not implemented" "$OUT/wrapper.log" | sort | uniq -c | sort -rn | head -8 | sed 's/^/   /'
  else
    echo "   wrapper.log: empty — the wrapper was never loaded (no grGlideInit reached the host)"
  fi
  echo "   player shots (the guest's frame, 3D included): $(ls "$OUT/shots"/2ksbox-*.png 2>/dev/null | wc -l)"
fi
echo
echo "shots: $(ls "$OUT/shots"/*.png 2>/dev/null | wc -l) in $OUT/shots"
ls "$OUT/frames"/*.ppm 2>/dev/null | wc -l | sed 's/^/frames: /'
