#!/usr/bin/env bash
# Install this package into a prefix, or just tell you that you don't have
# to. The extracted tree is already relocatable — `bin/2ksbox` finds the
# player, qemu-img, the firmware and the guest-tools ISO relative to its
# own location (doc 07's install layout) — so running it from wherever it
# was unpacked works. This script is for the rest: a desktop entry and an
# icon, so the launcher is in the applications menu.
#
#   ./install.sh                 # into ~/.local (no root)
#   ./install.sh --prefix /opt/2ksbox
#   sudo ./install.sh --prefix /usr/local
#   ./install.sh --uninstall     # removes what a previous run put there
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
prefix=${HOME}/.local
uninstall=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) prefix=$2; shift 2 ;;
    --prefix=*) prefix=${1#*=}; shift ;;
    --uninstall) uninstall=1; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "install.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
done

# The product name owns the directories and the commands; the application
# ID owns the desktop entry and the icon, because that pair is what a
# desktop matches window to launcher by (and what a Flatpak will require).
app=2ksbox
appid=com._2ksbox.Launcher
desktop_dir=$prefix/share/applications
# The icon arrives with the wholesale `share/` copy below, already under
# `share/icons/hicolor/<n>x<n>/apps/` where a desktop looks for it; this
# is only the size the desktop entry names outright, and the tree the
# uninstall has to clear (it is the one thing this installs outside
# `share/2ksbox`).
icon_dir=$prefix/share/icons/hicolor
icon_size=256
metainfo_dir=$prefix/share/metainfo

if [ "$uninstall" = 1 ]; then
  rm -rf "$prefix/lib/$app" "$prefix/libexec/$app" "$prefix/share/$app" "$prefix/share/doc/$app"
  rm -f "$prefix/bin/$app" "$prefix/bin/$app-player"
  rm -f "$desktop_dir/$appid.desktop" "$metainfo_dir/$appid.metainfo.xml"
  rm -f "$icon_dir"/*/apps/"$appid.png"
  command -v update-desktop-database >/dev/null && update-desktop-database "$desktop_dir" 2>/dev/null || true
  echo "removed $app from $prefix"
  exit 0
fi

if [ "$here" -ef "$prefix" ]; then
  echo "install.sh: source and destination are the same directory" >&2
  exit 1
fi

# The layout is copied wholesale: every path the launcher resolves is
# relative to bin/, so anything that arrives here as a set has to leave as
# one. `cp -a` rather than install(1) per file — pc-bios alone is hundreds
# of files, and none of them need a mode of their own.
for dir in bin lib libexec share; do
  [ -d "$here/$dir" ] || continue
  mkdir -p "$prefix/$dir"
  cp -a "$here/$dir/." "$prefix/$dir/"
done

# The desktop entry ships with a bare `Exec=2ksbox`, which is only right
# if the prefix's bin/ is on PATH. It is here that we know the absolute
# path, so write it in.
#
# `Icon=` gets the same treatment for the same reason: the bare
# `Icon=<app id>` only resolves when the prefix's `share/` is on
# XDG_DATA_DIRS, which a private prefix is not, so the entry names one
# size outright. The whole set is still installed — a desktop that *does*
# see the prefix picks the size it wants from it.
mkdir -p "$desktop_dir"
sed -e "s|^Exec=.*|Exec=$prefix/bin/$app|" \
    -e "s|^Icon=.*|Icon=$icon_dir/${icon_size}x${icon_size}/apps/$appid.png|" \
    "$here/share/$app/desktop/$appid.desktop" > "$desktop_dir/$appid.desktop"
# AppStream metadata, so a software centre knows what this is. Copied
# unmodified — nothing in it is path-dependent.
mkdir -p "$metainfo_dir"
cp -f "$here/share/$app/desktop/$appid.metainfo.xml" "$metainfo_dir/$appid.metainfo.xml"
command -v update-desktop-database >/dev/null && update-desktop-database "$desktop_dir" 2>/dev/null || true

echo "installed into $prefix"
echo "  launcher: $prefix/bin/$app"
echo "  player:   $prefix/bin/$app-player"

# Qt 6 is the one thing this package does not carry (ADR-015): every
# distribution has it, and a copy of our own would still have to match the
# host's Wayland, OpenGL and fontconfig. Say so here rather than let the
# launcher fail with a loader error nobody can act on. The QML modules are
# the other half and are invisible to ldd — a missing QtQuick.Controls is
# an error on stderr about a module not being installed, not a missing
# library — so the package names are given whole.
if command -v ldd >/dev/null && ldd "$prefix/bin/$app" 2>/dev/null | grep -q 'not found'; then
  echo
  echo "  Qt 6 is missing on this system, so the launcher will not start:"
  ldd "$prefix/bin/$app" | grep 'not found' | sed 's/^/    /'
  echo "  Install it from your distribution:"
  echo "    Arch          qt6-base qt6-declarative"
  echo "    Fedora        qt6-qtbase-gui qt6-qtdeclarative"
  echo "    Debian/Ubuntu libqt6quick6 qml6-module-qtquick-controls \\"
  echo "                  qml6-module-qtquick-dialogs qml6-module-qtquick-layouts"
fi
case ":$PATH:" in
  *":$prefix/bin:"*) ;;
  *) echo "  ($prefix/bin is not on your PATH; the desktop entry uses the absolute path)" ;;
esac
