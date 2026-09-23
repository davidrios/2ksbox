#!/usr/bin/env bash
# SETUP's "WineD3D as this machine's DirectDraw" and the login helper
# behind it (doc 19 §43), in a real Win98 guest, headless.
#
# What it proves, and why it takes three boots.
#
# On 9x the WineD3D copy of DDRAW.DLL next to a game reaches that game only if
# it is the first program of the session to touch DirectDraw (doc 19 §42): the
# machine keeps one module per name and DDHELP.EXE holds Windows' own once
# anything has used it. So the check deliberately runs a DirectDraw program
# (DDPROBE.EXE, from a folder with none of Wine's files in it) *before* the
# probe, which is the case a per-game folder loses.
#
#   boot 1  SETUP /I <n> installs the switcher, DDSYS.DLL and D3DPRE.EXE and
#           adds the Run entry. Nothing is redirected yet: which way it should
#           point is the host's business, and the helper decides that at every
#           login.
#   boot 2  no-exec=on, a host with no Direct3D executor. The helper must
#           point DirectDraw at WineD3D, and the probe must then find a HAL
#           *after* DDPROBE has loaded DirectDraw from the system folder.
#   boot 3  the same image with the executor available. The helper must take
#           the redirection away again, and the probe must find our own HAL.
#
# The third boot is the half that matters most: a switch that is never
# switched back would leave every machine on WineD3D the first time it met a
# host without Vulkan.
#
#   tools/wined3d-sys-test.sh [image.qcow2]
#     env: COMP= (the /I number, 7), RAW=, OUT=, KEEP=1 (leave the raw copy)
#
# It runs in the player (the GL pass-through is what WineD3D draws through, so
# a bare qemu-system-i386 would have no 3D provider at all), on a raw copy,
# never the user's image. Local only (needs a Win98 image), not in
# scripts/test.sh.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:-$HOME/.local/share/2ksbox/machines/base98-br-glide3/disk.qcow2}"
OUT="${OUT:-$ROOT/build/wined3d-sys}"
RAW="${RAW:-$ROOT/build/wined3d-sys/guest.raw}"
COMP="${COMP:-7}"
D="$ROOT/guest-tools/out/driver9x"
WAIT="$ROOT/guest-tools/out/waitfile.exe"
GLP="$ROOT/guest-tools/out/glprobe.exe"
ISO="$(ls -t "$ROOT"/guest-tools/out/guest-tools-*.iso 2>/dev/null | head -1)"
export MTOOLS_SKIP_CHECK=1

[ -f "$IMG" ] || { echo "no image at $IMG"; exit 1; }
[ -n "$ISO" ] || { echo "no guest-tools ISO (guest-tools/build-wrappers.sh)"; exit 1; }
[ -f "$D/d3d7test.exe" ] || { echo "no $D/d3d7test.exe (guest-tools/build-driver9x.sh)"; exit 1; }
mkdir -p "$OUT"

echo "==> raw copy of $IMG"
"$ROOT/build/qemu/qemu-img" convert -O raw "$IMG" "$RAW" || exit 1
M="$RAW@@32256"
# the probe lives in a folder of its own with *none* of Wine's files in it:
# whatever it ends up using, it did not get from its own directory
mmd -i "$M" ::/WTEST 2>/dev/null || true
mcopy -i "$M" -o "$D/d3d7test.exe" ::/WTEST/D3D7TEST.EXE || exit 1

fails=0
verdict() {   # verdict <name> <ok?> <detail>
  if [ "$2" = 1 ]; then printf '  PASS %-34s %s\n' "$1" "${3:-}"
  else printf '  FAIL %-34s %s\n' "$1" "${3:-}"; fails=$((fails + 1)); fi
}
pull() {      # pull <guest path> <local name>
  rm -f "$OUT/$2"
  mcopy -i "$M" -n "::/$1" "$OUT/$2" 2>/dev/null && tr -d '\r' < "$OUT/$2" > "$OUT/$2.txt" \
    && mv "$OUT/$2.txt" "$OUT/$2"
}

# **OpenGL is not part of the switch.** The pass-through is the only
# accelerated GL on this machine whether or not WineD3D is in play, so this
# component installs it as the system OPENGL32.DLL and leaves it there. Both
# boots ask the same thing: "GDI Generic" here would mean Microsoft's software
# renderer answered, which is what WineD3D came up empty on.
gl_verdicts() {   # gl_verdicts <log> <when>
  local log="$OUT/$1"
  grep -q "GL_RENDERER" "$log" 2>/dev/null
  verdict "OpenGL answered ($2)" $((1 - $?)) "$(sed -n 's/.*GL_RENDERER *//p' "$log" 2>/dev/null | head -1 | cut -c1-48)"
  if grep -qi "GDI Generic" "$log" 2>/dev/null; then
    verdict "and it is the pass-through ($2)" 0 "Microsoft's software GL"
  else
    grep -q "GL_RENDERER" "$log" 2>/dev/null
    verdict "and it is the pass-through ($2)" $((1 - $?))
  fi
  grep -q "reads 00ff00" "$log" 2>/dev/null
  verdict "GL draws what it is told ($2)" $((1 - $?)) "$(sed -n 's/.*cleared pixel //p' "$log" 2>/dev/null | head -1)"
}

# ---------------------------------------------------------------- boot 1
echo "==> boot 1: SETUP /I $COMP"
PLAYER=1 RAW="$RAW" \
GUEST_CMD=$'D:\\SETUP.EXE /I '"$COMP"$'\ncopy C:\\2KSBOX\\SETUP.LOG C:\\SETUP1.LOG' \
EXTRA='-global d3dpt-vga.no-exec=on' TABLET=0 CDS="$ISO" \
RUN_SECS=90 SHOTS=0 SETTLE=30 \
OUT="$OUT/boot1" "$ROOT/tools/win98-game-test.sh" "$IMG" wined3dsys1 > "$OUT/boot1.log" 2>&1
pull SETUP1.LOG setup.log
grep -q "DDSYS.DLL written\|DDSYS.DLL is already there" "$OUT/setup.log" 2>/dev/null
verdict "setup made DDSYS.DLL" $((1 - $?)) "$(grep -m1 'DDSYS' "$OUT/setup.log" 2>/dev/null | sed 's/^ *//')"
for f in DDRAWME.DLL WINEDD.DLL WGLPT32.DLL DDSYS.DLL; do
  mdir -i "$M" "::/WINDOWS/SYSTEM/$f" >/dev/null 2>&1
  verdict "$f in the system folder" $((1 - $?))
done
mdir -i "$M" ::/WINDOWS/D3DPRE.EXE >/dev/null 2>&1
verdict "D3DPRE.EXE installed" $((1 - $?))

# ---------------------------------------------------------------- boot 2
mdel -i "$M" ::/WINDOWS/D3DPRE.LOG 2>/dev/null   # the wait below must see *this* login's
echo "==> boot 2: no executor on the host — the helper should switch DirectDraw over"
PLAYER=1 RAW="$RAW" STAGE="$D/ddprobe.exe $WAIT $GLP" \
GUEST_CMD=$'C:\\WAITFILE.EXE C:\\WINDOWS\\D3DPRE.LOG 90\ncd \\\nstart /w C:\\DDPROBE.EXE\ncd \\WTEST\nstart /w D3D7TEST.EXE 640 480 16 60\ncopy C:\\2KSBOX\\D3D7TEST.LOG C:\\D3D7W.LOG\ncd \\\nC:\\GLPROBE.EXE 120' \
EXTRA='-global d3dpt-vga.no-exec=on' TABLET=0 CDS="$ISO" \
RUN_SECS=200 SHOTS=0 SETTLE=30 \
OUT="$OUT/boot2" "$ROOT/tools/win98-game-test.sh" "$IMG" wined3dsys2 > "$OUT/boot2.log" 2>&1
pull WINDOWS/D3DPRE.LOG d3dpre-noexec.log
pull D3D7W.LOG probe-noexec.log
grep -q "should be WineD3D's" "$OUT/d3dpre-noexec.log" 2>/dev/null
verdict "helper chose WineD3D" $((1 - $?)) "$(head -1 "$OUT/d3dpre-noexec.log" 2>/dev/null | cut -c1-90)"
grep -q "DDRAW -> ddrawme.dll" "$OUT/d3dpre-noexec.log" 2>/dev/null
verdict "KnownDLLs\\DDRAW redirected" $((1 - $?))
grep -q "HAL device present" "$OUT/probe-noexec.log" 2>/dev/null
verdict "a Direct3D HAL after DDPROBE" $((1 - $?))
grep -qi "WineD3D" "$OUT/probe-noexec.log" 2>/dev/null
verdict "and it is WineD3D's" $((1 - $?)) "$(grep -m1 'device:' "$OUT/probe-noexec.log" 2>/dev/null | cut -c1-70)"
pull GLPROBE.LOG glprobe-noexec.log
gl_verdicts glprobe-noexec.log "no executor"
fps=$(sed -n 's/.*= \([0-9.]*\) fps/\1/p' "$OUT/probe-noexec.log" 2>/dev/null | head -1)
# Microsoft's software GL would be single digits here; ours is hundreds. This
# is what says the OPENGL32 redirection found the pass-through and not
# Windows' own opengl32.dll.
awk -v f="${fps:-0}" 'BEGIN { exit !(f > 50) }'
verdict "rendering through the pass-through" $((1 - $?)) "${fps:-no} fps"

# ---------------------------------------------------------------- boot 3
mdel -i "$M" ::/WINDOWS/D3DPRE.LOG 2>/dev/null
echo "==> boot 3: the executor is there — the helper should hand DirectDraw back"
PLAYER=1 RAW="$RAW" STAGE="$D/ddprobe.exe $WAIT $GLP" \
GUEST_CMD=$'C:\\WAITFILE.EXE C:\\WINDOWS\\D3DPRE.LOG 90\ncd \\\nstart /w C:\\DDPROBE.EXE\ncd \\WTEST\nstart /w D3D7TEST.EXE 640 480 16 60\ncopy C:\\2KSBOX\\D3D7TEST.LOG C:\\D3D7O.LOG\ncd \\\nC:\\GLPROBE.EXE 120' \
TABLET=0 CDS="$ISO" \
RUN_SECS=200 SHOTS=0 SETTLE=30 \
OUT="$OUT/boot3" "$ROOT/tools/win98-game-test.sh" "$IMG" wined3dsys3 > "$OUT/boot3.log" 2>&1
pull WINDOWS/D3DPRE.LOG d3dpre-exec.log
pull D3D7O.LOG probe-exec.log
pull GLPROBE.LOG glprobe-exec.log
grep -q "should be Windows' own" "$OUT/d3dpre-exec.log" 2>/dev/null
verdict "helper chose our own Direct3D" $((1 - $?)) "$(head -1 "$OUT/d3dpre-exec.log" 2>/dev/null | cut -c1-90)"
grep -q "DDRAW removed\|DDRAW -> " "$OUT/d3dpre-exec.log" 2>/dev/null
verdict "the redirection was taken away" $((1 - $?))
grep -q "HAL device present" "$OUT/probe-exec.log" 2>/dev/null
verdict "a Direct3D HAL with the executor" $((1 - $?))
if grep -qi "WineD3D" "$OUT/probe-exec.log" 2>/dev/null; then
  verdict "and it is ours, not WineD3D's" 0 "still on WineD3D"
else
  verdict "and it is ours, not WineD3D's" 1 "$(grep -m1 'device:' "$OUT/probe-exec.log" 2>/dev/null | cut -c1-70)"
fi
gl_verdicts glprobe-exec.log "executor on"

[ "${KEEP:-0}" = 1 ] || rm -f "$RAW"
echo
echo "$( [ $fails = 0 ] && echo PASS || echo FAIL ): $fails check(s) failed; logs in $OUT"
exit $((fails ? 1 : 0))
