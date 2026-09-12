#!/usr/bin/env bash
# Prepare the QEMU submodule tree: overlay qemu-3dfx device models, apply the
# version-matched patch, and stamp the qemu-3dfx commit id (guest wrappers
# verify it — build wrappers from the SAME third_party/qemu-3dfx commit).
#
# Idempotent. Reset with:  git -C qemu checkout . && git -C qemu clean -fd hw/3dfx hw/mesa
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# qemu-3dfx's sign_commit uses GNU `sed -i -e`; BSD sed would leave "-e"
# backup files behind. Prefer Homebrew gnu-sed on macOS.
if [ "$(uname -s)" = Darwin ]; then
  GNUBIN="$(brew --prefix gnu-sed 2>/dev/null || true)/libexec/gnubin"
  [ -d "$GNUBIN" ] && export PATH="$GNUBIN:$PATH" || echo "warning: gnu-sed not found (brew install gnu-sed)"
fi
QEMU="$ROOT/qemu"
FX="$ROOT/third_party/qemu-3dfx"
PATCH="$FX/00-qemu92x-mesa-glide.patch"

[ -f "$QEMU/VERSION" ] || { echo "qemu submodule missing (git submodule update --init)"; exit 1; }
[ -f "$PATCH" ] || { echo "qemu-3dfx submodule missing (git submodule update --init)"; exit 1; }

case "$(cat "$QEMU/VERSION")" in
  9.2.*) ;;
  *) echo "QEMU $(cat "$QEMU/VERSION") is not 9.2.x — patch/version mismatch"; exit 1 ;;
esac

# qemu-3dfx's sign_commit rewrites hw/{3dfx,mesa}/meson.build with sed -i on
# every run; a fresh mtime makes ninja regenerate the build. Snapshot the
# meson files and restore their mtimes when the content is unchanged.
SNAP="$(mktemp -d)"; trap 'rm -rf "$SNAP"' EXIT
for f in meson.build hw/3dfx/meson.build hw/mesa/meson.build; do
  [ -f "$QEMU/$f" ] && mkdir -p "$SNAP/$(dirname "$f")" && cp -p "$QEMU/$f" "$SNAP/$f"
done
restore_mtimes() {
  for f in meson.build hw/3dfx/meson.build hw/mesa/meson.build; do
    if [ -f "$SNAP/$f" ] && cmp -s "$SNAP/$f" "$QEMU/$f"; then
      touch -r "$SNAP/$f" "$QEMU/$f"
    fi
  done
}

echo "==> overlaying hw/3dfx and hw/mesa"
# -c: checksum compare so unchanged files (esp. meson.build) are not rewritten —
# a touched meson.build makes ninja regenerate and reset configure options.
rsync -rc "$FX/qemu-0/hw/3dfx" "$FX/qemu-1/hw/mesa" "$QEMU/hw/"

echo "==> overlaying embed/ (libqemu_embed)"
rsync -rc --delete "$ROOT/embed/" "$QEMU/embed/"

# glidept/glide_host.h is the one header the Glide device (hw/3dfx, patch
# 33), the embed provider and the host-side wrapper share -- like
# d3dpt_proto.h for Direct3D. Both consumers get a copy beside them.
rsync -c "$ROOT/glidept/glide_host.h" "$QEMU/embed/"
rsync -c "$ROOT/glidept/glide_host.h" "$QEMU/hw/3dfx/"

echo "==> overlaying d3dpt/ (paravirtual Direct3D device: hw/d3dpt)"
mkdir -p "$QEMU/hw/d3dpt"
rsync -rc --delete --exclude d3dpt_proto.h --exclude d3dpt_fb.h --exclude d3dpt_exec.h "$ROOT/d3dpt/hw/" "$QEMU/hw/d3dpt/"
rsync -c "$ROOT/d3dpt/d3dpt_proto.h" "$ROOT/d3dpt/d3dpt_fb.h" "$ROOT/d3dpt/exec/d3dpt_exec.h" "$QEMU/hw/d3dpt/"

echo "==> overlaying voodoo/ (the 3dfx Voodoo 2: hw/voodoo, doc 21)"
# 86Box's rasterizer (voodoo/86box, verbatim), the shim of 86Box's headers
# (voodoo/shim), the QEMU device and the directory's own meson.build.
mkdir -p "$QEMU/hw/voodoo"
rsync -rc --delete --exclude UPSTREAM "$ROOT/voodoo/" "$QEMU/hw/voodoo/"

echo "==> overlaying libsynth/ (the music devices: hw/audio/opl3.c + mpu401.c, doc 20)"
rsync -c "$ROOT/libsynth/qemu/opl3.c" "$ROOT/libsynth/qemu/mpu401.c" "$ROOT/libsynth/libsynth.h" "$QEMU/hw/audio/"

echo "==> overlaying libdisc/ (CD-ROM image block driver: block/cdimage.c, doc 17)"
rsync -c "$ROOT/libdisc/qemu/cdimage.c" "$QEMU/block/"
rsync -c "$ROOT/libdisc/qemu/cdimage.h" "$ROOT/libdisc/libdisc.h" "$QEMU/include/block/"

echo "==> overlaying gamepad/ (the two pad devices: hw/usb/dev-gamepad.c, hw/input/gameport.c, M13)"
rsync -c "$ROOT/gamepad/qemu/dev-gamepad.c" "$QEMU/hw/usb/"
rsync -c "$ROOT/gamepad/qemu/gameport.c" "$QEMU/hw/input/"
# The headers go where both their consumers can reach them: each device
# beside its own, and the embed shim, which is its own rsync'd tree and
# feeds either device from one call (qemu_embed_pad_state).
rsync -c "$ROOT/gamepad/qemu/usb-gamepad.h" "$QEMU/hw/usb/"
rsync -c "$ROOT/gamepad/qemu/gameport.h" "$QEMU/hw/input/"
rsync -c "$ROOT/gamepad/qemu/usb-gamepad.h" "$ROOT/gamepad/qemu/gameport.h" "$QEMU/embed/"

# Deterministic: restore every TRACKED file any patch touches to pristine
# v9.2.4, then apply the 3dfx patch and our queue fresh. (Overlay files were
# already refreshed by rsync above.) Partial states — e.g. a manual
# `git checkout meson.build` — previously slipped past an "already applied"
# heuristic and silently dropped hunks.
patched_files() {  # print paths from '+++ ./x' (diff -Nru) and '+++ b/x' (git) headers
  sed -n 's|^+++ \./||p; s|^+++ b/||p' "$@" | sort -u
}
echo "==> restoring tracked files touched by patches"
patched_files "$PATCH" "$ROOT"/patches/qemu/*.patch | while read -r f; do
  if git -C "$QEMU" ls-files --error-unmatch "$f" >/dev/null 2>&1; then
    git -C "$QEMU" checkout -q -- "$f"
  fi
done
# Files a patch CREATES ('--- /dev/null' header) must not pre-exist for
# git apply. Untracked overlay files (hw/3dfx, hw/mesa, embed) are also
# untracked but are refreshed by rsync above — never touch those here.
created_files() {
  awk '/^--- \/dev\/null/ { getline; sub(/^\+\+\+ (b\/|\.\/)/, ""); print }' "$@" | sort -u
}
created_files "$PATCH" "$ROOT"/patches/qemu/*.patch | while read -r f; do
  rm -f "$QEMU/$f"
done

echo "==> applying $(basename "$PATCH")"
git -C "$QEMU" apply "$PATCH"

echo "==> applying our patch queue (patches/qemu/*.patch)"
for p in "$ROOT"/patches/qemu/*.patch; do
  [ -e "$p" ] || continue
  if git -C "$QEMU" apply --check "$p" 2>"$SNAP/apply.err"; then
    git -C "$QEMU" apply "$p" && echo "    $(basename "$p"): applied"
  else
    echo "    $(basename "$p"): DOES NOT APPLY"; sed 's/^/      /' "$SNAP/apply.err"; exit 1
  fi
done

# SeaBIOS reports 06/23/99 as the legacy BIOS date (F000:FFF5 — the eight
# bytes ending three from the end of every one of its images), and that one
# string decides how Windows 98 installs. Setup's DetectACPIBIOS (sysdetmg.dll)
# compares it against ACPICheckDate, which machine.inf on the Win98 SE CD
# sets to "12/01/99": a BIOS at least that new is trusted to be ACPI, an
# older one is only believed if it matches BIOSINFO.INF's [GoodACPIBios] --
# four 1998 machines named by their ACPI OEM ids, none of them us. So a
# plain SETUP installs in PnP-BIOS mode, where the PCI bus is never
# enumerated: "Plug and Play BIOS" with a yellow ! in Device Manager, and
# no device added to the bus afterwards (the USB tablet, AC'97, the NIC) is
# ever detected. The install has to be `SETUP /p j` -- which is not
# something a launcher can type for a user -- or the date has to be past
# the cutoff, which is this. 12/31/99 rather than a 2000s date because the
# comparison is on a two-digit year. Nothing else reads this field: the
# per-machine quirks in BIOSINFO.INF that key on `date=` all want an exact
# 1994-1996 day, and the guests' own clocks come from the RTC.
echo "==> stamping the legacy BIOS date (Win98 installs ACPI only past 12/01/99)"
BIOS_DATE=12/31/99
for b in bios.bin bios-256k.bin bios-microvm.bin; do
  f="$QEMU/pc-bios/$b"
  [ -f "$f" ] || continue
  git -C "$QEMU" checkout -q -- "pc-bios/$b"   # deterministic: stamp a pristine blob
  off=$(( $(wc -c < "$f") - 11 ))
  cur=$(dd if="$f" bs=1 skip="$off" count=8 2>/dev/null)
  case "$cur" in
    [0-9][0-9]/[0-9][0-9]/[0-9][0-9]) ;;
    *) echo "    $b: no BIOS date at $off (\"$cur\") -- NOT stamped, is this SeaBIOS?" >&2; exit 1 ;;
  esac
  printf %s "$BIOS_DATE" | dd of="$f" bs=1 seek="$off" conv=notrunc 2>/dev/null
  echo "    $b: $cur -> $BIOS_DATE"
done

# SeaBIOS's VGA BIOS has no VBE 4F09h (Set/Get Palette Data) and reports
# every VESA mode as "not VGA compatible", which is what sends a program to
# 4F09h in the first place: DOS Quake quits with "Unable to load VESA
# palette" and Duke Nukem 3D draws its 640x480 in the default colours.
# firmware/ holds the same VGA BIOS built with patches/seabios/
# (scripts/build-vgabios.sh), checked in because it needs an x86 gcc. A
# whole blob replaced, so nothing to restore first.
echo "==> installing our VGA BIOSes (VBE 4F09h, patches/seabios)"
for f in "$ROOT"/firmware/vgabios-*.bin; do
  [ -e "$f" ] || continue
  cp "$f" "$QEMU/pc-bios/"
  echo "    $(basename "$f")"
done

echo "==> signing with qemu-3dfx commit"
(cd "$QEMU" && bash "$FX/scripts/sign_commit" -git="$FX")

restore_mtimes
echo "==> done. Configure with: scripts/configure-qemu.sh  (uv-managed python, --disable-werror)"
