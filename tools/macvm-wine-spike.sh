#!/usr/bin/env bash
# The M15 spike inside a macOS guest (docs/tracks/m15-wine-executor.md,
# ADR-019): a pre-26 macOS in a UTM virtual machine on this Mac stands in
# for the community build's user — no Vulkan 1.3, so the executor has to
# run on Wine — and this drives the two host tests through the Windows
# build of the executor on Wine's own d3d9 *in that guest*, over ssh, and
# diffs the frames it drew against the DXVK frames this host made.
#
#   tools/macvm-wine-spike.sh <user>@<guest ip>        # utmctl ip-address <vm> gives the ip
#
# It expects, on this host, what the spike already made:
#   build/wine/wine-staging-*-osx64.tar.xz   WineHQ's macOS build (x86_64)
#   build/wine-spike/d3dpt_exec.dll          scripts/build-d3dpt-exec.sh --windows
#   build/wine-spike/d3dpt-dp2-test.exe      the display driver's host test, mingw build
#   build/wine-spike/d3dpt-exec-test.exe     the DLL path's host test, mingw build
#   build/wine-spike/libwinpthread-1.dll     Homebrew's mingw links it dynamically
#   build/test/dp2-test.bmp, build/wine-spike/final-native.bmp   the DXVK frames (the oracle)
# and, in the guest: an account with Remote Login on and Rosetta installed
# (`softwareupdate --install-rosetta --agree-to-license`, once, by hand —
# it wants an administrator). Everything else it copies in and runs.
#
# Two questions only the guest can answer, and both come out in the log:
# whether a Rosetta process on Apple's paravirtual GPU gets an OpenGL that
# wined3d can use (the `GL_RENDERER` line), and how fast the tests run
# there against this host (the `fps` line of the exec test).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
target="${1:?usage: $0 user@guest-ip}"
OUT="${OUT:-build/macvm/spike}"; mkdir -p "$OUT"
tar=$(ls build/wine/wine-staging-*-osx64.tar.xz | head -1)
for f in build/wine-spike/d3dpt_exec.dll build/wine-spike/d3dpt-dp2-test.exe \
         build/wine-spike/d3dpt-exec-test.exe build/wine-spike/libwinpthread-1.dll \
         build/test/dp2-test.bmp build/wine-spike/final-native.bmp; do
  [ -f "$f" ] || { echo "missing $f (run the spike on the host first: the track doc)"; exit 1; }
done
ssh_() { ssh -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new "$target" "$@"; }

echo "==> guest"
ssh_ 'sw_vers; uname -m; arch -x86_64 /usr/bin/true && echo "rosetta: yes" || echo "rosetta: NO -- softwareupdate --install-rosetta --agree-to-license in the guest first"' | tee "$OUT/guest.txt"
grep -q "rosetta: yes" "$OUT/guest.txt" || exit 1

echo "==> copying Wine ($(du -h "$tar" | cut -f1)) and the spike"
ssh_ 'mkdir -p ~/2ksbox-spike'
scp -q "$tar" build/wine-spike/d3dpt_exec.dll build/wine-spike/d3dpt-dp2-test.exe \
    build/wine-spike/d3dpt-exec-test.exe build/wine-spike/libwinpthread-1.dll "$target:~/2ksbox-spike/"
ssh_ 'cd ~/2ksbox-spike && [ -d "Wine Staging.app" ] || tar xJf wine-staging-*-osx64.tar.xz'

echo "==> running (the first start makes the prefix: ~30 s)"
ssh_ 'cd ~/2ksbox-spike
W="$HOME/2ksbox-spike/Wine Staging.app/Contents/Resources/wine/bin/wine"
export WINEPREFIX="$HOME/2ksbox-spike/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="d3d9=b;mscoree,mshtml="
"$W" wineboot -i >/dev/null 2>&1 || true
"$W" reg add "HKCU\\Software\\Wine\\Direct3D" /v renderer /d gl /f >/dev/null 2>&1
"$W" --version
D3DPT_EXEC_LIB=d3dpt_exec.dll D3DPT_D3D9=system WINEDEBUG=-all,+d3d "$W" d3dpt-dp2-test.exe dp2.bmp > dp2.log 2>&1; echo "dp2 exit=$?"
D3DPT_EXEC_LIB=d3dpt_exec.dll D3DPT_D3D9=system "$W" d3dpt-exec-test.exe exec.bmp 120 60 > exec.log 2>&1; echo "exec exit=$?"
grep -o "GL_RENDERER \"[^\"]*\"" dp2.log | head -1
grep "Using the .* renderer" dp2.log | head -1
grep "PASSED\|FAILED" dp2.log | tail -1
grep "frames," exec.log' | tee "$OUT/run.txt"

echo "==> frames"
scp -q "$target:~/2ksbox-spike/dp2.bmp" "$OUT/dp2.bmp"
scp -q "$target:~/2ksbox-spike/exec.bmp" "$OUT/exec.bmp"
scp -q "$target:~/2ksbox-spike/dp2.log" "$OUT/dp2.log"
scp -q "$target:~/2ksbox-spike/exec.log" "$OUT/exec.log"
rc=0
python3 tools/bmpdiff.py build/test/dp2-test.bmp "$OUT/dp2.bmp" --tolerance 8 -o "$OUT/dp2-diff.bmp" || rc=1
python3 tools/bmpdiff.py build/wine-spike/final-native.bmp "$OUT/exec.bmp" --tolerance 8 -o "$OUT/exec-diff.bmp" || rc=1
[ $rc = 0 ] && echo "PASS: both frames match the DXVK frames" || echo "FAIL: a frame differs (diff images in $OUT)"
exit $rc
