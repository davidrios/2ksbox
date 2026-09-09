#!/usr/bin/env bash
# The Win98/Me display driver in a real guest, headless (doc 19, M10) —
# the 9x counterpart of tools/xp-driver-test.sh, at the stage the driver
# is at: install it, boot the machine on `-vga none -device d3dpt-vga`,
# and report what the adapter and the screen say.
#
#   tools/win98-driver-test.sh ~/vms/win98.qcow2 [boot|install]
#
# `install` stages d3dpt9x.drv and its INF into the image's WINDOWS\INF so
# that PnP matches PCI\VEN_1234&DEV_3D00 on the next boot and installs the
# driver with no clicks; `boot` (the default) just boots and looks.
#
# **Never touches the user's image**: it converts a copy to raw once
# (build/w98/win98-m10.raw) and works on that, because mtools cannot write
# into a qcow2 and the driver has to be staged from outside — there is no
# in-guest shell to drive before the display works.
#
# What it prints, and what each line means:
#   BARs        the adapter's PCI base addresses over the boot. The BIOS
#               maps them; if Windows unmaps them, nothing claimed the
#               resources — that is the mini-VDD's job (doc 19), and the
#               16-bit driver then has nothing to map.
#   guest:      the driver's own debug output, through the adapter's DEBUG
#               register into the QEMU log, exactly as on XP.
#   0xE9        the same lines through QEMU's debug console, which works
#               before the register page is mapped — when this is empty
#               too, no code of ours ran at all.
#   colours     the screendump's colour count: 16 or fewer means Windows
#               fell back to VGA and the driver is not driving the screen.
#
# `PROG=<file.exe>` stages a program and names it in WIN.INI's `run=`, so
# Windows starts it once the shell is up — this harness has nothing to
# type at, so that is how anything gets exercised. The DirectDraw half of
# the driver needs it: nothing on a Win98 desktop calls DirectDrawCreate
# on its own, and until something does, the DCICOMMAND escapes and the
# ring-3 HAL are never reached (doc 19 §2). The probe built for exactly
# that is `guest-tools/out/driver9x/ddprobe.exe`:
#
#   PROG=guest-tools/out/driver9x/ddprobe.exe \
#     tools/win98-driver-test.sh <image> install
#
# and the evidence is `d3dpt9dd:` / `d3dpthal:` lines in the guest log
# plus the DDPROBE.LOG it leaves on C:.
#
# The boot is not slept out: the adapter says `linear mode on` the moment the
# driver programs the desktop mode, so the run waits for that and then lets
# the desktop paint for SETTLE seconds. A driver that never loads never says
# it, and then — and only then — the whole BOOT_WAIT is spent before the run
# reports what it found.
#
# `DDFLAGS=<n>` passes the adapter's bisection knob through
# (`-device d3dpt-vga,ddflags=N`); the 9x driver reads the high half of it
# (`D9F_*` in `w9x/d3dpt9x.h`), the NT one the low half.
#
# `BOOT_WAIT=<s>` is that cap (it buys more time on a slow run), `SETTLE=<s>`
# is the paint time after the mode switch, `SHOTS=<s>` adds a screendump
# every <s> seconds (`t<n>.png` in the output directory) so that a screen
# which is merely filling in slowly can be told from one that stopped
# changing, and `OUT=` moves the outputs.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

IMG="${1:?usage: win98-driver-test.sh <win98.qcow2> [boot|install]}"
WHAT="${2:-boot}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/guestwait.sh"
OUT="${OUT:-$ROOT/build/w98}"
DRV="$ROOT/guest-tools/out/driver9x"
QEMU="${QEMU_BIN:-$ROOT/build/qemu/qemu-system-i386}"
RAW="$OUT/win98-m10.raw"
SOCK="/tmp/claude-$(id -u)/w98m10.sock"
BOOT_WAIT="${BOOT_WAIT:-150}"

# What a PROG may leave in C:\ — deleted before the run and read back after,
# so what comes out at the end is this run's or nothing. One list, because
# three copies of it is how a new test's log silently never gets collected.
PROG_OUTPUTS="DDPROBE.LOG D3D7TEST.LOG D3D7TEST.BMP EBTEST.LOG EB1.BMP EB2.BMP EB3.BMP EB4.BMP EB5.BMP CKTEST.LOG DXTTEST.LOG SHTEST.LOG"

[ -x "$QEMU" ] || { echo "no QEMU at $QEMU (QEMU_BIN= to point elsewhere)"; exit 1; }
[ -f "$DRV/d3dpt9x.drv" ] || { echo "run guest-tools/build-driver9x.sh first"; exit 1; }
mkdir -p "$OUT/out" "$(dirname "$SOCK")"

if [ ! -f "$RAW" ] || [ "$WHAT" = install ]; then
  echo "==> raw copy of $IMG (the user's image is never written)"
  rm -f "$RAW"
  "${QEMU_IMG:-$ROOT/build/qemu/qemu-img}" convert -O raw "$IMG" "$RAW"
fi

# the FAT16/32 partition, from the MBR
OFF=$(python3 -c "
import struct,sys
m=open('$RAW','rb').read(512)
for i in range(4):
    e=m[446+i*16:446+(i+1)*16]
    if e[4]: print(struct.unpack_from('<I',e,8)[0]*512); break")

if [ "$WHAT" = install ]; then
  export MTOOLS_SKIP_CHECK=1
  echo "==> staging the driver and its INF (PnP installs it on the next boot)"
  mattrib -i "$RAW@@$OFF" -r ::/WINDOWS/SYSTEM/D3DPT9* ::/WINDOWS/INF/D3DPT9* 2>/dev/null || true
  mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9x.drv" ::/WINDOWS/SYSTEM/D3DPT9X.DRV
  mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9v.vxd" ::/WINDOWS/SYSTEM/D3DPT9V.VXD
  mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9x.drv" ::/WINDOWS/INF/D3DPT9X.DRV
  mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9v.vxd" ::/WINDOWS/INF/D3DPT9V.VXD
  mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9x.inf" ::/WINDOWS/INF/D3DPT9X.INF
  # the ring-3 HAL, if this host could build it: DirectDraw loads it by
  # the name the .drv gives it, so it only ever has to be in SYSTEM
  if [ -f "$DRV/d3dpt9hl.dll" ]; then
    mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9hl.dll" ::/WINDOWS/SYSTEM/D3DPT9HL.DLL
    mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9hl.dll" ::/WINDOWS/INF/D3DPT9HL.DLL
  fi

  # **`NAME_IN_INI=1` names the driver in SYSTEM.INI instead of letting PnP
  # pick it, and it is off by default because it is no longer needed.** PnP
  # writes both halves into the adapter's registry key and
  # `display.drv=pnpdrvr.drv` resolves through it — that is how every 9x
  # display driver loads, the inbox Cirrus in this same image included, and
  # there is no PNPDRVR.DRV file because there is not meant to be one.
  # Proven 2026-09-07 on a pristine image: install, restart, and the second
  # boot brings up the mini-VDD from the registry's `minivdd` value and the
  # driver from its `drv` value with nothing anywhere naming either. What
  # had been missing was the `DelReg` (doc 19 Section 16).
  #
  # Keep it for the question it answers: naming the driver here bypasses the
  # selection Windows would do, which is what you want when the question is
  # "does this build of the driver work" and not what you want when the
  # question is "does it install".
  # both halves into the adapter's registry key and `display.drv=pnpdrvr.drv`
  # resolves through it — that is how every 9x display driver loads, the
  # inbox Cirrus in this same image included, and there is no PNPDRVR.DRV
  # file because there is not meant to be one. Naming the driver in
  # SYSTEM.INI instead bypasses the selection Windows would do, which is
  # exactly what you want when the question is "does the driver work" and
  # exactly what you do not want when the question is "does it install".
  #
  #
  # **In binary, or not at all.** SYSTEM.INI has CRLF line endings and
  # Python's text mode eats them on the way through, which has already cost
  # this track a section header and the run that noticed.
  if [ "${NAME_IN_INI:-0}" = 1 ]; then
  echo "==> naming the driver and the mini-VDD in SYSTEM.INI (NAME_IN_INI=1)"
  mcopy -i "$RAW@@$OFF" -n ::/WINDOWS/SYSTEM.INI "$OUT/system.ini"
  python3 - "$OUT/system.ini" <<'PYINI'
import re, sys
p = sys.argv[1]
b = open(p, 'rb').read()

def section(name):
    m = re.search(br'^\[' + name + br'\]\r?\n', b, re.M | re.I)
    if not m:
        sys.exit("SYSTEM.INI has no [%s] section" % name.decode())
    return m.end()

if b'D3DPT9V.VXD' not in b.upper():
    at = section(b'386Enh')
    b = b[:at] + b'device=C:\\WINDOWS\\SYSTEM\\D3DPT9V.VXD\r\n' + b[at:]

m = re.search(br'^display\.drv=[^\r\n]*', b, re.M | re.I)
if m:
    b = b[:m.start()] + b'display.drv=d3dpt9x.drv' + b[m.end():]
else:
    at = section(b'boot')
    b = b[:at] + b'display.drv=d3dpt9x.drv\r\n' + b[at:]

open(p, 'wb').write(b)
PYINI
  mcopy -i "$RAW@@$OFF" -o "$OUT/system.ini" ::/WINDOWS/SYSTEM.INI
  fi

  # **PROG=<file.exe> runs a program once the shell is up.** This harness
  # has no way to drive the guest — no serial line, no shell, nothing to
  # type at until the display works — so the way to exercise anything is
  # to have Windows start it for us. WIN.INI's `[windows] run=` is that
  # hook: it is a full command line, it runs after the shell, and it
  # needs no shortcut in a Start menu whose folder names are in whatever
  # language the image was installed in. The program's own evidence is
  # whatever it leaves on C:; the driver's is in the QEMU log.
  #
  # In binary, like SYSTEM.INI above, and for the same reason.
  if [ -n "${PROG:-}" ]; then
    [ -f "$PROG" ] || { echo "PROG=$PROG: no such file"; exit 1; }
    pbase="$(basename "$PROG" | tr a-z A-Z)"
    echo "==> staging $pbase and naming it in WIN.INI's run="
    mattrib -i "$RAW@@$OFF" -r "::/$pbase" 2>/dev/null || true
    mcopy -i "$RAW@@$OFF" -o "$PROG" "::/$pbase"
    mcopy -i "$RAW@@$OFF" -n ::/WINDOWS/WIN.INI "$OUT/win.ini"
    python3 - "$OUT/win.ini" "$pbase" <<'PYWIN'
import re, sys
p, prog = sys.argv[1], sys.argv[2].encode()
b = open(p, 'rb').read()
line = b'run=C:\\' + prog
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
    mcopy -i "$RAW@@$OFF" -o "$OUT/win.ini" ::/WINDOWS/WIN.INI
  fi

  # **Turn the logo off and the boot log on.** A boot that stalls behind the
  # splash screen tells you nothing at all — it is a 640x400 bitmap over
  # whatever Windows is actually doing, and the post-install boot is exactly
  # where this track needs to see that. Without the logo the same stall is a
  # text screen with a name on it, and `BOOTLOG.TXT` says which driver was
  # last loaded. MSDOS.SYS is read-only/hidden/system, so the attributes come
  # off and go back on; and it must keep its trailing block of `x` padding
  # comment lines, which is why this rewrites lines rather than the file.
  echo "==> MSDOS.SYS: Logo=0, BootLog=1"
  mattrib -i "$RAW@@$OFF" -a -r -s -h ::/MSDOS.SYS 2>/dev/null || true
  mcopy -i "$RAW@@$OFF" -n ::/MSDOS.SYS "$OUT/msdos.sys"
  python3 - "$OUT/msdos.sys" <<'PYMS'
import re, sys
p = sys.argv[1]
b = open(p, 'rb').read()
for name, val in ((b'Logo', b'0'), (b'BootLog', b'1')):
    m = re.search(br'^' + name + br'=[^\r\n]*', b, re.M | re.I)
    if m:
        b = b[:m.start()] + name + b'=' + val + b[m.end():]
    else:
        o = re.search(br'^\[Options\]\r?\n', b, re.M | re.I)
        if not o:
            sys.exit("MSDOS.SYS has no [Options] section")
        b = b[:o.end()] + name + b'=' + val + b'\r\n' + b[o.end():]
open(p, 'wb').write(b)
PYMS
  mcopy -i "$RAW@@$OFF" -o "$OUT/msdos.sys" ::/MSDOS.SYS
  mattrib -i "$RAW@@$OFF" +r +s +h ::/MSDOS.SYS 2>/dev/null || true
else
  # A `boot` re-stages every binary the run is testing, and nothing else.
  # The three that change while this track is being worked on are the two
  # Watcom ones and the ring-3 HAL DLL — and `PROG`, which is the only way
  # anything on this desktop calls DirectDraw at all. Re-staging them here
  # is what makes an edit-build-test cycle on the DirectDraw half cost one
  # boot rather than a whole `install` (which re-converts the image).
  # WIN.INI already names PROG from the install that set it up; a different
  # PROG than that one needs the install again.
  export MTOOLS_SKIP_CHECK=1
  mattrib -i "$RAW@@$OFF" -r ::/WINDOWS/SYSTEM/D3DPT9* 2>/dev/null || true
  mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9x.drv" ::/WINDOWS/SYSTEM/D3DPT9X.DRV
  mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9v.vxd" ::/WINDOWS/SYSTEM/D3DPT9V.VXD
  [ -f "$DRV/d3dpt9hl.dll" ] &&
    mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9hl.dll" ::/WINDOWS/SYSTEM/D3DPT9HL.DLL
  if [ -n "${PROG:-}" ]; then
    [ -f "$PROG" ] || { echo "PROG=$PROG: no such file"; exit 1; }
    pbase="$(basename "$PROG" | tr a-z A-Z)"
    mattrib -i "$RAW@@$OFF" -r "::/$pbase" 2>/dev/null || true
    mcopy -i "$RAW@@$OFF" -o "$PROG" "::/$pbase"
    mcopy -i "$RAW@@$OFF" -n ::/WINDOWS/WIN.INI "$OUT/win.ini"
    python3 - "$OUT/win.ini" "$pbase" <<'PYWIN'
import re, sys
p, prog = sys.argv[1], sys.argv[2].encode()
b = open(p, 'rb').read()
line = b'run=C:\\' + prog
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
    mcopy -i "$RAW@@$OFF" -o "$OUT/win.ini" ::/WINDOWS/WIN.INI
  fi
fi

# A stale log read back after a run that never wrote one is a whole session
# spent on the wrong evidence: delete what the last run left before this one
# starts, so what comes out at the end is this run's or nothing.
for f in $PROG_OUTPUTS; do
  mdel -i "$RAW@@$OFF" "::/$f" 2>/dev/null || true
done

rm -f "$OUT/out/dbg.log" "$OUT/out/stderr.log" "$OUT/out"/t*.ppm* "$OUT/out"/t*.png
echo "==> booting on -vga none -device d3dpt-vga"
"$QEMU" -L "$ROOT/qemu/pc-bios" -machine pc -m 256 -accel tcg \
  -drive file="$RAW",format=raw,if=ide,index=0 -vga none \
  -device d3dpt-vga${DDFLAGS:+,ddflags=$DDFLAGS} \
  -net none -display none -rtc base=localtime \
  -debugcon file:"$OUT/out/dbg.log" -qmp unix:"$SOCK",server,nowait \
  > "$OUT/out/stderr.log" 2>&1 &
VM=$!
trap 'kill $VM 2>/dev/null || true' EXIT

hmp() { python3 "$ROOT/tools/qmpc.py" "$SOCK" json "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"$1\"}}"; }
bars() { hmp "info pci" | python3 -c "
import json,sys
o=json.load(sys.stdin).get('return','').splitlines()
for i,l in enumerate(o):
    if '1234:3d00' in l:
        print('BARs  %-6s %s' % ('$1', ' '.join(x.strip() for x in o[i+1:i+4]))); break"; }

# A screendump every SHOTS seconds, because the interesting question through
# most of this track is not what the screen ends on but *when* it stopped
# changing: a desktop that is merely slow under TCG fills in over the run,
# and one that is never painted does not. Off by default (a dump is a
# millisecond of the guest's time, but a hundred files is noise).
shots() {
  [ -n "${SHOTS:-}" ] || return 0
  local t=$1
  python3 "$ROOT/tools/qmpc.py" "$SOCK" screendump "$OUT/out/t$t.ppm" >/dev/null 2>&1 || true
}

# The boot, in SHOTS-second (or single-jump) steps, with the BARs read at
# the three moments that have ever differed.
sleep 6;  bars bios
GW_PID=$VM
step=${SHOTS:-8}
t=6
mode_at=
while [ $t -lt "$BOOT_WAIT" ]; do
  n=$(( t + step > BOOT_WAIT ? BOOT_WAIT - t : step ))
  sleep $n; t=$(( t + n ))
  [ $t -ge 30 ] && [ $(( t - n )) -lt 30 ] && bars 30s
  shots $t
  gw_dead && { echo "==> the guest exited after ${t}s"; break; }
  # the driver programming the mode is the milestone; the desktop still has
  # to paint, and SETTLE is the only part of this that is a guess
  if [ -z "$mode_at" ] && grep -q "linear mode on" "$OUT/out/stderr.log" 2>/dev/null; then
    mode_at=$t
    echo "==> the driver programmed the mode after ${t}s; ${SETTLE:-25}s to paint"
    sleep "${SETTLE:-25}"; t=$(( t + ${SETTLE:-25} )); shots $t
    break
  fi
done
[ -n "$mode_at" ] || echo "==> no 'linear mode on' in ${t}s: the driver never programmed the mode"
bars boot

# **Let the install finish.** PnP puts the driver in the registry and then
# asks to restart, and until that restart happens the switch-over is not
# done: the device still has the devnode the generic VGA driver was on, and
# the boot after a run that answered No comes up on the VGA. So `install`
# answers Yes and watches the second boot — which is the one that shows
# whether the registry alone is enough, with `display.drv=pnpdrvr.drv` (the
# magic name Windows writes for every PnP display driver, Cirrus included —
# there is no such file, and there is not meant to be).
if [ "$WHAT" = install ]; then
  echo "==> restarting to finish the install"
  python3 "$ROOT/tools/qmpc.py" "$SOCK" keys ret >/dev/null 2>&1 || true
  # The post-install boot is much slower than an ordinary one — PnP
  # re-enumerates and the registry is rebuilt — so it gets its own, longer
  # budget. A run that cuts it short reports "the driver did not load" about
  # a machine that is still showing the boot logo. It waits for the mode to
  # be programmed a second time (the first was this boot's), and only a
  # driver that never loads spends the whole budget.
  seen=$(grep -c "linear mode on" "$OUT/out/stderr.log" 2>/dev/null || true)
  want=$(( ${seen:-0} + 1 ))
  if gw_wait_count "$OUT/out/stderr.log" "linear mode on" "$want" "${RESTART_WAIT:-$((BOOT_WAIT * 2))}"; then
    sleep "${SETTLE:-25}"
  fi
  bars restart
fi

python3 "$ROOT/tools/qmpc.py" "$SOCK" screendump "$OUT/out/screen.png" >/dev/null
echo "colours   $(identify -format '%wx%h %k' "$OUT/out/screen.png" 2>/dev/null || echo '?')  ($OUT/out/screen.png)"

# **What the screen shows is not all Windows is saying.** When the guest
# faults, Windows puts its message up in VGA *text* mode — and the adapter is
# scanning out a linear frame buffer, so nobody sees it: the screendump is a
# black desktop with a wait cursor, which reads exactly like a driver that is
# merely slow. The text is still in VRAM, because QEMU's VGA core keeps its
# planes interleaved four bytes to a character cell from offset 0, which is
# also the top of our frame buffer — the band of coloured noise across the
# first 32 KB of every one of these screendumps *is* the message. So read it.
text_screen() {
  local bar0
  bar0=$(hmp "info pci" | python3 -c "
import json,sys,re
o = json.load(sys.stdin).get('return','').splitlines()
for i,l in enumerate(o):
    if '1234:3d00' in l:
        for m in o[i+1:i+4]:
            g = re.search(r'prefetchable memory at 0x([0-9a-f]+)', m)
            if g: print(int(g.group(1), 16)); break
        break")
  [ -n "$bar0" ] || return 0
  python3 "$ROOT/tools/qmpc.py" "$SOCK" json \
    "{\"execute\":\"pmemsave\",\"arguments\":{\"val\":$bar0,\"size\":32768,\"filename\":\"$OUT/out/vram.bin\"}}" >/dev/null 2>&1 || return 0
  python3 - "$OUT/out/vram.bin" <<'PYTXT'
import sys
v = open(sys.argv[1], 'rb').read()
page = v[:80 * 25 * 4]

# **Is this a text page at all?** Once the driver is running, the first
# 32 KB of VRAM is the top of the desktop, and reading every fourth byte of
# it as a character code prints stray letters — a 16 bpp desktop produced a
# convincing-looking three lines of nonsense the first time this ran. Three
# things are true of a real text page and of almost no pixel data: QEMU's
# VGA leaves plane 3 alone (plane 2 it does not — that is the character
# generator), a message screen draws on a handful of attributes, and it is
# mostly spaces. A 32 bpp desktop passes the first of those on its unused
# byte, which is why there are three.
def plane(n):
    return page[n::4]

if not (sum(1 for b in plane(3) if b == 0) >= 0.99 * len(plane(3))
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
print("text      Windows has a VGA text screen up behind the frame buffer:")
for line in rows:
    if line: print("          | " + line)
PYTXT
}
text_screen
# whatever PROG left behind, if it left anything
for f in $PROG_OUTPUTS; do
  rm -f "$OUT/out/$f"
  if mcopy -i "$RAW@@$OFF" -n "::/$f" "$OUT/out/$f" 2>/dev/null; then
    echo "$f:"
    case "$f" in
      *.BMP) echo "          (extracted bitmap: $OUT/out/$f)" ;;
      *)     sed 's/^/          /' "$OUT/out/$f" ;;
    esac
  fi
done
echo "0xE9      $(wc -c < "$OUT/out/dbg.log") bytes"
sed 's/^/          /' "$OUT/out/dbg.log" | head -20
echo "guest:    $(grep -c 'd3dpt-vga: guest' "$OUT/out/stderr.log" || true) lines"
grep 'd3dpt-vga' "$OUT/out/stderr.log" | sed 's/^/          /' | head -20 || true

# A killed Win98 leaves the FAT dirty, and the boot after that comes up in
# **safe mode** with no driver and no VxD — which looks exactly like the
# driver having failed, and costs a whole run to work out. The ACPI power
# button is the reliable way to end a run: this is an ACPI install (it has
# to be, or the adapter is never seen), and Windows shuts down and powers
# the machine off by itself, with no dependence on what is on screen. The
# Start menu is the fallback for when it does not, and it starts by
# dismissing whatever modal dialog may be swallowing the keys.
echo "==> shutdown"
# The order matters and each step is here for a run it cost. `alt+n` answers
# the one dialog this harness *knows* is up at the end of an `install` ("to
# finish setting up your new hardware, you must restart your computer" — No,
# because the restart is the next run's job, and Escape alone has been seen
# not to reach it). Escape then clears anything else modal, because a dialog
# swallows the power button as surely as it swallows keys. Only then the ACPI
# button, which is the reliable one: this is an ACPI install (it has to be, or
# the adapter is never seen) and Windows powers the machine off by itself with
# no dependence on what is on screen.
for k in alt+n esc; do
  python3 "$ROOT/tools/qmpc.py" "$SOCK" keys $k >/dev/null 2>&1 || true
  sleep 3
done
python3 "$ROOT/tools/qmpc.py" "$SOCK" json '{"execute":"system_powerdown"}' >/dev/null 2>&1 || true
for _ in $(seq 40); do kill -0 $VM 2>/dev/null || break; sleep 3; done

if kill -0 $VM 2>/dev/null; then
  echo "           the power button was ignored; trying the Start menu"
  for k in ret esc ctrl+esc u ret; do
    python3 "$ROOT/tools/qmpc.py" "$SOCK" keys $k >/dev/null 2>&1 || true
    sleep 4
  done
  for _ in $(seq 40); do kill -0 $VM 2>/dev/null || break; sleep 3; done
fi

if kill -0 $VM 2>/dev/null; then
  # What is on the screen *now* is the only evidence of what refused to go
  # away, and without it the next run is spent finding out.
  python3 "$ROOT/tools/qmpc.py" "$SOCK" screendump "$OUT/out/stuck.png" >/dev/null 2>&1 || true
  echo "shutdown   the machine did not power off — the next boot is a ScanDisk"
  echo "           or safe mode. What was on screen: $OUT/out/stuck.png"
else
  echo "shutdown   clean"
fi

for f in $PROG_OUTPUTS; do
  rm -f "$OUT/out/$f"
  if mcopy -i "$RAW@@$OFF" -n "::/$f" "$OUT/out/$f" 2>/dev/null; then
    echo "post-shutdown $f:"
    case "$f" in
      *.BMP) echo "          (extracted bitmap: $OUT/out/$f)" ;;
      *)     sed 's/^/          /' "$OUT/out/$f" ;;
    esac
  fi
done
