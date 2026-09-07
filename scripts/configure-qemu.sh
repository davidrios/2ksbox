#!/usr/bin/env bash
# Configure the prepared QEMU tree with a uv-managed Python, so the build
# never depends on whichever interpreter wins the host PATH race.
# Python version pinned in .python-version (QEMU 9.2.x supports <= 3.13;
# host 3.14s broke mkvenv/distlib).
#
# Usage: scripts/configure-qemu.sh [--windows] [extra configure flags...]
#
# --windows cross-compiles for Windows x86_64 with mingw-w64 into
# build/win/qemu instead of build/qemu, against the Rust staticlib built
# for x86_64-pc-windows-gnu. Run it inside the cross container
# (scripts/win-cross.sh), which is where the mingw glib/pixman/epoxy the
# build needs actually exist — docs/build-windows.md. The two build
# directories are independent, so one checkout holds a Linux build and a
# Windows build at once.
#
# QEMU_PYTHON=<interpreter> uses that one and never consults uv — for a
# build inside a sandbox that has a suitable Python already and cannot
# fetch one (the Flatpak, M6 step 6b: no uv in org.freedesktop.Sdk, and no
# network during the build). It is checked for version rather than
# trusted, because the failure it prevents (3.14 breaking mkvenv) is
# obscure at the point it bites.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

WINDOWS=""
if [ "${1:-}" = "--windows" ]; then WINDOWS=1; shift; fi

if [ -n "$WINDOWS" ]; then
  BUILD="$ROOT/build/win/qemu"
  CARGO_TARGET=x86_64-pc-windows-gnu
else
  BUILD="$ROOT/build/qemu"
  CARGO_TARGET=""
fi
LIBDISC_DIR="$ROOT/target${CARGO_TARGET:+/$CARGO_TARGET}/release"

PYVER="$(cat "$ROOT/.python-version")"
if [ -n "${QEMU_PYTHON:-}" ]; then
  PYTHON="$QEMU_PYTHON"
  command -v "$PYTHON" >/dev/null || { echo "QEMU_PYTHON=$PYTHON is not executable"; exit 1; }
  # QEMU 9.2.x supports 3.8 … 3.13; anything newer breaks mkvenv/distlib.
  "$PYTHON" -c 'import sys; sys.exit(0 if (3,8) <= sys.version_info[:2] <= (3,13) else 1)' || {
    echo "QEMU_PYTHON=$PYTHON is $("$PYTHON" -V 2>&1); QEMU 9.2.x needs 3.8–3.13"; exit 1; }
else
  command -v uv >/dev/null || {
    echo "uv not found — install it (https://docs.astral.sh/uv/), or set QEMU_PYTHON to a 3.8–3.13 interpreter"; exit 1; }
  uv python install "$PYVER" --quiet
  PYTHON="$(uv python find "$PYVER")"
fi
echo "==> python: $PYTHON ($("$PYTHON" -V 2>&1))"

# libdisc (the CD-ROM image model, libdisc/): a Rust staticlib linked into
# qemu-system-* and libqemu-embed-* for block/cdimage.c (patch 50). The crate
# has no QEMU dependency, so no cycle with the player.
echo "==> cargo build --release -p libdisc${CARGO_TARGET:+ --target $CARGO_TARGET}"
(cd "$ROOT" && cargo build --release -p libdisc ${CARGO_TARGET:+--target "$CARGO_TARGET"})

mkdir -p "$BUILD"
cd "$BUILD"
# --disable-werror: pinned 9.2.x trips new-toolchain warnings (glibc const strstr)
# --extra-cflags: vendored Khronos GL headers (third_party/khronos/README.md)
# -fPIC: objects are also linked into libqemu-embed-<target> (shared)
EXTRA_CFLAGS="-I$ROOT/third_party/khronos -fPIC"
CFG=(-Db_staticpic=true)
if [ -n "$WINDOWS" ]; then
  # PE code is position-independent by construction and gcc says so on every
  # file it compiles ("-fPIC ignored for target"), so the native build's flag
  # goes away here rather than being repeated a few thousand times.
  EXTRA_CFLAGS="-I$ROOT/third_party/khronos"
  CFG=(--cross-prefix=x86_64-w64-mingw32-)
  command -v x86_64-w64-mingw32-gcc >/dev/null || {
    echo "no x86_64-w64-mingw32-gcc — run this inside scripts/win-cross.sh"; exit 1; }
elif [ "$(uname -s)" = Darwin ]; then
  # qemu-3dfx's Darwin path is GLX via XQuartz (patched meson.build hardcodes
  # /opt/X11 into every emulator's link line, so the headers must be there
  # even though only libqemu-embed's own backend ever creates a context).
  [ -d /opt/X11/include ] || { echo "XQuartz missing: brew install --cask xquartz"; exit 1; }
  # SDK 15.4+ declares strchrnul (and friends) with an availability of 15.4;
  # QEMU detects and uses them unguarded, so a lower deployment target spams
  # -Wunguarded-availability-new. Target the running OS for local builds
  # (release packaging picks its own floor). Honour a preset value.
  if [ -z "${MACOSX_DEPLOYMENT_TARGET:-}" ]; then
    export MACOSX_DEPLOYMENT_TARGET="$(sw_vers -productVersion | cut -d. -f1,2)"
  fi
  echo "==> MACOSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET"
fi
# No QEMU user interface at all (2026-09-07). The player is the front end:
# it embeds QEMU, the embed library appends `-display none` itself
# (embed/libqemu_embed.c), and it brings its own 3D context provider
# (patch 30) and audio backend (patch 20). Every display QEMU can build was
# therefore dead code that each packager still had to carry — SDL2 (and,
# through sdl2-compat, SDL3) beside the player on Windows and macOS, GTK
# and its whole pango/cairo/gdk chain in libqemu-embed on Linux, spice's
# server, curses. Turning them off costs nothing we use and takes ~40
# libraries off the Linux embed library alone.
#
# VNC is deliberately kept. It needs no toolkit, and it is the only way
# left to *look at* a guest under a hand-run `qemu-system-i386` — which
# QEMU makes automatic: with no local display compiled in and no
# `-display` given, `qemu_setup_display()` starts a VNC server on
# localhost:5900 instead (system/vl.c). Anything scripted passes
# `-display none` and gets neither.
#
# The host audio backends go for the same reason: the player's audio is
# patch 20's `embed` audiodev, an SPSC ring the embedding application owns
# (docs/11), every machine the launcher writes says `audiodev=embed0`, and
# every headless tool says `audiodev=none`. ALSA, PulseAudio, PipeWire,
# JACK, OSS, sndio, CoreAudio and DirectSound were all compiled in and
# linked and none of them was ever opened. `none` and `wav` are built
# unconditionally (audio/meson.build) and `embed` is ours, so what the
# tree actually uses is untouched — `tools/xp-cdimage-test.sh` still
# captures CD-DA through `-audiodev wav`.
#
# Note this is *not* `--audio-drv-list=`: that list only picks the default
# priority order, while the libraries are pulled in by the per-driver
# feature options below being auto-detected.
#
# And the same for the rest of QEMU's optional surface that no machine the
# launcher writes can reach. Networking: every bundle says `-netdev user`
# and nothing else, so slirp stays and AF_XDP and vde go — and so does
# libbpf, whose one consumer is `hw/net/virtio-net.c`'s eBPF RSS steering
# and whose device the launcher never writes (pcnet on 98, rtl8139 on XP). Block: every drive is a local file — a qcow2, a raw floppy
# or one of doc 17's disc images through our own `cdimage` driver — so the
# network-storage drivers go, curl and libssh because this host has them
# and iscsi/nfs/rbd/gluster/blkio *pinned off* because another host might.
# Auto-detection is the thing to remove here: it makes the build depend on
# which libraries the machine happened to have, which is how the Flatpak
# and the Mac end up with a different libqemu-embed from this box's.
# brlapi is a braille chardev; nothing here has ever opened one.
"$ROOT/qemu/configure" \
  --python="$PYTHON" \
  --disable-werror \
  --disable-sdl \
  --disable-sdl-image \
  --disable-gtk \
  --disable-vte \
  --disable-cocoa \
  --disable-curses \
  --disable-spice \
  --disable-spice-protocol \
  --disable-alsa \
  --disable-pa \
  --disable-pipewire \
  --disable-jack \
  --disable-oss \
  --disable-sndio \
  --disable-coreaudio \
  --disable-dsound \
  --disable-brlapi \
  --disable-af-xdp \
  --disable-vde \
  --disable-bpf \
  --disable-curl \
  --disable-libssh \
  --disable-libiscsi \
  --disable-libnfs \
  --disable-rbd \
  --disable-glusterfs \
  --disable-blkio \
  --extra-cflags="$EXTRA_CFLAGS" \
  "${CFG[@]}" \
  --target-list=i386-softmmu,x86_64-softmmu \
  -Dlibdisc_dir="$LIBDISC_DIR" \
  "$@"

# QEMU's configure writes `werror = true` into its native file for git
# checkouts on Linux/Windows; meson re-applies native-file options on
# auto-regeneration, overriding the -Dwerror=false from --disable-werror and
# breaking the pinned 9.2.x build on new toolchains. Strip it.
sed -i.bak '/^werror = true$/d' "$BUILD/config-meson.cross" && rm -f "$BUILD/config-meson.cross.bak"
# keep the edited native file from looking newer than build.ninja (spurious regen)
touch -r "$BUILD/build.ninja" "$BUILD/config-meson.cross"
