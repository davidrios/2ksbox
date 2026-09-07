#!/usr/bin/env bash
# setup-guest-test.sh — SETUP.EXE from the guest-tools ISO, in a real
# Windows guest, headless. The installer decides what to do from the
# Windows it finds itself on, so the only honest test is to run it on both
# families and look at what ended up on the disk:
#
#   tools/setup-guest-test.sh ~/vms/winxp.qcow2  xp
#   tools/setup-guest-test.sh ~/vms/win98.qcow2  win98
#
# `SETUP /LIST` (the component list for this family), `SETUP /ALL` (install
# every one of them) and `SETUP /GAME 3 C:\2KSBOX` (a per-game file set),
# then Windows' own `dir` on each thing that should now exist — that, and
# the MAPMEM service on NT, are the proof, not SETUP's own exit code.
# Output comes back over COM1; PASS/FAIL per check at the end.
#
# The XP machine boots on the paravirtual adapter (-vga none -device
# d3dpt-vga) because its display-driver component binds to the device as it
# installs; Win98's stages three files for PnP to pick up on the next boot
# and so needs nothing (doc 19 §16), and stays on doc 06's cirrus machine
# — where SETUP must now offer that component, and land its three files.
#
# Needs a guest image, so it is run by hand and never from scripts/test.sh.
# The image is never written: everything goes to a qcow2 overlay under
# build/setup-test.
#
# Nothing here sleeps out a boot: the run waits for the guest to say it is
# there (tools/guestwait.sh) and BOOT_WAIT / WARMUP_WAIT are only the caps
# on that wait.
#
# Env: OUT=dir (default build/setup-test), BOOT_WAIT=s (cap, 300),
# WARMUP_WAIT=s (cap, 300), NO_WARMUP=1, NO_KVM=1, FORCE_KVM=1 (Win98 under
# KVM), KEEP=1.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/guestwait.sh"
IMG="${1:?image.qcow2}"; FAMILY="${2:-xp}"
OUT="${OUT:-$ROOT/build/setup-test}"; mkdir -p "$OUT"
OVL="$OUT/overlay-$FAMILY.qcow2"
FLOPPY="$OUT/run-$FAMILY.img"
LOG="$OUT/serial-$FAMILY.log"
QLOG="$OUT/qemu-$FAMILY.log"
# an AF_UNIX path is capped at 108 bytes, and a build/ path under a deep
# checkout is already close: keep the socket short and outside the tree
SOCK="${SOCK:-/tmp/2ks-setup-$FAMILY.sock}"
QEMU="$ROOT/build/qemu/qemu-system-i386"
ISO="$(ls -t "$ROOT"/guest-tools/out/guest-tools-3dfx-*.iso 2>/dev/null | head -1)"
[ -x "$QEMU" ] || { echo "no $QEMU: build QEMU first"; exit 1; }
[ -f "$ISO" ] || { echo "no guest-tools ISO: run guest-tools/build-wrappers.sh"; exit 1; }

# The CD's drive letter differs per image, and neither COMMAND.COM nor a
# fresh CMD.EXE can be relied on to set a variable inside a FOR. So every
# command is written once per candidate letter behind `if exist`, which
# both shells do the same thing with.
setup_line() {  # $1 = arguments to SETUP.EXE
  local d
  for d in D E F G; do printf 'if exist %s:\\SETUP.EXE %s:\\SETUP.EXE %s > COM1\n' "$d" "$d" "$1"; done
}
{
  echo '@echo off'
  echo 'echo ==== list > COM1'
  setup_line '/LIST'
  echo 'echo ==== install > COM1'
  setup_line '/ALL'
  echo 'echo ==== per-game set 3 (OpenGL) > COM1'
  setup_line '/GAME 3 C:\2KSBOX'
  echo 'echo ==== what is on the disk now > COM1'
  echo 'dir %windir%\CDSHELF.EXE > COM1'
  echo 'dir C:\2KSBOX\WGLGEARS.EXE > COM1'
  echo 'dir C:\2KSBOX\OPENGL32.DLL > COM1'
  if [ "$FAMILY" = win98 ]; then
    echo 'dir %windir%\SYSTEM\GLIDE2X.DLL > COM1'
    echo 'dir %windir%\SYSTEM\FXMEMMAP.VXD > COM1'
  else
    echo 'dir %windir%\system32\GLIDE2X.DLL > COM1'
    echo 'dir %windir%\system32\drivers\FXPTL.SYS > COM1'
    echo 'net start MAPMEM > COM1'
  fi
  echo 'echo SETUPDONE > COM1'
} > "$OUT/RUN.BAT"
sed -i 's/\r$//; s/$/\r/' "$OUT/RUN.BAT"
rm -f "$FLOPPY"
mkfs.fat -C -F 12 "$FLOPPY" 1440 >/dev/null
mcopy -o -i "$FLOPPY" "$OUT/RUN.BAT" ::/RUN.BAT

rm -f "$OVL"
"$ROOT/build/qemu/qemu-img" create -q -f qcow2 -b "$IMG" -F qcow2 "$OVL"
rm -f "$SOCK" "$LOG"

# Win98 re-detects its hardware on the first boot of a fresh overlay (the
# device set is not the one the image was last shut down with), and that
# boot regularly takes Explorer down with it — no taskbar, so no Start
# menu, so no Run dialog and no way in. So burn one boot first and shut it
# down over ACPI, which needs no shell; the second boot comes up settled.
warmup() {
  local pid
  "$QEMU" -L "$ROOT/qemu/pc-bios" "${ACCEL[@]}" -machine pc "${HW[@]}" \
    -hda "$OVL" -boot c -display none -qmp "unix:$SOCK,server,nowait" -monitor none \
    > "$OUT/warmup-$FAMILY.log" 2>&1 &
  pid=$!
  # The shell is the one thing this boot cannot be asked about — Explorer
  # dying is the very thing it exists to absorb — so it waits on the disks
  # instead: the re-detection is disk-heavy from end to end, and when the
  # reads stop it is over, one way or the other (tools/guestwait.sh).
  GW_PID=$pid
  gw_wait_sock "$SOCK" && gw_wait_quiet "$SOCK" "${WARMUP_WAIT:-300}" 10 || true
  python3 "$ROOT/tools/qmpc.py" "$SOCK" screendump "$OUT/$FAMILY-warmup.png" >/dev/null 2>&1 || true
  python3 "$ROOT/tools/qmpc.py" "$SOCK" json '{"execute":"system_powerdown"}' >/dev/null 2>&1 || true
  gw_wait_exit "$pid" 120 || true
  GW_PID=
  kill $pid 2>/dev/null || true
  wait $pid 2>/dev/null || true
  rm -f "$SOCK"
}

# Win98 is an emulated guest by decision (doc 06, and the launcher defaults
# its family to TCG): under KVM this image's Explorer dies at startup with
# "SHELL32.DLL is linked to missing export SHLWAPI.DLL:GetFileAttributesA"
# and there is then no Start menu to type into. XP uses KVM when there is a
# /dev/kvm. FORCE_KVM=1 runs Win98 under KVM anyway, to check that again.
ACCEL=(-cpu pentium3)
if [ "$FAMILY" != win98 ] || [ -n "${FORCE_KVM:-}" ]; then
  [ -e /dev/kvm ] && [ -z "${NO_KVM:-}" ] && ACCEL=(-accel kvm -cpu pentium3)
fi
if [ "$FAMILY" = win98 ]; then
  # doc 06's Win98 machine (the warm-up boot above absorbs the hardware
  # re-detection this device set triggers on a fresh overlay)
  HW=(-m 256 -vga cirrus -netdev user,id=n0 -device pcnet,netdev=n0
      -audiodev none,id=a0 -device sb16,audiodev=a0)
  SHELL_CMD='command /c A:\RUN.BAT'
else
  # the adapter the display-driver component installs a driver for
  HW=(-m 512 -vga none -device d3dpt-vga -net none)
  SHELL_CMD='cmd /k A:\RUN.BAT'
fi
if [ "$FAMILY" = win98 ] && [ -z "${NO_WARMUP:-}" ]; then warmup; fi

"$QEMU" -L "$ROOT/qemu/pc-bios" "${ACCEL[@]}" -machine pc "${HW[@]}" \
  -hda "$OVL" -fda "$FLOPPY" -boot c -cdrom "$ISO" \
  -usb -device usb-tablet -display none \
  -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" -monitor none > "$QLOG" 2>&1 &
QPID=$!
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }

GW_PID=$QPID
gw_wait_sock "$SOCK" || exit 1
# Typing into the Run dialog is the only way in and it can miss (the shell
# may still be starting, or a message box may be in front of it), so keep
# knocking until the guest's own output turns up on COM1 — that, and not a
# sleep, is what says the shell is there.
gw_poke_until "$SOCK" "$FAMILY" "$SHELL_CMD" "${BOOT_WAIT:-300}" test -s "$LOG" || {
  Q screendump "$OUT/$FAMILY-noshell.png" || true
  echo "the guest never ran anything: see $OUT/$FAMILY-noshell.png and $QLOG"
}
gw_wait_log "$LOG" SETUPDONE "${WAIT_SECS:-240}" || true
Q screendump "$OUT/$FAMILY-end.png" || true
if [ "$FAMILY" = win98 ]; then
  # a Win98 run ends with a Start-menu shutdown, never a kill (CLAUDE.md)
  Q keys ctrl+esc || true; sleep 2; Q keys u || true; sleep 2; Q keys ret || true
  gw_wait_exit "$QPID" 90 || true
else
  Q json '{"execute":"system_powerdown"}' >/dev/null || true
  gw_wait_exit "$QPID" 60 || true
fi
kill -0 $QPID 2>/dev/null && [ -z "${KEEP:-}" ] && kill $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true

echo "---- $LOG"
tr -d '\r' < "$LOG" || true
fails=0
want() {  # a line that must be in the output
  if grep -qF "$1" "$LOG"; then echo "PASS  $2"; else echo "FAIL  $2 (missing: $1)"; fails=$((fails + 1)); fi
}
never() {  # a line that must NOT be there
  if grep -qF "$1" "$LOG"; then echo "FAIL  $2 (present: $1)"; fails=$((fails + 1)); else echo "PASS  $2"; fi
}
echo "----"
want "SETUPDONE" "the batch ran to the end"
want "2ksbox guest tools" "SETUP started"
want "Installed." "the install finished without errors"
want "GLIDE2X.DLL ->" "SETUP copied the Glide wrappers"
want "CDSHELF.EXE ->" "SETUP copied the disc shelf tool"
want "WGLGEARS.EXE" "the test programs are in C:\\2KSBOX (Windows' own dir)"
want "OPENGL32.DLL" "the per-game set landed in C:\\2KSBOX (Windows' own dir)"
if [ "$FAMILY" = win98 ]; then
  want "Windows 98" "the family was detected"
  want "FXMEMMAP.VXD ->" "the 9x device mapper was installed"
  # The 9x display driver is three files dropped where PnP will find them —
  # there is no installer to run and nothing to bind to until the next boot,
  # which is why this checks the copies and not the adapter. That the driver
  # then comes up is tools/win98-driver-test.sh's job, on a machine that has
  # the device; this one is doc 06's Win98 reference machine and keeps its
  # cirrus.
  want "D3DPT9X.INF ->" "the 9x display driver's INF is in WINDOWS\\INF"
  want "D3DPT9X.DRV ->" "the 9x display driver was staged"
  want "D3DPT9V.VXD ->" "the 9x mini-VDD was staged"
else
  want "Windows XP" "the family was detected"
  want "drvinst: installed" "the display driver was installed"
  want "FXPTL.SYS ->" "the NT device mapper was installed"
  want "MAPMEM service: running" "the device-mapper service is running"
  grep -q "d3dptvid: adapter found" "$QLOG" && echo "PASS  the miniport came up (QEMU log)" \
    || { echo "FAIL  the miniport never logged (see $QLOG)"; fails=$((fails + 1)); }
fi
if [ "$fails" = 0 ]; then
  echo "setup guest test ($FAMILY): PASS"
else
  echo "setup guest test ($FAMILY): FAIL ($fails checks), see $OUT/$FAMILY-*.png and $QLOG"
  exit 1
fi
