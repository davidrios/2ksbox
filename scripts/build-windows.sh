#!/usr/bin/env bash
# Build the Windows artefacts, in dependency order, from a Linux host --
# the counterpart of scripts/build.sh, which builds for the host it runs
# on. Everything happens inside the cross container
# (scripts/win-cross.sh, packaging/windows/Dockerfile) except the guest
# tools and the packaging step, which are host-side by nature.
#
# On Windows itself, in MSYS2's MINGW64 shell, the same stages build
# natively, for debugging on the PC: the same compilers, C runtime and
# Rust target as the cross image, so a build here is the build that
# ships. The guest-tools ISO and the package stay on Linux; copy an ISO
# into guest-tools/out/ and run what was built with scripts/win-run.sh.
#
#   scripts/build-windows.sh                everything this host can build
#   scripts/build-windows.sh qemu rust      only those stages
#   scripts/build-windows.sh --package      ... and then roll the zip (Linux)
#   scripts/build-windows.sh --msys2-deps   (Windows) install what the build needs
#
# Stages, in the order they must run:
#
#   qemu    configure-qemu.sh --windows -> ninja: qemu-system-i386.exe,
#           qemu-img.exe, qemu-io.exe, libqemu-embed-i386.dll, into
#           build/win/qemu (with libdisc built for windows-gnu first)
#   rust    cargo build --release --target x86_64-pc-windows-gnu: the
#           player, launcher-core, discx. After `qemu`, because the
#           player links the embed DLL out of build/win/qemu.
#   qt      cargo build in launcher-qt/ (its own workspace): the Qt 6 /
#           QML launcher, the one every package ships (ADR-015).
#           Cross-compiled like everything else — the image carries the
#           mingw Qt to link against and the native Qt of the same
#           version for moc/rcc/qmltyperegistrar.
#   exec    DXVK's d3d9.dll into build/win/dxvk (configure-dxvk.sh
#           --windows), then build-d3dpt-exec.sh --windows: d3dpt_exec.dll,
#           the Direct3D executor (doc 14). The package ships DXVK as
#           dxvk_d3d9.dll and the executor runs on nothing else — the same
#           d3d9 as every other host (2026-09-17).
#   guest   guest-tools/build-wrappers.sh: the guest-tools ISO. Host-side
#           and host-independent -- the ISO is 32-bit guest code, the same
#           file the Linux package ships -- so it is only built here when
#           there is not one already.
#
# docs/build-windows.md is the prose; docs/tracks/m11-windows-host.md is
# the track. Nothing here writes to build/qemu or target/release: a
# checkout holds a Linux build and a Windows build side by side.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Native on Windows (MSYS2's MINGW64 shell) or cross from Linux. The other
# MSYS2 shells are other C runtimes and C++ libraries than the package's
# msvcrt + libstdc++, so a build there would not be the one that ships.
NATIVE=""; HOW=cross
case "${MSYSTEM:-}" in
  "") ;;
  MINGW64) NATIVE=1; HOW=native ;;
  *) echo "build-windows.sh: this is MSYS2's $MSYSTEM shell; open the MINGW64 one" >&2; exit 1 ;;
esac

# Everything the native build needs from MSYS2, in one place: the cross
# image's list (packaging/windows/Dockerfile) under MSYS2's names, plus gdb,
# which is what building on the PC is for. Rust is not among them: it is
# rustup's own installer, with the GNU host (docs/build-windows.md).
MSYS2_PACKAGES=(git rsync
  mingw-w64-x86_64-{gcc,clang,lld,gdb,ninja,meson,pkgconf,python,python-distlib}
  mingw-w64-x86_64-{glib2,pixman,zlib,libepoxy,libslirp}
  mingw-w64-x86_64-{glslang,qt6-base,qt6-declarative})

JOBS=(); PACKAGE=""; STAGES=(); EXPLICIT=""
while [ $# -gt 0 ]; do
  case "$1" in
    -j) JOBS=(-j "$2"); shift 2 ;;
    -j*) JOBS=(-j "${1#-j}"); shift ;;
    -p|--package) PACKAGE=1; shift ;;
    --msys2-deps)
      [ -n "$NATIVE" ] || { echo "build-windows.sh: --msys2-deps is for MSYS2's MINGW64 shell on Windows" >&2; exit 2; }
      pacman -S --needed "${MSYS2_PACKAGES[@]}"
      exit ;;
    -h|--help) sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    qemu|rust|qt|exec|guest) STAGES+=("$1"); shift ;;
    *) echo "build-windows.sh: unknown argument '$1' (try --help)" >&2; exit 2 ;;
  esac
done
if [ ${#STAGES[@]} -eq 0 ]; then STAGES=(qemu rust qt exec guest); else EXPLICIT=1; fi

BUILT=(); SKIPPED=(); T0=$SECONDS
want() { local s; for s in "${STAGES[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }
say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
skip() { # stage, reason
  if [ -n "$EXPLICIT" ]; then echo "build-windows.sh: cannot build '$1': $2" >&2; exit 1; fi
  echo "    SKIP $1 - $2"; SKIPPED+=("$1 ($2)"); return 1
}
if [ -n "$NATIVE" ]; then
  inw() { "$@"; }
  # MSYS2's compilers carry no target prefix: the host is the target.
  WCC=gcc WCXX=g++
else
  inw() { scripts/win-cross.sh "$@"; }
  WCC=x86_64-w64-mingw32-gcc WCXX=x86_64-w64-mingw32-g++
fi

if [ -n "$NATIVE" ]; then
  [ -z "$PACKAGE" ] || { echo "build-windows.sh: --package runs on Linux (wine checks, the cross image's sysroot)" >&2; exit 2; }
  # A checkout with CRLF line endings fails far from here: every patch of
  # the queue "does not apply". Git for Windows converts by default.
  if [ -f qemu/configure ] && grep -q $'\r' qemu/configure; then
    echo "build-windows.sh: the checkout has CRLF line endings; clone again with core.autocrlf=false (docs/build-windows.md)" >&2
    exit 1
  fi
  missing=()
  for t in git rsync cygpath gcc g++ clang ld.lld ninja meson pkg-config windres glslangValidator cargo rustc; do
    command -v "$t" >/dev/null || missing+=("$t")
  done
  want qt && ! command -v qmake6 >/dev/null && missing+=(qmake6)
  if [ ${#missing[@]} -gt 0 ]; then
    echo "build-windows.sh: not found: ${missing[*]} -- run scripts/build-windows.sh --msys2-deps" >&2
    exit 1
  fi
  # rustup's default on Windows is the MSVC toolchain, whose build scripts
  # need Microsoft's link.exe: the host has to be the GNU one.
  host="$(rustc -vV | sed -n 's/^host: //p')"
  [ "$host" = x86_64-pc-windows-gnu ] || {
    echo "build-windows.sh: rustc's host is $host; run: rustup default stable-x86_64-pc-windows-gnu" >&2; exit 1; }
  export CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER="${CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER:-gcc}"
  export QMAKE="${QMAKE:-qmake6}"
fi

if [ ! -f qemu/VERSION ] || [ ! -f third_party/qemu-3dfx/00-qemu92x-mesa-glide.patch ]; then
  say "git submodule update --init (qemu, qemu-3dfx)"
  git submodule update --init --depth 1 qemu third_party/qemu-3dfx
fi

# The patch queue is applied to the one qemu/ tree both builds compile
# from, so it is prepared here exactly as scripts/build.sh does it -- and
# never twice in a row for nothing, which is what the stamp is for there.
# Here it is unconditional but cheap: a Windows build is not the inner
# loop, and a tree left half-prepared by an interrupted native build is
# the failure that costs an hour.
if want qemu; then
  say "qemu: prepare (overlay + patch queue)"
  scripts/prepare-qemu.sh

  # the compiler is part of a build directory: meson will not switch one
  # (patch 68 moved the Windows QEMU from GCC to clang, 2026-09-17)
  want_cc="${WIN_QEMU_CC:-clang}"
  if [ -f build/win/qemu/build.ninja ] && [ "$(cat build/win/qemu/.2ksbox-cc 2>/dev/null || echo gcc)" != "$want_cc" ]; then
    echo "    build/win/qemu was built with $(cat build/win/qemu/.2ksbox-cc 2>/dev/null || echo gcc), wanted $want_cc - configuring afresh"
    rm -rf build/win/qemu
  fi
  if [ ! -f build/win/qemu/build.ninja ]; then
    say "qemu: configure (mingw-w64 $HOW, $want_cc)"
    inw scripts/configure-qemu.sh --windows
    echo "$want_cc" > build/win/qemu/.2ksbox-cc
  else
    echo "    build/win/qemu is configured - skipping configure"
    # prepare re-applied the queue, so meson may need to regenerate; ninja
    # works that out itself from the mtimes it just saw change.
    :
  fi
  say "qemu: ninja"
  inw ninja -C build/win/qemu ${JOBS[@]+"${JOBS[@]}"} \
    qemu-system-i386.exe qemu-img.exe qemu-io.exe libqemu-embed-i386.dll
  BUILT+=(qemu)
fi

if want rust; then
  say "rust: cargo build --release --target x86_64-pc-windows-gnu"
  # Default members only (Cargo.toml): `launcher-capi` is left to the
  # native `scripts/build.sh`, which keeps it from rotting.
  inw cargo build --release --target x86_64-pc-windows-gnu ${JOBS[@]+"${JOBS[@]}"}
  BUILT+=(rust)
fi

# launcher-qt is its own cargo workspace (so a plain `cargo build` never
# needs Qt), which is why this is a separate stage with its own directory
# rather than another member of the one above.
if want qt; then
  if [ -z "$NATIVE" ] && ! scripts/win-cross.sh test -x /usr/bin/x86_64-w64-mingw32-qmake-qt6; then
    skip qt "the cross image has no mingw Qt 6 (rebuild it: scripts/win-cross.sh --build)" || true
  else
    say "qt: cargo build --release --target x86_64-pc-windows-gnu (launcher-qt)"
    inw sh -c 'cd launcher-qt && exec cargo build --release --target x86_64-pc-windows-gnu'
    BUILT+=(qt)
  fi
fi

if want exec; then
  # The queue is applied on the host, like qemu's above. The DXVK tree is
  # shared with the native build, so this keeps scripts/build.sh's own
  # stamp (same file, same hash): a prepare hands both builds fresh
  # mtimes, and one that changed nothing would cost the native DXVK a
  # full rebuild.
  say "exec: DXVK d3d9.dll (prepare + mingw $HOW)"
  dxvk_stamp=$( { git -C third_party/dxvk rev-parse HEAD 2>/dev/null || echo none
                  find patches/dxvk scripts/prepare-dxvk.sh -type f | LC_ALL=C sort | tr '\n' '\0' | xargs -0 cat
                } | sha256sum | cut -d' ' -f1)
  if [ "$(cat build/.stamp-dxvk-prepare 2>/dev/null || true)" != "$dxvk_stamp" ]; then
    scripts/prepare-dxvk.sh
    mkdir -p build && printf '%s\n' "$dxvk_stamp" > build/.stamp-dxvk-prepare
  else
    echo "    patch queue and submodule unchanged - skipping prepare"
  fi
  if [ ! -f build/win/dxvk/build.ninja ]; then
    inw scripts/configure-dxvk.sh --windows
  fi
  inw ninja -C build/win/dxvk ${JOBS[@]+"${JOBS[@]}"} src/d3d9/d3d9.dll
  say "exec: d3dpt_exec.dll (the Direct3D decoder + executor)"
  inw scripts/build-d3dpt-exec.sh --windows
  # ... and the display driver's host test, which package-windows.sh runs
  # under wine against the staged pair: a frame through the Windows DLLs.
  inw "$WCXX" -std=c++17 -O2 -static -o build/win/d3dpt-dp2-test.exe tools/d3dpt-dp2-test.cpp
  # The WGL probe rides along: it is the same 3D stage, it is one
  # compile, and it is the first thing to run on a Windows machine whose
  # Win98 guest gets no OpenGL (tools/wgl-probe.c).
  say "exec: wgl-probe.exe (the embed backend's WGL sequence, without QEMU)"
  inw "$WCC" -O1 -o build/win/wgl-probe.exe tools/wgl-probe.c \
    -lopengl32 -lgdi32 -luser32
  BUILT+=(exec)
fi

# The ISO is guest code and identical whatever host built it, so this
# stage exists to notice that there is none rather than to rebuild one.
if want guest; then
  if ls guest-tools/out/guest-tools-*.iso >/dev/null 2>&1; then
    say "guest"
    echo "    guest-tools ISO present - skipping (guest-tools/build-wrappers.sh rebuilds it)"
  elif [ -n "$NATIVE" ]; then
    skip guest "built on Linux: copy guest-tools/out/guest-tools-*.iso from there" || true
  elif ! command -v i686-w64-mingw32-gcc >/dev/null; then
    skip guest "needs mingw-w64 (i686-w64-mingw32-gcc)" || true
  elif ! command -v xorriso >/dev/null && ! command -v genisoimage >/dev/null; then
    skip guest "needs xorriso (or genisoimage) for the ISO" || true
  else
    say "guest: guest-tools ISO"
    guest-tools/build-wrappers.sh
    BUILT+=(guest)
  fi
fi

say "summary after $((SECONDS - T0)) s"
[ ${#BUILT[@]} -gt 0 ] && printf '    built: %s\n' "${BUILT[*]}"
if [ ${#SKIPPED[@]} -gt 0 ]; then printf '    skipped:\n'; printf '      %s\n' "${SKIPPED[@]}"; fi

if [ -n "$PACKAGE" ]; then
  say "scripts/package-windows.sh"
  exec scripts/package-windows.sh
fi
echo
if [ -n "$NATIVE" ]; then
  echo "    next: scripts/win-run.sh launcher   (or player / qemu; GDB=1 runs it under gdb)"
else
  echo "    next: scripts/package-windows.sh   (the zip, checked under wine)"
fi
