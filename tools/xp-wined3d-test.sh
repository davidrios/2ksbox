#!/usr/bin/env bash
# xp-wined3d-test.sh — Direct3D 9 on WineD3D in an XP guest, in the player,
# on a host with no Vulkan at all: ADR-013's path, the one every host below
# the Vulkan 1.3 bar takes (a Mac before macOS 26, which has no KosmicKrisp,
# pre-Broadwell Intel, Kepler, TeraScale). The whole chain:
#
#   D3DGAME9.EXE -> WineD3D's D3D9.DLL + WINED3D.DLL next to it (SETUP /GAME 4)
#   -> qemu-3dfx's OPENGL32.DLL next to it (the same set) -> FXPTL.SYS, the
#   MAPMEM service (SETUP /I 2) -> hw/mesa -> the embed backend's context
#   -> the player
#
#   tools/xp-wined3d-test.sh ~/vms/winxp.qcow2
#
# **It must be the player**: a bare qemu-system-i386 registers no 3D provider
# and refuses the guest's GL context by design (patch 04), so WineD3D would
# have nothing to draw with.
#
# The evidence is the guest's own frame: D3DGAME9 -dump 300 reads frame 300
# back through WineD3D, which is glReadPixels through the pass-through, so a
# host that never drew the scene cannot make those pixels up. It is diffed
# against the rig golden under the guest stage's mask, tolerance and budget
# (scripts/test.sh, guest-G9~rig), and against the native DXVK frame when the
# host stage has made one, for information: WineD3D over the Mac's GL is not
# the same renderer and is not expected to match it byte for byte.
#
# The host is made Vulkan-less the way `test.sh`'s host-check does it (both
# loader variables at a file that does not exist), so nothing here can lean
# on KosmicKrisp. VULKAN=1 leaves the host's Vulkan alone.
#
# The image is never written (`snapshot=on`); results go to a scratch FAT
# disk (E:) that is read after XP has shut down, and COM1 carries the
# guest's progress and D3DGAME9's log while it runs. Local only (needs a
# guest image and a display), never from scripts/test.sh.
#
# Env: OUT=dir (build/wd3d), VGA=cirrus|d3dpt (cirrus: the guest stage's
# machine, which is what ~/vms/winxp.qcow2 has a driver for; d3dpt: the
# launcher's XP machine, whose executor then finds no Vulkan and must say so
# without taking the VM down), BOOT_WAIT=s (cap on the Run dialog, 300),
# RUN_WAIT=s (cap on the run, 900), FRAMES=n (600), DUMP=n (300).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/guestwait.sh"
IMG="${1:?image.qcow2}"
OUT="${OUT:-$ROOT/build/wd3d}"; mkdir -p "$OUT"
LOG="$OUT/serial.log"
QLOG="$OUT/player.log"
SOCK="${SOCK:-/tmp/2ks-wd3d.sock}"
PLAYER="$ROOT/target/release/player"
BIOS="$ROOT/qemu/pc-bios"
ISO="$(ls -t "$ROOT"/guest-tools/out/guest-tools-3dfx-*.iso 2>/dev/null | head -1)"
GOLDEN="$ROOT/reference/d3d/rig-2026-09-03/d3dgame9-w300-ff.bmp"
NATIVE="$ROOT/build/test/g9-native.bmp"
HUD_MASK="0,368,270,112"               # scripts/test.sh: the wall-time HUD
BUDGET="${D3D_GOLDEN_BUDGET:-1200}"
FRAMES="${FRAMES:-600}" DUMP="${DUMP:-300}"

[ -x "$PLAYER" ] || { echo "no $PLAYER (cargo build --release)"; exit 2; }
[ -f "$ISO" ] || { echo "no guest-tools ISO (guest-tools/build-wrappers.sh)"; exit 2; }
[ -f "$IMG" ] || { echo "no image $IMG"; exit 2; }

# The scratch disk: one FAT32 partition at 2048, as the other XP tools make
# it — GNU tools where there are some, mtools alone on a Mac
# (tools/xp-cdimage-test.sh has the why of each flag).
scratch="$OUT/scratch.img"; fat="$scratch@@1048576"
rm -f "$scratch"
dd if=/dev/null of="$scratch" bs=1 seek=$((64 * 1048576)) 2>/dev/null
if command -v sfdisk >/dev/null && command -v mkfs.fat >/dev/null; then
  printf 'label: dos\nstart=2048, type=c\n' | sfdisk -q "$scratch" >/dev/null
  mkfs.fat -F 32 --offset 2048 "$scratch" >/dev/null
else
  command -v mformat >/dev/null || { echo "needs mkfs.fat + sfdisk, or mtools"; exit 2; }
  python3 - "$scratch" <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    size = f.seek(0, 2) // 512
    entry = struct.pack("<B3sB3sII", 0, b"\0\0\0", 0x0c, b"\0\0\0", 2048, size - 2048)
    f.seek(446); f.write(entry); f.seek(510); f.write(b"\x55\xaa")
PY
  mformat -i "$fat" -F -H 2048 -T $((64 * 2048 - 2048)) ::
fi

# RUN.BAT: the device mapper, then the two per-game sets next to D3DGAME9,
# then the program. Every step says itself on COM1; the log goes there too,
# since the scratch disk is only read after shutdown.
printf '%s\r\n' '@echo off' \
  'echo started > COM1' \
  'mkdir E:\OUT' 'mkdir E:\WD3D' \
  'D:\SETUP.EXE /I 2 /LOG E:\OUT\SETUP-MAPPER.LOG > COM1' \
  'sc query MAPMEM > COM1' 'sc qc MAPMEM > COM1' \
  'sc query MAPMEM > nul' \
  'if errorlevel 1 D:\GLIDE\INSTDRV.EXE > COM1' \
  'if errorlevel 1 echo INSTDRV exit %ERRORLEVEL% > COM1' \
  'D:\SETUP.EXE /GAME 4 E:\WD3D /LOG E:\OUT\SETUP-WD3D.LOG > COM1' \
  'copy D:\TESTS\D3DGAME9.EXE E:\WD3D\ > COM1' \
  'cd /d E:\WD3D' \
  'dir /b E:\WD3D > COM1' \
  'echo ==== d3dgame9 > COM1' \
  "D3DGAME9.EXE -frames $FRAMES -dump $DUMP E:\\OUT\\G9.BMP" \
  'type d3dgame9.log > COM1' \
  'echo WD3DDONE > COM1' > "$OUT/RUN.BAT"
mcopy -o -i "$fat" "$OUT/RUN.BAT" ::/RUN.BAT

case "${VGA:-cirrus}" in
  cirrus) VGAARGS=(-vga cirrus) ;;
  d3dpt)  VGAARGS=(-vga none -device d3dpt-vga,addr=0x02) ;;
  *) echo "VGA=cirrus|d3dpt"; exit 2 ;;
esac
NOVK=()
[ -n "${VULKAN:-}" ] || NOVK=(VK_DRIVER_FILES=/nonexistent.json VK_ICD_FILENAMES=/nonexistent.json)

echo "==> XP in the player: $IMG (snapshot), ${VGA:-cirrus}, ${VULKAN:+host Vulkan}${VULKAN:-no Vulkan on the host}"
rm -f "$SOCK" "$LOG"
env ${NOVK[@]+"${NOVK[@]}"} "$PLAYER" -- -L "$BIOS" -machine pc -accel tcg -cpu pentium3 -m 512 \
  -drive "file=$IMG,if=ide,index=0,media=disk,snapshot=on" \
  -drive "file=$scratch,format=raw,if=ide,index=1,media=disk" -cdrom "$ISO" \
  "${VGAARGS[@]}" -nic none -usb -device usb-tablet \
  -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" \
  > "$QLOG" 2>&1 &
QPID=$!
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }
GW_PID=$QPID
gw_wait_sock "$SOCK" || { echo "the player never opened its QMP socket: $QLOG"; tail -5 "$QLOG"; exit 1; }
gw_poke_until "$SOCK" xp 'E:\RUN.BAT' "${BOOT_WAIT:-300}" test -s "$LOG" || {
  Q screendump "$OUT/noshell.png" || true
  echo "the guest never ran RUN.BAT: see $OUT/noshell.png and $QLOG"
}
t0=$SECONDS
gw_wait_log "$LOG" WD3DDONE "${RUN_WAIT:-900}" || true
echo "==> the run took $((SECONDS - t0)) s after RUN.BAT started"
Q screendump "$OUT/end.png" || true
Q json '{"execute":"system_powerdown"}' >/dev/null 2>&1 || true
gw_wait_exit "$QPID" 180 || true
kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true
GW_PID=; rm -f "$SOCK"

rm -f "$OUT/G9.BMP"
mcopy -n -i "$fat" ::/OUT/G9.BMP "$OUT/G9.BMP" 2>/dev/null || true
for l in SETUP-MAPPER SETUP-GL SETUP-WD3D; do mcopy -n -i "$fat" "::/OUT/$l.LOG" "$OUT/$l.LOG" 2>/dev/null || true; done
mcopy -n -i "$fat" ::/WD3D/d3dgame9.log "$OUT/d3dgame9.log" 2>/dev/null || true

echo
echo "==== the guest's serial output ===================================="
sed 's/\r$//' "$LOG" 2>/dev/null || echo "(nothing)"
echo "==================================================================="
echo "player log: $QLOG   ($(grep -ac '^mesapt: ' "$QLOG" 2>/dev/null || true) mesapt, $(grep -ac '^glcntx: ' "$QLOG" 2>/dev/null || true) glcntx lines)"
grep -a -m 6 -e '^mesapt: ' -e '^glcntx: ' -e 'd3dpt' "$QLOG" 2>/dev/null | cut -c1-160 || true

fail=0
grep -qa "MAPMEM service: running" "$LOG" || { echo "FAIL: the device mapper is not running (SETUP /I 2)"; fail=1; }
grep -qa "frame $DUMP -> .*(written)" "$LOG" || { echo "FAIL: D3DGAME9 did not write frame $DUMP"; fail=1; }
if [ -f "$OUT/G9.BMP" ]; then
  if python3 "$ROOT/tools/bmpdiff.py" "$GOLDEN" "$OUT/G9.BMP" -o "$OUT/g9-vs-rig.bmp" --mask "$HUD_MASK" \
       --tolerance 8 --max-over "$BUDGET" > "$OUT/rig.txt" 2>&1; then
    echo "PASS vs the rig golden: $(head -2 "$OUT/rig.txt" | tr '\n' ' ')"
  else
    echo "FAIL vs the rig golden: $(head -2 "$OUT/rig.txt" | tr '\n' ' ')"; fail=1
  fi
  if [ -f "$NATIVE" ]; then
    python3 "$ROOT/tools/bmpdiff.py" "$NATIVE" "$OUT/G9.BMP" --mask "$HUD_MASK" > "$OUT/native.txt" 2>&1 || true
    echo "vs native DXVK (information): $(head -1 "$OUT/native.txt")"
  fi
else
  echo "FAIL: no G9.BMP on the scratch disk"; fail=1
fi
[ "$fail" = 0 ] && echo "PASS xp-wined3d" || echo "FAIL xp-wined3d — $OUT"
exit $fail
