#!/usr/bin/env bash
# Configure the prepared QEMU tree with a uv-managed Python, so the build
# never depends on whichever interpreter wins the host PATH race.
# Python version pinned in .python-version (QEMU 9.2.x supports <= 3.13,
# and 3.14 only with a real distlib: see QEMU_PYTHON below).
#
# Usage: scripts/configure-qemu.sh [--windows] [extra configure flags...]
#
# --windows cross-compiles for Windows x86_64 with mingw-w64 into
# build/win/qemu instead of build/qemu, against the Rust staticlib built
# for x86_64-pc-windows-gnu. Run it inside the cross container
# (scripts/win-cross.sh), which has the mingw glib/pixman/epoxy the build
# needs (docs/build-windows.md). The two build
# directories are independent, so one checkout holds a Linux build and a
# Windows build at once.
#
# On Windows itself, in MSYS2's MINGW64 shell, the same build is native
# (docs/build-windows.md, "Building on Windows"): --windows is implied, no
# cross prefix, and MSYS2's own Python rather than uv's. A python.org
# interpreter makes a venv with `Scripts\` where QEMU's configure looks for
# `bin/`.
#
# QEMU_PYTHON=<interpreter> uses that one and never consults uv. It is for
# a sandbox that has a suitable Python and cannot fetch one (the Flatpak:
# no uv in the SDK, no network during the build). Its version is checked,
# because the failure is obscure where it bites. QEMU 9.2's mkvenv
# supports 3.8-3.13, and 3.14 works only with the real `distlib`
# installed, since pip >= 26 trimmed the vendored copy mkvenv falls back
# to (3.14 with distlib configures and builds; MSYS2 has no older Python).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

WINDOWS=""; NATIVE=""
if [ "${1:-}" = "--windows" ]; then WINDOWS=1; shift; fi
case "${MSYSTEM:-}" in
  "") ;;
  MINGW64) WINDOWS=1; NATIVE=1 ;;
  # UCRT64 and CLANG64 are other C runtimes and C++ libraries than the
  # cross image's msvcrt + libstdc++, so a debugging build there would not
  # be the build that ships.
  *) echo "MSYS2 $MSYSTEM shell: build from the MINGW64 one (docs/build-windows.md)"; exit 1 ;;
esac

if [ -n "$WINDOWS" ]; then
  BUILD="${WIN_QEMU_BUILD:-$ROOT/build/win/qemu}"
  CARGO_TARGET=x86_64-pc-windows-gnu
else
  BUILD="$ROOT/build/qemu"
  CARGO_TARGET=""
fi
# Paths that end up inside meson's files are read by native Windows
# programs, which cannot resolve MSYS2's /c/... form: C:/... there.
MROOT="$ROOT"
[ -n "$NATIVE" ] && MROOT="$(cygpath -m "$ROOT")"
LIBDISC_DIR="$MROOT/target${CARGO_TARGET:+/$CARGO_TARGET}/release"
# libsynth's staticlib lands in the same directory. It has its own variable
# because the two meson options are separate and either can point
# elsewhere.
LIBSYNTH_DIR="$LIBDISC_DIR"

PYVER="$(cat "$ROOT/.python-version")"
check_python() {  # the variable that named it, for the message
  command -v "$PYTHON" >/dev/null || { echo "$1=$PYTHON is not executable"; exit 1; }
  "$PYTHON" -c '
import sys
v = sys.version_info[:2]
if v == (3, 14):
    import distlib.scripts, distlib.version
elif not (3, 8) <= v <= (3, 13):
    sys.exit(1)
' 2>/dev/null || {
    echo "$1=$PYTHON is $("$PYTHON" -V 2>&1); QEMU 9.2.x needs 3.8–3.13, or 3.14 with the distlib package"
    exit 1; }
}
if [ -n "${QEMU_PYTHON:-}" ]; then
  PYTHON="$QEMU_PYTHON"
  check_python QEMU_PYTHON
elif [ -n "$NATIVE" ]; then
  PYTHON=/mingw64/bin/python3
  [ -x "$PYTHON.exe" ] || [ -x "$PYTHON" ] || {
    echo "no $PYTHON: pacman -S mingw-w64-x86_64-python mingw-w64-x86_64-python-distlib"; exit 1; }
  check_python "MSYS2's python"
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

# libsynth (the music engines, libsynth/): the same arrangement for
# hw/audio/opl3.c and hw/audio/mpu401.c (patch 60, doc 20). Also no QEMU
# dependency, so no cycle with the player.
echo "==> cargo build --release -p libsynth${CARGO_TARGET:+ --target $CARGO_TARGET}"
(cd "$ROOT" && cargo build --release -p libsynth ${CARGO_TARGET:+--target "$CARGO_TARGET"})

mkdir -p "$BUILD"
cd "$BUILD"
# --disable-werror: pinned 9.2.x trips new-toolchain warnings (glibc const strstr)
# --extra-cflags: vendored Khronos GL headers (third_party/khronos/README.md)
# -fPIC: objects are also linked into libqemu-embed-<target> (shared)
EXTRA_CFLAGS="-I$ROOT/third_party/khronos -fPIC"
CFG=(-Db_staticpic=true)
if [ -n "$NATIVE" ]; then
  # On Windows, in MSYS2's MINGW64 shell, this is the cross build below
  # without the cross. Same compiler (clang against GCC's mingw runtime), same linker
  # (lld, named through meson's CC_LD rather than a wrapper script, which a
  # native meson cannot execute), same flags.
  EXTRA_CFLAGS="-I$MROOT/third_party/khronos"
  # And the same libraries: MSYS2 with Qt installed has several the cross
  # image lacks (zstd and friends arrive as Qt's dependencies), and QEMU
  # links whatever it detects. Each of these said NO in the cross build's
  # configure summary, so they are pinned to it here.
  CFG=(--disable-zstd --disable-gnutls --disable-nettle --disable-gcrypt --disable-capstone
       --disable-libusb --disable-usb-redir --disable-lzo --disable-snappy --disable-smartcard
       --disable-libcbor --disable-lzfse)
  if [ "${WIN_QEMU_CC:-clang}" = clang ]; then
    command -v clang >/dev/null && command -v ld.lld >/dev/null || {
      echo "no clang/lld: pacman -S mingw-w64-x86_64-clang mingw-w64-x86_64-lld"; exit 1; }
    export CC_LD=lld CXX_LD=lld
    CFG+=(--cc=clang --cxx=clang++ --disable-plugins)
  fi
elif [ -n "$WINDOWS" ]; then
  # PE code is position-independent by construction and gcc says so on every
  # file it compiles ("-fPIC ignored for target"), so the native build's flag
  # goes away here rather than being repeated a few thousand times.
  EXTRA_CFLAGS="-I$ROOT/third_party/khronos"
  CFG=(--cross-prefix=x86_64-w64-mingw32-)
  command -v x86_64-w64-mingw32-gcc >/dev/null || {
    echo "no x86_64-w64-mingw32-gcc — run this inside scripts/win-cross.sh"; exit 1; }
  # clang, not GCC (patch 68). mingw GCC 15 has only emulated TLS, a call
  # on every __thread access, and QEMU makes several on every device
  # access. One VGA register read took 121.6 ns against clang's 64.3
  # (Linux: 52.7). The cross prefix still names the binutils and the
  # mingw sysroot. WIN_QEMU_CC=gcc builds the old way. No TCG plugins:
  # lld has no --dynamic-list, and nothing here loads a plugin.
  if [ "${WIN_QEMU_CC:-clang}" = clang ]; then
    command -v clang >/dev/null && command -v ld.lld >/dev/null || {
      echo "no clang/lld in the cross image — scripts/win-cross.sh --build"; exit 1; }
    CFG+=(--cc="$ROOT/packaging/windows/clang-mingw-cc" --cxx="$ROOT/packaging/windows/clang-mingw-cxx"
          --host-cc=gcc --disable-plugins)
  fi
elif [ "$(uname -s)" = Darwin ]; then
  # Every Mac build targets Homebrew's floor, the oldest macOS the app's
  # Homebrew libraries exist for (scripts/macos-floor.sh; build.sh exports
  # the same value, and a preset one wins). It goes in as a flag as well as
  # the environment. A changed flag changes every command line, so a
  # reconfigure recompiles the tree, where a changed environment alone
  # would keep the objects built for the old target.
  # -Werror=unguarded-availability-new goes with it, because an API newer
  # than the target used without an @available check makes a binary that
  # dies on the floor's macOS. Since the flag reaches meson's own checks, a
  # function detected through its real declaration is only found when the
  # target has it (patch 46: strchrnul, 15.4).
  if [ -z "${MACOSX_DEPLOYMENT_TARGET:-}" ]; then
    export MACOSX_DEPLOYMENT_TARGET="$("$ROOT/scripts/macos-floor.sh")"
  fi
  EXTRA_CFLAGS="$EXTRA_CFLAGS -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -Werror=unguarded-availability-new"
  echo "==> MACOSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET"
fi
# No QEMU user interface at all. The player is the front end. It embeds
# QEMU, the embed library appends `-display none` itself
# (embed/libqemu_embed.c), and it brings its own 3D context provider
# (patch 30) and audio backend (patch 20). Every display QEMU can build was
# dead code each packager still had to carry: SDL2 (and, through
# sdl2-compat, SDL3) beside the player on Windows and macOS, GTK and its
# pango/cairo/gdk chain in libqemu-embed on Linux, spice's server, curses.
# Turning them off costs nothing we use and takes ~40 libraries off the
# Linux embed library alone.
#
# VNC stays. It needs no toolkit, and it is the only way left to *look at*
# a guest under a hand-run `qemu-system-i386`. With no local display
# compiled in and no `-display` given, `qemu_setup_display()` starts a VNC
# server on localhost:5900 (system/vl.c). Anything scripted passes
# `-display none` and gets neither.
#
# The host audio backends go for the same reason. The player's audio is
# patch 20's `embed` audiodev, an SPSC ring the embedding application owns
# (docs/11); every machine the launcher writes says `audiodev=embed0`, and
# every headless tool says `audiodev=none`. ALSA, PulseAudio, PipeWire,
# JACK, OSS, sndio, CoreAudio and DirectSound were compiled in and linked,
# and none was ever opened. `none` and `wav` are built unconditionally
# (audio/meson.build) and `embed` is ours, so what the tree uses is
# untouched. `tools/xp-cdimage-test.sh` still captures CD-DA through
# `-audiodev wav`.
#
# This is *not* `--audio-drv-list=`. That list only sets the default
# priority order; the per-driver feature options below, auto-detected,
# are what pull the libraries in.
#
# The same goes for the rest of QEMU's optional features that no machine
# the launcher writes can reach. Networking: every bundle says `-netdev
# user` and nothing else, so slirp stays and AF_XDP and vde go. So does
# libbpf, whose one consumer is `hw/net/virtio-net.c`'s eBPF RSS steering,
# a device the launcher never writes (pcnet on 98, rtl8139 on XP). Block:
# every drive is a local file (a qcow2, a raw floppy or one of doc 17's
# disc images through our own `cdimage` driver), so the network-storage
# drivers go. curl and libssh go because this host has them, and
# iscsi/nfs/rbd/gluster/blkio are *pinned off* because another host might.
# Auto-detection makes the build depend on which libraries the machine
# happened to have, which is how the Flatpak and the Mac would end up with
# a different libqemu-embed from this box's. brlapi is a braille chardev
# nothing here opens.
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
  ${CFG[@]+"${CFG[@]}"} \
  --target-list=i386-softmmu,x86_64-softmmu \
  -Dlibdisc_dir="$LIBDISC_DIR" \
  -Dlibsynth_dir="$LIBSYNTH_DIR" \
  "$@"

# QEMU's configure writes `werror = true` into its native file for git
# checkouts on Linux/Windows; meson re-applies native-file options on
# auto-regeneration, overriding the -Dwerror=false from --disable-werror and
# breaking the pinned 9.2.x build on new toolchains. Strip it.
sed -i.bak '/^werror = true$/d' "$BUILD/config-meson.cross" && rm -f "$BUILD/config-meson.cross.bak"
# keep the edited native file from looking newer than build.ninja (spurious regen)
touch -r "$BUILD/build.ninja" "$BUILD/config-meson.cross"
