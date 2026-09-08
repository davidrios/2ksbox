#!/usr/bin/env bash
# Build (and install) the Flatpak — M6 step 6b, manifest in
# packaging/flatpak/. Everything the build needs is compiled inside the
# SDK: the host's glibc is newer than the runtime's, so host-built
# binaries cannot run in it.
#
# **Not at the same time as `scripts/build-windows.sh`.** Both use the one
# `qemu/` tree: the Windows build re-applies the patch queue over it while
# flatpak-builder is copying it into the build sandbox, and the copy comes
# out a mix of patched and restored files. It fails deep in QEMU's compile
# with missing headers from a version of a file neither build is using
# (`hw/3dfx/glidept_mm.c: hw/core/sysbus.h: No such file`, 2026-09-07).
# Run one, then the other.
#
# The runtime is `org.kde.Platform` since ADR-015 (2026-09-07), because
# the launcher it packages is Qt 6 / QML and KDE's runtime is where Qt
# comes from. The first build after that change downloads a fresh ~3 GB
# runtime + SDK pair.
#
#   scripts/package-flatpak.sh              build, install --user, smoke check
#   scripts/package-flatpak.sh --no-install just build into the repo
#   scripts/package-flatpak.sh --check      only re-run the smoke check
#
# Environment:
#   FLATPAK_USER_DIR    which `--user` installation to use (flatpak's own
#                       variable). Set it if ~/.local/share/flatpak is on
#                       a full filesystem.
#   FLATPAK_BUILD_DIR   where flatpak-builder works, default build/flatpak.
#                       The build tree is several GB (a whole QEMU and a
#                       release Rust workspace), so point it at a disk
#                       with room.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

APPID=com._2ksbox.Launcher
MANIFEST="packaging/flatpak/$APPID.yml"
BUILD_DIR="${FLATPAK_BUILD_DIR:-$ROOT/build/flatpak}"
INSTALL=1 ONLY_CHECK=0
while [ $# -gt 0 ]; do
  case "$1" in
    --no-install) INSTALL=0; shift ;;
    --check) ONLY_CHECK=1; shift ;;
    -h|--help) sed -n '2,31p' "$0"; exit 0 ;;
    *) echo "package-flatpak.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
done

smoke() {
  # The app answering from inside its own sandbox: every companion has to
  # resolve under /app, which is the same property the tarball's check
  # makes, against a completely different prefix.
  echo "==> flatpak run $APPID --paths"
  local out
  out=$(flatpak run --user --command=2ksbox "$APPID" --paths) || return 1
  echo "$out"
  local fail=0
  while read -r what path; do
    case "$what" in player|qemu-img|pc-bios|guest-tools|prefix) ;; *) continue ;; esac
    case "$path" in "("*) continue ;; /app*) ;; *)
      echo "package-flatpak.sh: $what resolved outside /app: $path" >&2; fail=1 ;;
    esac
  done <<< "$out"
  # The three companions QEMU dlopens by name — the Glide wrapper, the
  # Direct3D executor, the DXVK it runs on. They are in no import table, so
  # nothing above would notice their absence; the packaged *player* is what
  # knows where they should be (`player/src/companions.rs`), and
  # `--companions` prints what that rule resolved. Inside the sandbox the
  # answer has to be under /app, and "(not shipped)" means the build made
  # one and did not stage it — or did not make it at all.
  echo "==> flatpak run $APPID --companions"
  local comp
  comp=$(flatpak run --user --command=2ksbox-player "$APPID" --companions) || return 1
  echo "$comp"
  while read -r what path; do
    case "$what" in glide|d3dpt-exec|dxvk) ;; *) continue ;; esac
    case "$path" in
      /app/*) ;;
      "(not"*) echo "package-flatpak.sh: the app ships no $what (its build step failed, or staged nothing)" >&2; fail=1 ;;
      *) echo "package-flatpak.sh: $what is $path, outside /app" >&2; fail=1 ;;
    esac
  done <<< "$comp"
  # The data directory is the one thing a Flatpak deliberately moves: it
  # lands under ~/.var/app/<app-id>, not ~/.local/share.
  case "$out" in *"/.var/app/$APPID/"*) ;; *)
    echo "package-flatpak.sh: the library is not under ~/.var/app/$APPID" >&2; fail=1 ;;
  esac
  # And the window, which `--paths` never reaches: the launcher is Qt 6 /
  # QML (ADR-015) and Qt resolves its platform plugin and every QtQuick
  # module by name at run time, out of the runtime rather than out of
  # /app. That is exactly the thing a wrong `runtime:` line would break
  # while every check above stayed green, so ask for a real window: the
  # launcher's own headless grab (doc 07), offscreen, and a PNG out of it.
  # Under $HOME, not /tmp: the sandbox has a /tmp of its own, so a grab
  # written there lands nowhere this shell can see it and the check fails
  # on a package that is perfectly fine (it did, first time). `$HOME` is
  # the same path on both sides, and this app has `--filesystem=host`.
  local shot="$HOME/.2ksbox-flatpak-window.png"
  rm -f "$shot"
  echo "==> flatpak run $APPID (offscreen window grab)"
  flatpak run --user --command=2ksbox \
    --env=QT_QPA_PLATFORM=offscreen --env=LAUNCHER_QT_SHOT="$shot" \
    --env=LAUNCHER_QT_DELAY=1500 "$APPID" >/dev/null 2>&1 || true
  if [ -s "$shot" ]; then
    echo "window         $(du -h "$shot" | cut -f1) grabbed offscreen: QML, plugins and all"
    rm -f "$shot"
  else
    echo "package-flatpak.sh: the app opened no window offscreen — Qt's QML modules or platform plugin are not in the runtime" >&2
    fail=1
  fi
  return $fail
}

if [ "$ONLY_CHECK" = 1 ]; then smoke; exit $?; fi

command -v flatpak-builder >/dev/null || { echo "flatpak-builder not installed" >&2; exit 1; }
# The patch queue runs on the host: it needs git and rsync, and the tree
# flatpak-builder copies has neither a .git nor rsync in the SDK.
[ -d qemu/hw/3dfx ] || { echo "qemu/ is not prepared: run scripts/prepare-qemu.sh first" >&2; exit 1; }
[ -f guest-tools/out/guest-tools-3dfx-*.iso ] 2>/dev/null || \
  ls guest-tools/out/guest-tools-*.iso >/dev/null 2>&1 || \
  echo "package-flatpak.sh: no guest-tools ISO built; the app will ship without it"

mkdir -p "$BUILD_DIR"
avail=$(df -Pk "$BUILD_DIR" | awk 'NR==2 {print int($4/1048576)}')
[ "$avail" -ge 12 ] || {
  echo "package-flatpak.sh: only ${avail} GB free at $BUILD_DIR; a QEMU + Rust build needs ~12 GB." >&2
  echo "Set FLATPAK_BUILD_DIR to somewhere with room." >&2; exit 1; }
echo "==> installation: ${FLATPAK_USER_DIR:-$HOME/.local/share/flatpak}"
echo "==> build dir:    $BUILD_DIR (${avail} GB free)"

args=(--user --force-clean --state-dir "$BUILD_DIR/state")
[ "$INSTALL" = 1 ] && args+=(--install)
flatpak-builder "${args[@]}" "$BUILD_DIR/build" "$MANIFEST"

[ "$INSTALL" = 1 ] || { echo "built (not installed): $BUILD_DIR/build"; exit 0; }
smoke
echo "installed: flatpak run $APPID"
