#!/usr/bin/env bash
# Run the base98-br machine's player by hand with one Voodoo 2 property
# changed, for an A/B the launcher has no switch for (MSYS2's MINGW64
# shell, like scripts/win-run.sh, which this calls):
#
#   scripts/win-voodoo-ab.sh ramfifo=off     the command ring back to MMIO
#   scripts/win-voodoo-ab.sh recompiler=off  86Box's interpreter
#   scripts/win-voodoo-ab.sh threads=1       one render thread
#
# The machine is the launcher's own (its bundle's disk, its guest-tools
# ISO); close the launcher's window for that machine first, or the disk's
# write lock will refuse this one. The player started from a shell prints
# to that shell -- only the launcher redirects it -- so this tees the run
# into build/win-voodoo-ab.log, which is what to read afterwards
# (%APPDATA%\2ksbox\data\player.log has nothing from these runs).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROP="${1:-}"
[ -n "$PROP" ] || { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

DATA="$(cygpath "$APPDATA")/2ksbox/data"
M="$DATA/machines/base98-br"
[ -f "$M/disk.qcow2" ] || { echo "win-voodoo-ab.sh: no $M/disk.qcow2" >&2; exit 1; }

ISO="$(ls -t "$ROOT"/guest-tools/out/guest-tools-3dfx-*.iso 2>/dev/null | head -1)"
[ -n "$ISO" ] || { echo "win-voodoo-ab.sh: no guest-tools ISO in guest-tools/out" >&2; exit 1; }

win() { cygpath -w "$1"; }
TMP="$(cygpath "${TMP:-/tmp}")"

LOG="$ROOT/build/win-voodoo-ab.log"
mkdir -p "$ROOT/build"
{ echo; echo "=== voodoo2,$PROP — $(date) ==="; } >> "$LOG"
echo "win-voodoo-ab.sh: voodoo2,$PROP; log: $LOG"

set -o pipefail
"$ROOT/scripts/win-run.sh" player \
  --shader "$(win "$DATA/shaders/crt/crt-aperture.slangp")" \
  -- \
  -L "$(win "$ROOT/qemu/pc-bios")" \
  -machine pc,hpet=off -accel tcg -m 256 -cpu pentium3 \
  -drive "file=$(win "$M/disk.qcow2"),if=ide,index=0,media=disk" \
  -device "voodoo2,addr=0x05,$PROP" \
  -nic none -vga none -device d3dpt-vga,addr=0x02 \
  -device sb16,audiodev=embed0 \
  -device opl3,audiodev=embed0,sbbase=0x220 \
  -device mpu401,audiodev=embed0,synth=gm \
  -drive "if=none,id=cd0,media=cdrom,file=$(win "$ISO")" \
  -device "ide-cd,bus=ide.1,id=ide1-cd0,drive=cd0,audiodev=embed0" \
  -qmp "unix:$(win "$TMP/voodoo-ab.qmp"),server,nowait" 2>&1 | tee -a "$LOG"
