#!/usr/bin/env bash
# The Linux package (M6 step 6, doc 07's install layout): stage everything
# a stranger needs into one relocatable tree, check that the staged
# launcher really resolves its companions *inside* it, and roll a tarball.
#
#   scripts/package-linux.sh                 # build, stage, check, tar
#   scripts/package-linux.sh --no-build      # use target/release as it is
#   scripts/package-linux.sh --no-tar        # leave the staged tree only
#   scripts/package-linux.sh --with-shaders  # include the preset collection
#   scripts/package-linux.sh --out DIR       # default build/package
#   scripts/package-linux.sh --prefix DIR    # stage straight into DIR
#
# `--prefix` stages the layout directly into an existing prefix instead of
# a versioned subdirectory, and rolls no tarball: it is how the Flatpak
# (packaging/flatpak/) fills `/app`, since an install prefix and the
# staged tree are the same shape. It never deletes the destination.
#
# It does not build QEMU: build/qemu (libqemu-embed-i386.so + qemu-img)
# and qemu/pc-bios must already be there, per CLAUDE.md's build order.
# The guest-tools ISO is included when guest-tools/out has one.
#
# The launcher is `launcher-qt` (ADR-015, 2026-09-07): Qt 6 / QML is the
# front end the project ships, and `launcher/` (egui) stays a maintained
# second view over `launcher-core` that no package installs. Qt itself is
# **not** in the tarball -- it is ~38 MB of shared libraries, QML modules
# and plugins that every distribution already packages, and a tarball
# that carried its own would still have to match the host's Wayland,
# OpenGL and fontconfig stacks. So this package depends on the system's
# `qt6-base` + `qt6-declarative` (+ `qt6-quickcontrols2`), which the check
# below states plainly by listing what the staged launcher resolves. The
# Flatpak is the build for a host that has none: it gets Qt from
# `org.kde.Platform` (packaging/flatpak/).
#
# The layout, relative to the tree's root (= an install prefix):
#   bin/2ksbox                        the launcher (Qt 6, ADR-015)
#   bin/2ksbox-player                 the player
#   lib/2ksbox/libqemu-embed-i386.so
#   lib/2ksbox/libglide2x.so          the Glide wrapper, when one is built
#   lib/2ksbox/libd3dpt_exec.so       the Direct3D executor, and the DXVK
#   lib/2ksbox/libdxvk_d3d9.so.0        it runs on — both or neither
#   libexec/2ksbox/qemu-img           ours, patched — kept off PATH
#   share/2ksbox/pc-bios/             QEMU firmware (the player's -L)
#   share/2ksbox/guest-tools/         the guest-tools ISO
#   share/2ksbox/shaders/             presets, with --with-shaders
#   share/2ksbox/desktop/             .desktop + AppStream, for install.sh
#   share/icons/hicolor/<n>x<n>/apps/ the application icon, at every size
#   share/doc/2ksbox/                 COPYING, notices, README
#   install.sh                        copy the above into a prefix
#
# `2ksbox` is the product (2ksbox.com); `com._2ksbox.Launcher` is the
# application ID the desktop entry, the icon and the Wayland app_id carry
# (a name segment may not start with a digit — flatpak rejects
# `com.2ksbox.…` — so the leading digit is escaped, as `7-zip.org` gets
# `org._7zip.…`). Everything else carries the same name since 2026-09-06:
# the repository, the docs and the user's data directory (moved once by
# `launcher-core/src/paths.rs::data_dir`).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUILD=1 TAR=1 SHADERS=0 OUT="$ROOT/build/package" PREFIX=""
while [ $# -gt 0 ]; do
  case "$1" in
    --no-build) BUILD=0; shift ;;
    --no-tar) TAR=0; shift ;;
    --with-shaders) SHADERS=1; shift ;;
    --out) OUT=$2; shift 2 ;;
    --prefix) PREFIX=$2; TAR=0; shift 2 ;;
    -h|--help) sed -n '2,45p' "$0"; exit 0 ;;
    *) echo "package-linux.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
done

VERSION=$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)
NAME="2ksbox-$VERSION-linux-$(uname -m)"
if [ -n "$PREFIX" ]; then STAGE="$PREFIX"; else STAGE="$OUT/$NAME"; fi

need() { [ -e "$1" ] || { echo "package-linux.sh: missing $1${2:+ ($2)}" >&2; exit 1; }; }
need build/qemu/libqemu-embed-i386.so "scripts/configure-qemu.sh && ninja -C build/qemu libqemu-embed-i386.so"
need build/qemu/qemu-img "ninja -C build/qemu qemu-img"
need qemu/pc-bios "scripts/prepare-qemu.sh"

if [ "$BUILD" = 1 ]; then
  cargo build --release -p player
  # Its own cargo workspace, so its own build command (scripts/build.sh's
  # `qt` stage does the same thing): that boundary is what keeps Qt 6 off
  # the root `cargo build`.
  ( cd launcher-qt && cargo build --release )
fi
need launcher-qt/target/release/launcher-qt "scripts/build.sh qt"
need target/release/player

# Only ever clear a staging directory of our own making. `--prefix` names
# somewhere that already exists and belongs to someone else (`/app`).
[ -n "$PREFIX" ] || rm -rf "$STAGE"
mkdir -p "$STAGE"/{bin,lib/2ksbox,libexec/2ksbox,share/2ksbox/desktop,share/doc/2ksbox}

install -m755 launcher-qt/target/release/launcher-qt "$STAGE/bin/2ksbox"
install -m755 target/release/player "$STAGE/bin/2ksbox-player"
install -m755 build/qemu/libqemu-embed-i386.so "$STAGE/lib/2ksbox/"
install -m755 build/qemu/qemu-img "$STAGE/libexec/2ksbox/"
# The Glide wrapper (doc 12 §5). qemu-3dfx's `hw/3dfx` only *dispatches*:
# at `grGlideInit` it dlopens a `libglide2x` and looks up 183 entry points,
# and the search that finds `build/glide` in a checkout finds nothing in a
# package — so without this file an installed guest has no Glide at all,
# silently, and `grSstWinOpen` fails. `player/src/companions.rs` names the
# packaged copy to QEMU through `QEMU_GLIDE_LIB`; the check below asks the
# staged player whether it really found this one.
if [ -f build/glide/libglide2x.so ]; then
  install -m755 build/glide/libglide2x.so "$STAGE/lib/2ksbox/"
else
  echo "package-linux.sh: no build/glide/libglide2x.so (scripts/build.sh glide); packaging without Glide — 3dfx titles will not run"
fi
# The Direct3D executor and the DXVK it runs on (doc 14), found the same
# way and staged together: the executor `dlopen`s DXVK by the name
# `companions.rs` puts in `D3DPT_DXVK_LIB`, so one without the other is a
# package whose XP guests fall back to WineD3D anyway. DXVK's real file
# carries its full version; it is installed under the soname the executor
# asks for, since nothing here links it and only that name is looked up.
# No Vulkan travels with the package: on Linux the system's driver is the
# right one, and a host below Vulkan 1.3 keeps the GL + WineD3D path by
# decision (ADR-013).
if [ -f build/d3dpt/libd3dpt_exec.so ] && [ -f build/dxvk/src/d3d9/libdxvk_d3d9.so.0 ]; then
  install -m755 build/d3dpt/libd3dpt_exec.so "$STAGE/lib/2ksbox/"
  install -m755 build/dxvk/src/d3d9/libdxvk_d3d9.so.0 "$STAGE/lib/2ksbox/libdxvk_d3d9.so.0"
else
  echo "package-linux.sh: no Direct3D executor (scripts/build.sh dxvk exec); packaging without it — XP Direct3D will fall back to WineD3D"
fi
rm -rf "$STAGE/share/2ksbox/pc-bios"   # a re-run must replace it, not nest inside it
cp -a qemu/pc-bios "$STAGE/share/2ksbox/pc-bios"

# The General MIDI bank the machine form's MIDI port plays through
# (doc 20 §4). Not optional like the shader presets: a machine whose
# music picker is on its default has nothing to play through without it,
# and 5.7 MB is not a size worth making anyone think about. The *player*
# names it to QEMU (LIBSYNTH_SF2, companions.rs), which is what the check
# below asks it.
install -Dm644 soundfonts/TimGM6mb.sf2 "$STAGE/share/2ksbox/soundfonts/TimGM6mb.sf2"

# The guest-tools ISO: the newest one, the same choice the launcher's
# "Add guest-tools ISO" button makes in a checkout.
iso=$(ls -t guest-tools/out/guest-tools-*.iso 2>/dev/null | head -1 || true)
if [ -n "$iso" ]; then
  mkdir -p "$STAGE/share/2ksbox/guest-tools"
  install -m644 "$iso" "$STAGE/share/2ksbox/guest-tools/"
else
  echo "package-linux.sh: no guest-tools ISO in guest-tools/out (guest-tools/build-wrappers.sh); packaging without it"
fi

# The shader presets are 80 MB and the launcher can fetch them itself, so
# they are opt-in; a distro package that would rather ship them says so.
if [ "$SHADERS" = 1 ]; then
  need third_party/slang-shaders "git submodule update --init third_party/slang-shaders"
  mkdir -p "$STAGE/share/2ksbox/shaders"
  # No .git*: this is a copy of the presets, not a checkout of them.
  tar -c --exclude='.git*' -C third_party/slang-shaders . | tar -x -C "$STAGE/share/2ksbox/shaders"
fi

install -m644 packaging/linux/com._2ksbox.Launcher.desktop "$STAGE/share/2ksbox/desktop/"
install -m644 packaging/linux/com._2ksbox.Launcher.metainfo.xml "$STAGE/share/2ksbox/desktop/"
# The icon goes in at its final path rather than beside the desktop entry:
# `share/icons/hicolor/<n>x<n>/apps/<app id>.png` is where a desktop looks
# for it, `install.sh` copies `share/` wholesale, and so the same tree is
# right for a distro package that unpacks the tarball into /usr as it is
# for a prefix install. Every size `scripts/gen-icons.sh` writes is
# shipped: 16 for a task switcher, 512 for a software centre's banner.
for icon in packaging/icon/2ksbox-*.png; do
  size=${icon##*-}; size=${size%.png}
  install -Dm644 "$icon" "$STAGE/share/icons/hicolor/${size}x${size}/apps/com._2ksbox.Launcher.png"
done
install -m755 packaging/linux/install.sh "$STAGE/install.sh"
install -m644 COPYING THIRD-PARTY-NOTICES.md README.md "$STAGE/share/doc/2ksbox/"

# --- the check -------------------------------------------------------
# Qt is the one thing this package does not carry, so it is the one thing
# whose absence would be found by a user rather than here: a launcher with
# an unresolved `libQt6Quick.so.6` says "No such file or directory" and
# nothing else. `ldd` answers for the import tables; the QML modules and
# the platform plugin are not in them and come from the same packages, so
# what is listed here is also what the tarball's README has to name.
fail=0
missing=$(ldd "$STAGE/bin/2ksbox" | grep 'not found' || true)
if [ -n "$missing" ]; then
  printf '%s\n' "$missing" | sed 's/^/  /' >&2
  echo "package-linux.sh: the staged launcher has unresolved libraries (install qt6-base and qt6-declarative)" >&2
  fail=1
else
  echo "qt             $(ldd "$STAGE/bin/2ksbox" | grep -c 'libQt6') Qt 6 libraries, all from the system"
fi

# A package whose launcher still answers with the checkout it was built
# from is not a package. Ask the staged binary itself, with `env -i` so
# not one LAUNCHER_*/PLAYER_* knob from this shell can be what makes it
# work, and from `/` so nothing is found by a relative path either.
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
resolved=$(cd / && env -i HOME="$scratch" LAUNCHER_LIBRARY_DIR="$scratch/machines" \
  "$STAGE/bin/2ksbox" --paths)
echo "$resolved"
while read -r what path; do
  case "$what" in
    player|qemu-img|pc-bios|guest-tools|prefix) ;;
    *) continue ;;
  esac
  # `--paths` says "(none built or shipped)" where there is nothing to
  # name — a package rolled without a guest-tools ISO, which is allowed
  # and already warned about above.
  case "$path" in "("*) continue ;; esac
  case "$path" in
    "$STAGE"|"$STAGE"/*) ;;
    *) echo "package-linux.sh: $what resolved outside the package: $path" >&2; fail=1 ;;
  esac
done <<< "$resolved"
# The player's own dependency: an installed tree has no build/qemu, so the
# origin-relative rpath (player/build.rs) is what has to find the embed
# library. ldd resolves it exactly as the loader will.
embed=$(cd / && env -i ldd "$STAGE/bin/2ksbox-player" | sed -n 's/.*libqemu-embed-i386.so => \([^ ]*\).*/\1/p')
embed=$(readlink -f "$embed" 2>/dev/null || echo "$embed")  # the loader reports it via bin/../lib
case "$embed" in
  "$STAGE"/lib/2ksbox/*) echo "libqemu-embed  $embed" ;;
  *) echo "package-linux.sh: the player's libqemu-embed came from $embed, not the package" >&2; fail=1 ;;
esac
# The one companion that is not a library: the General MIDI bank. Same
# question, same answer — the staged player's own rule has to find the
# copy this package staged, not one left in a checkout.
sf2=$(cd / && env -i "$STAGE/bin/2ksbox-player" --companions | awk '$1 == "soundfont" { print $2 }')
case "$sf2" in
  "$STAGE"/*) printf '%-15s%s\n' soundfont "$sf2" ;;
  *) echo "package-linux.sh: the bank is staged but the player answered ${sf2:-nothing}" >&2; fail=1 ;;
esac

# The companions QEMU dlopens late by name — the Glide wrapper here, the
# Direct3D executor and its DXVK on the packages that carry them. They are
# in no import table, so `ldd` above says nothing about them; the staged
# player's own rule (`player/src/companions.rs`) does, and `--companions`
# prints what that rule resolved. Asking the binary rather than restating
# the layout here is the point: a package that stages a file the player
# looks for somewhere else passes every other check in this script.
companions=$(cd / && env -i "$STAGE/bin/2ksbox-player" --companions)
while read -r what file; do
  got=$(printf '%s\n' "$companions" | awk -v w="$what" '$1 == w { print $2 }')
  if [ ! -f "$STAGE/lib/2ksbox/$file" ]; then
    # Not built on this host — the staging step above already said which
    # guests lose what. All that is left to check is that the player is not
    # about to hand QEMU somebody else's copy instead.
    case "$got" in
      "(not"|"") ;;
      *) echo "package-linux.sh: $what is not in the package, but the player found $got" >&2; fail=1 ;;
    esac
    continue
  fi
  case "$got" in
    "$STAGE"/*) printf '%-15s%s\n' "$what" "$got" ;;
    *) echo "package-linux.sh: $file is staged but the player answered ${got:-nothing}" >&2; fail=1 ;;
  esac
  # Each of these links the system's own GL / Vulkan stack, like every
  # other such program on the host; an unresolvable one fails deep inside
  # QEMU ("Glide pass-through off", "Direct3D pass-through off") and
  # nowhere a user would look.
  missing=$(ldd "$STAGE/lib/2ksbox/$file" | grep 'not found' || true)
  if [ -n "$missing" ]; then
    printf '%s\n' "$missing" | sed 's/^/  /' >&2
    echo "package-linux.sh: the staged $file has unresolved libraries" >&2
    fail=1
  fi
done <<EOF
glide       libglide2x.so
d3dpt-exec  libd3dpt_exec.so
dxvk        libdxvk_d3d9.so.0
EOF
# The window itself, which is the half `--paths` cannot reach. Qt resolves
# its platform plugin and every QML module the views import at run time,
# by name, from directories no import table mentions — so a package that
# has passed every check above still opens nothing on a host whose Qt is
# half installed, and says so in one line on a stderr a double-click has
# nowhere to show. The launcher's own headless grab (doc 07) is the check:
# `QT_QPA_PLATFORM=offscreen` plus `LAUNCHER_QT_SHOT`, and a PNG out the
# other end means a real window with real QML in it.
shot="$scratch/window.png"
if (cd / && env -i HOME="$scratch" LAUNCHER_LIBRARY_DIR="$scratch/machines" \
      QT_QPA_PLATFORM=offscreen LAUNCHER_QT_SHOT="$shot" LAUNCHER_QT_DELAY=1500 \
      "$STAGE/bin/2ksbox" >/dev/null 2>&1) && [ -s "$shot" ]; then
  echo "window         $(du -h "$shot" | cut -f1) grabbed offscreen: QML, plugins and all"
else
  echo "package-linux.sh: the staged launcher opened no window offscreen (Qt QML modules or platform plugin missing)" >&2
  fail=1
fi

# The machine's own bundle-creating path, end to end: the staged launcher
# runs the staged qemu-img to make a disk, and translates the result to a
# command line pointing at the staged firmware.
(cd / && env -i HOME="$scratch" LAUNCHER_LIBRARY_DIR="$scratch/machines" \
  "$STAGE/bin/2ksbox" --wizard-new xp "Package check" 1 >/dev/null)
bundle="$scratch/machines/package-check/machine.toml"
args=$(cd / && env -i HOME="$scratch" "$STAGE/bin/2ksbox" --print-args "$bundle")
case "$args" in
  *"-L $STAGE/share/2ksbox/pc-bios"*) echo "qemu args      -L inside the package" ;;
  *) echo "package-linux.sh: --print-args did not point at the packaged firmware" >&2; fail=1 ;;
esac
[ -s "$scratch/machines/package-check/disk.qcow2" ] \
  || { echo "package-linux.sh: the packaged qemu-img did not create a disk" >&2; fail=1; }
desktop-file-validate "$STAGE/share/2ksbox/desktop/com._2ksbox.Launcher.desktop" \
  || { echo "package-linux.sh: the desktop entry is not valid" >&2; fail=1; }
# The AppStream metadata, which the Flatpak (6b) will require and GNOME
# Software / KDE Discover read. `--no-net` because a package build must
# not depend on the network; only `E:` lines fail the build — a warning
# about a missing screenshot is a real gap (they need somewhere to be
# hosted) but not a reason to refuse to package.
if command -v appstreamcli >/dev/null; then
  metainfo_out=$(appstreamcli validate --no-net "$STAGE/share/2ksbox/desktop/com._2ksbox.Launcher.metainfo.xml" 2>&1) || true
  if printf '%s\n' "$metainfo_out" | grep -q '^E:'; then
    echo "$metainfo_out" >&2
    echo "package-linux.sh: the AppStream metadata has errors" >&2
    fail=1
  else
    echo "metainfo       $(printf '%s\n' "$metainfo_out" | tail -1)"
  fi
else
  echo "metainfo       (appstreamcli not installed; not validated)"
fi
[ "$fail" = 0 ] || exit 1
echo "checks passed"

du -sh "$STAGE" | sed 's/^/staged  /'
if [ "$TAR" = 1 ]; then
  if tar --help 2>/dev/null | grep -q -- --zstd; then
    archive="$OUT/$NAME.tar.zst"; comp=(--zstd)
  else
    archive="$OUT/$NAME.tar.gz"; comp=(-z)
  fi
  rm -f "$archive"
  tar -C "$OUT" "${comp[@]}" -cf "$archive" "$NAME"
  du -h "$archive" | sed 's/^/tarball /'
fi
echo "package: $STAGE"
