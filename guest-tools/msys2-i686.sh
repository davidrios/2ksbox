# Sourced by the guest-tools build scripts (build-wrappers.sh,
# build-driver.sh, build-driver9x.sh) after they set ROOT. On Linux and
# macOS it does nothing. In MSYS2 on Windows (docs/build-windows.md,
# "Building on Windows") it makes the shell the one those scripts and
# qemu-3dfx's own build expect:
#
# - MSYSTEM=MINGW32. qemu-3dfx's conf_wrapper builds the guest wrappers
#   natively only there (it asks `uname`, which MSYS2 derives from MSYSTEM,
#   and `gcc -v` for an i686 target), and its Makefiles refuse any other.
# - MSYS2's i686 toolchain (/mingw32/bin) first on PATH, so plain `gcc`,
#   `objdump` and `windres` are the guest's. The x86_64 tools behind it
#   still answer for what the guest build runs on the host: python3,
#   gendef.
# - The target-prefixed names the scripts call (i686-w64-mingw32-objdump,
#   -nm, -ar, -windres ...), which MSYS2's binutils does not install: shims
#   in guest-tools/tools/msys2-bin. gcc has its prefixed name already.
# - perl's core_perl on PATH, for qemu-3dfx's `shasum`.
#
# Idempotent: build-wrappers.sh runs the other two, which source it again.

case "${MSYSTEM:-}" in
  "") ;;
  MINGW64|MINGW32)
    if [ ! -x /mingw32/bin/i686-w64-mingw32-gcc.exe ]; then
      echo "no MSYS2 i686 toolchain (/mingw32/bin/i686-w64-mingw32-gcc): scripts/build-windows.sh --msys2-deps" >&2
      exit 1
    fi
    export MSYSTEM=MINGW32
    _msys2_shims="$ROOT/guest-tools/tools/msys2-bin"
    mkdir -p "$_msys2_shims"
    for _t in objdump nm ar ranlib windres dlltool strip; do
      printf '#!/bin/sh\nexec /mingw32/bin/%s "$@"\n' "$_t" > "$_msys2_shims/i686-w64-mingw32-$_t"
      chmod +x "$_msys2_shims/i686-w64-mingw32-$_t"
    done
    case ":$PATH:" in
      *":$_msys2_shims:"*) ;;
      *) export PATH="$_msys2_shims:/mingw32/bin:$PATH:/usr/bin/core_perl" ;;
    esac
    _msys2_missing=""
    for _t in make which xxd shasum nasm xorriso gendef python3; do
      command -v "$_t" >/dev/null || _msys2_missing="$_msys2_missing $_t"
    done
    if [ -n "$_msys2_missing" ]; then
      echo "not found:$_msys2_missing -- scripts/build-windows.sh --msys2-deps" >&2
      exit 1
    fi
    unset _t _msys2_missing
    ;;
  *)
    echo "MSYS2 $MSYSTEM shell: build from the MINGW64 one (docs/build-windows.md)" >&2
    exit 1
    ;;
esac
