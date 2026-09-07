#!/usr/bin/env bash
# Every size of the application icon, from the one master.
#
#   scripts/gen-icons.sh            # regenerate packaging/icon/ from the master
#   scripts/gen-icons.sh --check    # fail if anything is out of date (no writes)
#
# The master is `packaging/icon/2ksbox.png` — the artwork itself, a
# transparent RGBA render of the beige CRT the whole stack is pretending
# to be, on a 500x500 canvas it does not fill (the drawing is 431x436,
# with transparent margins around it). It is the only file to replace
# when the icon changes; everything below is derived from it and checked
# in, because the places that consume an icon cannot run ImageMagick when
# they need one:
#
#   * `launcher` embeds one with `include_bytes!` at compile time,
#   * the Flatpak build is offline and installs files, it does not draw them,
#   * the Windows package is cross-built in a container without ImageMagick,
#   * a user running `packaging/linux/install.sh` from a tarball has no
#     build tools at all.
#
# What each size is for (`hicolor` is the freedesktop icon theme's
# directory-per-size layout, which is what a Linux desktop looks in):
#
#   16, 24, 32, 48   task switchers, window decorations, small menus
#   64, 128          the applications menu, GNOME Software's lists
#   256, 512         software-centre banners, macOS, HiDPI everywhere
#   2ksbox.ico       Windows: 16/32/48/256 in one file, which is what a
#                    shortcut and an .exe resource both want
#
# Nothing is ever scaled *up*: the master is first padded with
# transparency to 512x512, centred, which costs no pixels because the
# margin is already transparent, and every size is then a downscale of
# that. Padding rather than resizing to 512 also keeps the drawing at its
# own native size in the largest icon, where it is looked at closest. If
# the artwork is ever redrawn larger, nothing here changes.
set -euo pipefail
cd "$(dirname "$0")/.."

DIR=packaging/icon
MASTER=$DIR/2ksbox.png
SIZES=(16 24 32 48 64 128 256 512)
ICO_SIZES=(16 32 48 256)

check=0
[ "${1:-}" = "--check" ] && check=1

command -v magick >/dev/null || { echo "gen-icons.sh: ImageMagick (magick) is required" >&2; exit 1; }
[ -f "$MASTER" ] || { echo "gen-icons.sh: no master at $MASTER" >&2; exit 1; }

# The square the sizes come from: the master centred on a 512x512
# transparent canvas. A master already 512x512 passes through unchanged,
# and one *larger* than that is scaled down to fit first, so this stays
# right whatever the artwork's canvas is.
pad=$(mktemp -d)/master-512.png
trap 'rm -rf "$(dirname "$pad")"' EXIT
magick "$MASTER" -background none -colorspace sRGB \
  -resize '512x512>' -gravity center -extent 512x512 -strip "PNG32:$pad"

# `-background none` keeps the alpha the master has (the icon is not a
# square: it has to sit on whatever colour a desktop puts behind it), and
# the explicit sRGB colorspace stops ImageMagick from linearising the
# downscale, which lightens a dark icon's edges.
render() { # size, out
  magick "$pad" -background none -colorspace sRGB \
    -resize "${1}x${1}" -strip "PNG32:$2"
}

out=$DIR
if [ "$check" = 1 ]; then
  out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
fi

for s in "${SIZES[@]}"; do render "$s" "$out/2ksbox-$s.png"; done
# One .ico holding the four sizes Windows actually picks from.
ico_inputs=(); for s in "${ICO_SIZES[@]}"; do ico_inputs+=("$out/2ksbox-$s.png"); done
magick "${ico_inputs[@]}" "$out/2ksbox.ico"

if [ "$check" = 1 ]; then
  rc=0
  for f in "$out"/*; do
    n=$(basename "$f")
    cmp -s "$f" "$DIR/$n" || { echo "gen-icons.sh: $DIR/$n is out of date"; rc=1; }
  done
  [ $rc = 0 ] && echo "gen-icons.sh: every size matches the master"
  exit $rc
fi

echo "gen-icons.sh: wrote $DIR/2ksbox-{$(IFS=,; echo "${SIZES[*]}")}.png and 2ksbox.ico"
