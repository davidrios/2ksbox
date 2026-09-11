#!/usr/bin/env bash
# xp-driver-test.sh — drive an XP image through the d3dpt-vga driver tests
# headlessly (doc 15). Boots standalone QEMU (KVM when /dev/kvm exists)
# with the driver ISO and a FAT scratch disk, types commands over QMP, and
# pulls the guest logs out of the scratch disk with mtools.
#
#   tools/xp-driver-test.sh <image.qcow2> install      # DRVINST from the ISO, reboot, desktop on the driver
#   tools/xp-driver-test.sh <image.qcow2> ddtest       # DDTEST 640x480x8 (palette) / x16 / x32 / windowed, logs + BMP
#   tools/xp-driver-test.sh <image.qcow2> modes        # SETMODE 1024x768x32@85, 800x600x16@75, list
#   tools/xp-driver-test.sh <image.qcow2> d3d7         # D3D7TEST: the DX7 HAL scene, diffed against the host test's frame
#   tools/xp-driver-test.sh <image.qcow2> d3dgame8     # D3DGAME8 through XP's own d3d8.dll on the DX8 DDI (no wrapper DLL),
#                                                       # its frame diffed against the native d3d9 oracle of scripts/test.sh
#   tools/xp-driver-test.sh <image.qcow2> shtest       # SHTEST: vertex / pixel shaders 1.x through d3d8.dll on the DX8 DDI,
#                                                       # every draw read back in the guest; PASS = "0 failed" in shtest.log
#   tools/xp-driver-test.sh <image.qcow2> cktest       # CKTEST: palettized textures + colour keying through the DX7 HAL,
#                                                       # every draw read back in the guest; PASS = "0 failed" in cktest.log
#   tools/xp-driver-test.sh <image.qcow2> cubetest     # CUBETEST: cube textures through d3d8.dll on the DX8 DDI (protocol v11),
#                                                       # every draw read back in the guest; PASS = "0 failed" in cubetest.log
#   tools/xp-driver-test.sh <image.qcow2> probe VOLTEST  # one DX8 feature probe (d3d8probe.h: CUBETEST STRMTEST VOLTEST FMTTEST
#                                                       # BUMPTEST SPRTEST ANISTEST PATCHTST MSAATEST): PASS, NOT OFFERED (the caps say
#                                                       # the driver has no such feature) or FAIL, from the probe's last line
#   tools/xp-driver-test.sh <image.qcow2> probes       # all eight in one boot, a verdict each
#   tools/xp-driver-test.sh <image.qcow2> ebtest       # EBTEST: the DirectX 3 path (IDirect3D v1, execute buffers, texture
#                                                       # handles, viewport Clear) on the HAL; PASS = "0 failed" in ebtest.log
#   tools/xp-driver-test.sh <image.qcow2> cmd 'D:\DRIVER\SETMODE.EXE'   # any guest command line
#   tools/xp-driver-test.sh <image.qcow2> bat run.bat                   # a batch file, staged as E:\RUN.BAT (long command lines)
#
# Env: QEMU_EXTRA='-audiodev none,id=snd0 -device AC97,audiodev=snd0' (more QEMU arguments: a
# sound card), VGA=cirrus (XP's inbox driver instead of ours: the control for a crash), DDFLAGS=N (-device d3dpt-vga,ddflags=N), OUT=dir for screendumps
# and logs (default build/xp-driver-test), NO_KVM=1, CPU=pentium3 (the KVM CPU model), GAME_ISO=game.iso (the
# game disc takes the CD-ROM drive the game was installed from, D:; the
# driver ISO moves to the next drive, F: after the E: scratch), and for `cmd` / `bat`:
# CMD_WAIT=s (the cap on waiting for the command to say it is done over
# COM1, default 300; BOOT_WAIT=s is the cap on finding the shell at all,
# REBOOT_WAIT=s the cap on `install`'s restart — none of the three is a
# sleep any more, see tools/guestwait.sh) or SHOTS=n SHOT_EVERY=s (n screendumps
# cmd-01.png … every s seconds — for watching a game start; SHOT_KEYS="26:esc"
# presses a key right before screendump n). Needs
# guest-tools/build-driver.sh run first (guest-tools/out/d3dpt-driver.iso),
# mtools + python3, and mkfs.fat + sfdisk where they exist (the Mac has
# neither: mformat then builds the scratch disk). Ends every run with a clean
# power-down. Keys typed while a full-screen DirectDraw window is up are
# lost, so each test is ONE chained "cmd /k a & b & c" command line.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/guestwait.sh"
if [ "$(uname -s)" = Darwin ]; then
  # scripts/test.sh's macOS run environment: DXVK dlopens the Vulkan loader
  # by leaf name, and a DYLD_* variable handed to this script is stripped by
  # SIP at the #!/usr/bin/env exec. Without it the first Direct3D context
  # takes QEMU down (DXVK calls the loader it never found: SIGSEGV in
  # LibraryFn, "vkGetInstanceProcAddr not found" just before). The loader's
  # own keg only, never all of /opt/homebrew/lib (doc 00's ImageIO gotcha).
  VKLIB=/opt/homebrew/opt/vulkan-loader/lib
  [ -d "$VKLIB" ] || VKLIB=/opt/homebrew/lib
  export DYLD_LIBRARY_PATH="$VKLIB${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
  if [ -z "${VK_ICD_FILENAMES:-}" ]; then
    for f in "$HOME"/VulkanSDK/*/macOS/share/vulkan/icd.d/libkosmickrisp_icd.json; do
      [ -f "$f" ] && export VK_ICD_FILENAMES="$f"
    done
  fi
fi
IMG="${1:?image.qcow2}"; MODE="${2:?install|ddtest|modes|d3d7|d3dgame8|shtest|cktest|cubetest|probe|probes|ebtest|cmd|bat}"; shift 2
OUT="${OUT:-$ROOT/build/xp-driver-test}"; mkdir -p "$OUT"
ISO="$ROOT/guest-tools/out/d3dpt-driver.iso"
[ -f "$ISO" ] || { echo "no $ISO: run guest-tools/build-driver.sh"; exit 1; }
SCRATCH="$OUT/scratch.img"
if [ ! -f "$SCRATCH" ]; then
  dd if=/dev/null of="$SCRATCH" bs=1 seek=$((64 * 1048576)) 2>/dev/null
  if command -v sfdisk >/dev/null && command -v mkfs.fat >/dev/null; then
    printf 'label: dos\nstart=2048, type=0c\n' | sfdisk -q "$SCRATCH"
    mkfs.fat -F 32 --offset 2048 "$SCRATCH" >/dev/null
  else
    # the Mac has neither: the partition table by hand and mtools inside it,
    # tools/xp-cdimage-test.sh's recipe (-H 2048, the hidden-sectors field =
    # the partition's start, or XP does not mount the volume at all)
    python3 - "$SCRATCH" <<'MBR'
import struct, sys
start, total = 2048, 64 * 2048
mbr = bytearray(512)
mbr[0x1be:0x1be + 16] = struct.pack('<B3sB3sII', 0x00, b'\xfe\xff\xff', 0x0c, b'\xfe\xff\xff', start, total - start)
mbr[510:512] = b'\x55\xaa'
with open(sys.argv[1], 'r+b') as f:
    f.write(bytes(mbr))
MBR
    mformat -i "$SCRATCH@@1048576" -F -H 2048 -T $((64 * 2048 - 2048)) ::
  fi
fi
stage_bat() {  # the Run dialog truncates long lines: stage a batch file on the scratch disk (before the guest mounts it)
  sed 's/\r$//; s/$/\r/' "$1" > "$OUT/RUN.BAT"
  mcopy -o -i "$SCRATCH@@1048576" "$OUT/RUN.BAT" ::/RUN.BAT
}
[ "$MODE" = bat ] && stage_bat "${1:?batch file}"
if [ "$MODE" = ddtest ]; then
  printf '%s\n' '@echo off' 'cd /d E:\' 'for %%b in (8 16 32) do (' \
    '  D:\DRIVER\DDTEST.EXE 640 480 %%b 300' '  copy ddtest.log E:\dd%%b.log > nul' '  copy ddtest.bmp E:\dd%%b.bmp > nul' ')' \
    'D:\DRIVER\DDTEST.EXE 640 480 32 200 -windowed' 'copy ddtest.log E:\ddwin.log > nul' 'echo DDDONE > COM1' > "$OUT/ddtest.bat"
  stage_bat "$OUT/ddtest.bat"
fi
if [ "$MODE" = d3dgame8 ]; then
  # the reference scene's DX8 build from the guest-tools ISO (TESTS\), copied out alone so no
  # D3DPT\D3D8.DLL sits next to it: XP's own d3d8.dll, our DX8 DDI
  FULL_ISO="$(ls -t "$ROOT"/guest-tools/out/guest-tools-*.iso 2>/dev/null | head -1)"
  [ -f "$FULL_ISO" ] || { echo "no guest-tools ISO (TESTS\\D3DGAME8.EXE): run guest-tools/build-wrappers.sh"; exit 1; }
  ISO="$FULL_ISO"
  printf '%s\n' '@echo off' 'mkdir E:\G8' 'copy D:\TESTS\D3DGAME8.EXE E:\G8\ > nul' 'cd /d E:\G8' \
    'D3DGAME8.EXE -frames 600 -dump 300 E:\G8.BMP' 'copy d3dgame8.log E:\g8.log > nul' 'echo done > E:\G8DONE.TXT' 'echo G8DONE > COM1' > "$OUT/g8.bat"
  stage_bat "$OUT/g8.bat"
fi
PROBES="CUBETEST STRMTEST VOLTEST FMTTEST BUMPTEST SPRTEST ANISTEST PATCHTST MSAATEST"
if [ "$MODE" = probes ]; then
  # the DX8 feature probes one after the other; each writes <name>.log where it runs
  { printf '%s\n' '@echo off' 'cd /d %TEMP%'
    for p in $PROBES; do printf '%s\n' "D:\\DRIVER\\$p.EXE" "copy $p.LOG E:\\ > nul"; done
    printf '%s\n' 'echo PRDONE > COM1'; } > "$OUT/probes.bat"
  stage_bat "$OUT/probes.bat"
fi
SOCK="$OUT/qmp.sock"; rm -f "$SOCK"
ACCEL=(-cpu pentium3)
[ -e /dev/kvm ] && [ -z "${NO_KVM:-}" ] && ACCEL=(-accel kvm -cpu "${CPU:-host}")   # CPU=pentium3: Max Payne's JPEG decoder mis-decodes on a modern family
LOG="$OUT/qemu-$MODE.log"
# The guest's own line out. The scratch disk cannot be asked anything while
# the guest is running — XP's lazy writer can hold a small FAT write for
# minutes (tools/xp-cdimage-test.sh found that the hard way) — so every
# command this script types ends by echoing a marker to COM1, and the run
# waits for the marker instead of sleeping for as long as the command has
# ever taken.
SER="$OUT/serial-$MODE.log"; rm -f "$SER"
VGA_ARGS=(-vga none -device "d3dpt-vga,ddflags=${DDFLAGS:-0}")
[ -n "${VGA:-}" ] && VGA_ARGS=(-vga "$VGA")     # VGA=cirrus: the control run on XP's inbox driver (no Direct3D)
CD2=()
CD1="$ISO"
if [ -n "${GAME_ISO:-}" ]; then CD1="$GAME_ISO"; CD2=(-drive "file=$ISO,media=cdrom,if=ide,index=3,readonly=on"); fi
export D3DPT_EXEC_LIB="${D3DPT_EXEC_LIB:-$ROOT/build/d3dpt/libd3dpt_exec.so}"
export D3DPT_DXVK_LIB="${D3DPT_DXVK_LIB:-$ROOT/build/dxvk/src/d3d9/libdxvk_d3d9.so.0}"
"$ROOT/build/qemu/qemu-system-i386" -L "$ROOT/qemu/pc-bios" "${ACCEL[@]}" -machine pc -m 512 \
  -hda "$IMG" -hdb "$SCRATCH" -cdrom "$CD1" "${CD2[@]}" "${VGA_ARGS[@]}" \
  -net none -usb -device usb-tablet -display none -qmp "unix:$SOCK,server,nowait" \
  -serial "file:$SER" -monitor none ${QEMU_EXTRA:-} > "$LOG" 2>&1 &
QPID=$!
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }
run() {  # one chained guest command line in a console that stays open
  Q keys esc; sleep 1; Q keys meta_l+r; sleep 2
  Q type ' '; Q keys backspace          # the first key after the chord is lost, and Run opens on its last command
  Q type "cmd /k $1"; Q keys ret
}
run_until() {  # <marker> <cap> <command line> — run it, and wait for it to say it is done
  local mark=$1 cap=$2; shift 2
  run "$* & echo $mark > COM1"
  gw_wait_log "$SER" "$mark" "$cap" || true
}
pull() { mcopy -n -i "$SCRATCH@@1048576" "::/$1" "$OUT/$1" 2>/dev/null && echo "-- $1" && cat "$OUT/$1"; }
probe_verdict() {  # <NAME>: a DX8 feature probe's last line as PASS, NOT OFFERED or FAIL (d3d8probe.h); no log is a FAIL
  local l="$OUT/$(echo "$1" | tr A-Z a-z).log"
  rm -f "$l"
  mcopy -n -i "$SCRATCH@@1048576" "::/$(basename "$l")" "$l" 2>/dev/null || true
  if grep -q 'failed (not offered' "$l" 2>/dev/null; then
    echo "-- $1: NOT OFFERED ($(grep -o 'not offered: [^)]*' "$l" | tail -1 | cut -c14-))"
  elif grep -qE ': [1-9][0-9]* cases, 0 failed' "$l" 2>/dev/null; then
    echo "-- $1: PASS ($(grep -oE '[0-9]+ cases' "$l" | tail -1))"
  else
    echo "-- $1: FAIL (see $l and the device log)"
  fi
}
finish() {
  Q screendump "$OUT/$MODE-end.png" || true
  Q json '{"execute":"system_powerdown"}' >/dev/null || true
  gw_wait_exit "$QPID" 90 || true
  kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true
  echo "---- $LOG (device side)"; grep -v "^WARNING" "$LOG" | sed 's/qemu-system-i386: info: //' | tail -40
}

GW_PID=$QPID
gw_wait_sock "$SOCK" || exit 1
# No fixed boot sleep: knock on the Run dialog until the guest runs
# something and says so on COM1 (tools/guestwait.sh). `install` is the mode
# where the driver is not in the image yet, so there is no adapter line to
# wait for either — the knocking is what works on every mode.
gw_poke_until "$SOCK" xp 'cmd /c echo SHELLUP > COM1' "${BOOT_WAIT:-300}" grep -q SHELLUP "$SER" || {
  Q screendump "$OUT/$MODE-noshell.png" || true
  echo "the guest never reached its shell: see $OUT/$MODE-noshell.png and $LOG"
}
case "$MODE" in
  install)
    # a shorter cap than the rest: if DRVINST ever turns out not to return,
    # the restart below still works — cmd buffers the typed line until it does
    run_until DRVDONE "${CMD_WAIT:-180}" 'D:\DRIVER\DRVINST.EXE'
    Q screendump "$OUT/install-done.png"
    # the count to beat: the machine has to program the desktop mode once
    # more, after the restart, and that — not a screendump of a desktop
    # that looks the same either way — is the proof it came back on our
    # driver (nothing at all here on the first install: the count is 0)
    seen=$(grep -c "linear mode on" "$LOG" 2>/dev/null || true)
    want=$(( ${seen:-0} + 1 ))
    Q type 'shutdown -r -t 0'; Q keys ret
    gw_wait_count "$LOG" "linear mode on" "$want" "${REBOOT_WAIT:-300}" || true
    Q screendump "$OUT/install-rebooted.png"
    finish ;;
  ddtest)
    run 'E:\RUN.BAT'                                        # 8 / 16 / 32 bpp chains, then windowed (staged above)
    sleep 5; Q screendump "$OUT/ddtest-fullscreen.png"      # the 8 bpp chain: the palette shows in the dump
    gw_wait_log "$SER" DDDONE "${CMD_WAIT:-300}" || true
    finish
    pull dd8.log; pull dd16.log; pull dd32.log; pull ddwin.log
    for b in 8 16 32; do mcopy -n -i "$SCRATCH@@1048576" "::/dd$b.bmp" "$OUT/dd$b.bmp" 2>/dev/null || true; done ;;
  modes)
    run_until MODESDONE "${CMD_WAIT:-180}" 'D:\DRIVER\SETMODE.EXE 1024 768 32 85 & D:\DRIVER\SETMODE.EXE 800 600 16 75 & D:\DRIVER\SETMODE.EXE 1024 768 32 85 & D:\DRIVER\SETMODE.EXE > E:\modes.log'
    finish
    pull modes.log ;;
  d3d7)
    run 'D:\DRIVER\D3D7TEST.EXE 640 480 32 300 & copy d3d7test.log E:\d3d7.log & copy d3d7test.bmp E:\d3d7.bmp & echo D3D7DONE > COM1'
    sleep 8; Q screendump "$OUT/d3d7-fullscreen.png"
    gw_wait_log "$SER" D3D7DONE "${CMD_WAIT:-300}" || true
    finish
    pull d3d7.log
    mcopy -n -i "$SCRATCH@@1048576" ::/d3d7.bmp "$OUT/d3d7.bmp" 2>/dev/null || true
    # the same scene through the executor without a guest (tools/d3dpt-dp2-test.cpp): the frames must agree
    if [ -f "$OUT/d3d7.bmp" ] && [ -x "$ROOT/build/d3dpt-dp2-test" ]; then
      ( cd "$ROOT" && D3DPT_EXEC_LIB="${D3DPT_EXEC_LIB:-$ROOT/build/d3dpt/libd3dpt_exec.so}" build/d3dpt-dp2-test "$OUT/d3d7-host.bmp" >"$OUT/d3d7-host.log" 2>&1 ) || true
      python3 "$ROOT/tools/bmpdiff.py" "$OUT/d3d7-host.bmp" "$OUT/d3d7.bmp" --tolerance 2 --max-over 0 -o "$OUT/d3d7-diff.bmp" && echo "-- d3d7: guest frame == host frame" || echo "-- d3d7: FRAMES DIFFER ($OUT/d3d7-diff.bmp)"
    fi ;;
  d3dgame8)
    run 'E:\RUN.BAT'
    # COM1, not G8DONE.TXT on the scratch disk: the file is written the
    # moment the run ends but XP's lazy writer decides when the host sees it
    gw_wait_log "$SER" G8DONE "${CMD_WAIT:-300}" || true
    sleep 2; Q screendump "$OUT/d3dgame8-end.png" || true
    finish
    pull g8.log
    mcopy -n -i "$SCRATCH@@1048576" ::/G8.BMP "$OUT/G8.BMP" 2>/dev/null || true
    # the native d3d9 frame of the same scene (scripts/test.sh host writes it): the HUD masked, the rig budget
    if [ -f "$OUT/G8.BMP" ] && [ -f "$ROOT/build/test/g9-native.bmp" ]; then
      python3 "$ROOT/tools/bmpdiff.py" "$ROOT/build/test/g9-native.bmp" "$OUT/G8.BMP" --mask 0,368,270,112 --tolerance 8 --max-over 1200 -o "$OUT/g8-diff.bmp" \
        && echo "-- d3dgame8: frame within budget of the native d3d9 frame" || echo "-- d3dgame8: FRAME DIFFERS ($OUT/g8-diff.bmp)"
    else echo "-- d3dgame8: no frame ($OUT/G8.BMP) or no native oracle (build/test/g9-native.bmp: run scripts/test.sh host)"; fi ;;
  shtest)
    run 'cd /d %TEMP% & D:\DRIVER\SHTEST.EXE & copy shtest.log E:\ & echo SHDONE > COM1'
    sleep 8; Q screendump "$OUT/shtest-window.png" || true
    gw_wait_log "$SER" SHDONE "${CMD_WAIT:-300}" || true
    finish
    pull shtest.log
    if grep -q 'shtest: [1-9][0-9]* cases, 0 failed' "$OUT/shtest.log" 2>/dev/null; then echo "-- shtest: PASS"; else echo "-- shtest: FAIL (see $OUT/shtest.log and the device log)"; fi ;;
  cubetest|probe)
    p=CUBETEST; [ "$MODE" = probe ] && p="$(echo "${1:?probe name, e.g. VOLTEST}" | tr a-z A-Z)"
    lp="$(echo "$p" | tr A-Z a-z)"
    run_until PRDONE "${CMD_WAIT:-300}" "cd /d %TEMP% & D:\\DRIVER\\$p.EXE & copy $lp.log E:\\"
    finish
    pull "$lp.log" || true
    probe_verdict "$p" ;;
  probes)
    run 'E:\RUN.BAT'                                        # the eight probes (staged above)
    gw_wait_log "$SER" PRDONE "${CMD_WAIT:-900}" || true
    finish
    for p in $PROBES; do probe_verdict "$p"; done ;;
  cktest)
    run 'cd /d %TEMP% & D:\DRIVER\CKTEST.EXE & copy cktest.log E:\ & copy ck*.bmp E:\ & echo CKDONE > COM1'
    sleep 8; Q screendump "$OUT/cktest-fullscreen.png" || true
    gw_wait_log "$SER" CKDONE "${CMD_WAIT:-300}" || true
    finish
    pull cktest.log
    for n in 1 2 3 4 5 6; do mcopy -n -i "$SCRATCH@@1048576" "::/ck$n.bmp" "$OUT/ck$n.bmp" 2>/dev/null || true; done
    if grep -q 'cktest: [1-9][0-9]* cases, 0 failed' "$OUT/cktest.log" 2>/dev/null; then echo "-- cktest: PASS"; else echo "-- cktest: FAIL (see $OUT/cktest.log and the device log)"; fi ;;
  ebtest)
    run "cd /d %TEMP% & D:\\DRIVER\\EBTEST.EXE ${*:-} & copy ebtest.log E:\\ & copy eb*.bmp E:\\ & echo EBDONE > COM1"    # extra args: e.g. -rgb, the software-device control
    sleep 8; Q screendump "$OUT/ebtest-fullscreen.png" || true
    gw_wait_log "$SER" EBDONE "${CMD_WAIT:-300}" || true
    finish
    pull ebtest.log
    for n in 1 2 3 4 5 6; do mcopy -n -i "$SCRATCH@@1048576" "::/eb$n.bmp" "$OUT/eb$n.bmp" 2>/dev/null || true; done
    if grep -q 'ebtest: [1-9][0-9]* cases, 0 failed' "$OUT/ebtest.log" 2>/dev/null; then echo "-- ebtest: PASS"; else echo "-- ebtest: FAIL (see $OUT/ebtest.log and the device log)"; fi ;;
  cmd|bat)
    if [ "$MODE" = bat ]; then run 'E:\RUN.BAT'; else run "${1:?guest command line}"; fi
    if [ -n "${SHOTS:-}" ]; then
      for i in $(seq -f '%02g' 1 "$SHOTS"); do
        sleep "${SHOT_EVERY:-5}"
        # SHOT_KEYS="12:esc,14:ret": press the key (qmpc.py names) just before that screendump
        SK="${SHOT_KEYS:-}"; for k in ${SK//,/ }; do [ "${k%%:*}" = "${i#0}" ] && { Q keys "${k#*:}" || true; sleep 1; }; done
        Q screendump "$OUT/cmd-$i.png" || true
      done
    else
      sleep "${CMD_WAIT:-30}"
    fi
    finish ;;
  *) echo "unknown mode $MODE"; kill $QPID; exit 1 ;;
esac
