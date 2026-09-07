#!/usr/bin/env bash
# The macOS app (doc 07's "signed .app, JIT entitlement, notarized"):
# stage everything a stranger's Mac needs into one bundle that depends on
# nothing but the system, sign it for Developer ID, notarize it and roll a
# disk image.
#
#   scripts/package-macos.sh                      # build, stage, check, sign, notarize, dmg
#   scripts/package-macos.sh --no-build           # use what is in target/release
#   scripts/package-macos.sh --no-sign            # stage and check only (ad-hoc signed)
#   scripts/package-macos.sh --no-notarize        # sign, but do not submit
#   scripts/package-macos.sh --no-dmg             # leave the .app, no image
#   scripts/package-macos.sh --identity NAME      # default: the one Developer ID Application
#   scripts/package-macos.sh --keychain-profile P # notarytool credentials (default: 2ksbox-notary)
#   scripts/package-macos.sh --out DIR            # default build/macos
#
# Notarization needs credentials stored once, and they are not this
# script's to invent:
#   xcrun notarytool store-credentials 2ksbox-notary \
#       --apple-id <you@example.com> --team-id <TEAMID> --password <app-specific-password>
#
# What is different here from the Linux package (scripts/package-linux.sh),
# and why there are two scripts rather than one with a switch:
#
# * **Nothing may come from outside the bundle.** A Linux package leans on
#   the distribution for glib, pixman, zstd and the rest; a Mac has none
#   of them, and the machine that will run this has no Homebrew, no
#   XQuartz and no Vulkan. So the whole non-system dylib closure is copied
#   in and every install name rewritten to @rpath. That is the bulk of
#   this script.
# * **Qt travels with the app, and `macdeployqt` is what puts it there.**
#   The launcher is `launcher-qt` (ADR-015), so the bundle needs the Qt
#   frameworks, the cocoa platform plugin and the QtQuick QML module tree
#   — and only the first of those three is in a load command, which is
#   why the closure below would never have found the other two. Qt's own
#   deployment tool is the supported way to collect them, so it runs
#   first and the closure runs over what it leaves. Our QML is compiled
#   into the binary as a Qt resource, so `macdeployqt` is pointed at
#   `launcher-qt/qml` to find the imports: without `-qmldir` its import
#   scanner sees no QML at all and deploys no modules. What it collects
#   is then pruned and rewired — it deploys whole plugin categories and
#   QML module trees out of Homebrew's one shared Qt prefix, and leaves
#   both what we never asked for and stale Homebrew rpaths behind. The
#   two passes after it say why in full.
# * **The bundle *is* the prefix.** `Contents` has the same
#   `lib/libexec/share` shape a Unix prefix has — `launcher_core::paths`
#   finds it by the same `share/2ksbox` marker — with `MacOS/` in the part
#   `bin/` plays elsewhere, because that is the one directory macOS will
#   launch an executable from.
# * **Signing is inside-out and the floor is measured, not chosen.** Every
#   Mach-O is signed before the thing containing it, and
#   LSMinimumSystemVersion is the highest minos of everything carried,
#   since a bundled Homebrew dylib built on a newer system sets the real
#   floor whatever we would prefer to claim.
#
# It does not build QEMU, DXVK or the Glide wrapper. Per CLAUDE.md's build
# order those come from `scripts/build.sh`; what is missing is reported by
# name rather than quietly left out, except the three optional companions
# (Glide, the Direct3D executor, Vulkan), each of which is a warning and a
# guest that has one fewer accelerated path.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
[ "$(uname -s)" = Darwin ] || { echo "package-macos.sh: macOS only" >&2; exit 1; }

BUILD=1 SIGN=1 NOTARIZE=1 DMG=1 OUT="$ROOT/build/macos"
IDENTITY="" PROFILE="2ksbox-notary"
while [ $# -gt 0 ]; do
  case "$1" in
    --no-build) BUILD=0; shift ;;
    --no-sign) SIGN=0; NOTARIZE=0; shift ;;
    --no-notarize) NOTARIZE=0; shift ;;
    --no-dmg) DMG=0; shift ;;
    --identity) IDENTITY=$2; shift 2 ;;
    --keychain-profile) PROFILE=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    -h|--help) sed -n '2,55p' "$0"; exit 0 ;;
    *) echo "package-macos.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
done

VERSION=$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)
APP="$OUT/2ksbox.app"
C="$APP/Contents"

need() { [ -e "$1" ] || { echo "package-macos.sh: missing $1${2:+ ($2)}" >&2; exit 1; }; }
warn() { echo "package-macos.sh: $*" >&2; }
need build/qemu/libqemu-embed-i386.dylib "scripts/configure-qemu.sh && ninja -C build/qemu libqemu-embed-i386.dylib"
need build/qemu/qemu-img "ninja -C build/qemu qemu-img"
need qemu/pc-bios "scripts/prepare-qemu.sh"

if [ "$BUILD" = 1 ]; then
  cargo build --release -p player
  # Its own cargo workspace (ADR-015), so its own build command — the
  # boundary that keeps Qt 6 off a plain `cargo build`.
  ( cd launcher-qt && cargo build --release )
fi
need launcher-qt/target/release/launcher-qt "scripts/build.sh qt"
need target/release/player

# --- stage -----------------------------------------------------------
rm -rf "$APP"
mkdir -p "$C"/{MacOS,Resources,lib/2ksbox,libexec/2ksbox,share/2ksbox,share/doc/2ksbox}

install -m755 launcher-qt/target/release/launcher-qt "$C/MacOS/2ksbox"
install -m755 target/release/player   "$C/MacOS/2ksbox-player"
install -m755 build/qemu/libqemu-embed-i386.dylib "$C/lib/2ksbox/"
install -m755 build/qemu/qemu-img "$C/libexec/2ksbox/"
cp -a qemu/pc-bios "$C/share/2ksbox/pc-bios"
install -m644 COPYING THIRD-PARTY-NOTICES.md README.md "$C/share/doc/2ksbox/"

iso=$(ls -t guest-tools/out/guest-tools-*.iso 2>/dev/null | head -1 || true)
if [ -n "$iso" ]; then
  mkdir -p "$C/share/2ksbox/guest-tools" && install -m644 "$iso" "$C/share/2ksbox/guest-tools/"
else
  warn "no guest-tools ISO in guest-tools/out (guest-tools/build-wrappers.sh); packaging without it"
fi

# The three optional companions. QEMU dlopens each of them by a search
# that begins in a checkout's build/ directory, so the packaged player
# names them through the environment instead (player/src/companions.rs);
# all this has to do is put them where that expects.
if [ -f build/glide/libglide2x.dylib ]; then
  install -m755 build/glide/libglide2x.dylib "$C/lib/2ksbox/"
else
  warn "no build/glide/libglide2x.dylib (scripts/build-glide.sh); Glide games will not run"
fi

D3D=1
if [ -f build/d3dpt/libd3dpt_exec.dylib ] && [ -f build/dxvk/src/d3d9/libdxvk_d3d9.0.dylib ]; then
  install -m755 build/d3dpt/libd3dpt_exec.dylib "$C/lib/2ksbox/"
  install -m755 build/dxvk/src/d3d9/libdxvk_d3d9.0.dylib "$C/lib/2ksbox/"
else
  warn "no Direct3D executor (scripts/build-d3dpt-exec.sh); XP Direct3D will fall back"
  D3D=0
fi

# Vulkan: stock macOS has none, so the executor's driver travels with us.
# The LunarG SDK's own loader and the KosmicKrisp ICD (docs/build-macos.md,
# patches/dxvk/README.md) — the pair the D3D9 harnesses actually pass on.
if [ "$D3D" = 1 ]; then
  sdk=$(ls -d "$HOME"/VulkanSDK/*/macOS 2>/dev/null | sort -V | tail -1 || true)
  if [ -n "$sdk" ] && [ -f "$sdk/lib/libvulkan_kosmickrisp.dylib" ]; then
    # Follow the symlink: the bundle carries files, not links into $HOME.
    cp -L "$sdk/lib/libvulkan.1.dylib" "$C/lib/2ksbox/libvulkan.1.dylib"
    install -m755 "$sdk/lib/libvulkan_kosmickrisp.dylib" "$C/lib/2ksbox/"
    chmod 755 "$C/lib/2ksbox/libvulkan.1.dylib"
    # Our own ICD manifest: the SDK's points at its own tree, and
    # library_path is resolved relative to the manifest.
    mkdir -p "$C/share/2ksbox/vulkan/icd.d"
    cat > "$C/share/2ksbox/vulkan/icd.d/driver.json" <<JSON
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "../../../../lib/2ksbox/libvulkan_kosmickrisp.dylib",
        "api_version": "1.4.0",
        "is_portability_driver": true
    }
}
JSON
    echo "vulkan         $(basename "$(dirname "$sdk")") KosmicKrisp"
  else
    warn "no ~/VulkanSDK/*/macOS/lib/libvulkan_kosmickrisp.dylib; the app will carry no Vulkan driver and Direct3D will be off on a machine without one"
  fi
fi

# --- Info.plist, first pass -------------------------------------------
# `macdeployqt` below reads CFBundleExecutable out of the plist to know
# which binary to follow, so it has to exist before Qt is deployed rather
# than after everything is staged. It is written again at the end, when
# the floor can be measured from what the bundle actually carries.
write_plist() { sed -e "s/@VERSION@/$VERSION/" -e "s/@MINOS@/$1/" \
  packaging/macos/Info.plist.in > "$C/Info.plist"; }
write_plist 13.0

# --- Qt ---------------------------------------------------------------
# The frameworks, the cocoa platform plugin and the QtQuick QML modules
# (ADR-015). `-qmldir` is not optional: our QML is compiled into the
# binary as a Qt resource, so the import scanner has nothing to read
# unless it is pointed at the sources, and a bundle deployed without it
# starts and then dies on `module "QtQuick" is not installed`.
QMAKE=${QMAKE:-qmake6}
command -v "$QMAKE" >/dev/null || { echo "package-macos.sh: no $QMAKE (brew install qt); the launcher is Qt 6" >&2; exit 1; }
QT_BINS=$("$QMAKE" -query QT_HOST_BINS)
QT_PLUGINS=$("$QMAKE" -query QT_INSTALL_PLUGINS)
MACDEPLOYQT=${MACDEPLOYQT:-$QT_BINS/macdeployqt}
[ -x "$MACDEPLOYQT" ] || { echo "package-macos.sh: no macdeployqt at $MACDEPLOYQT (MACDEPLOYQT=)" >&2; exit 1; }
echo "qt             $("$QMAKE" -query QT_VERSION) from $("$QMAKE" -query QT_INSTALL_PREFIX)"
#
# Its "ERROR: Cannot resolve rpath @rpath/Qt<X>.framework/…" pairs are
# folded into one line: they are the over-collection described below,
# they come two lines each and by the dozen, and real trouble here is a
# different shape and a non-zero exit. What it collected is checked
# afterwards, at the bottom, rather than read out of this scroll.
"$MACDEPLOYQT" "$APP" -qmldir="$ROOT/launcher-qt/qml" -no-strip 2>&1 | awk '
  /^ERROR: Cannot resolve rpath/ { n++; pair=1; next }
  /^ERROR:  using QList/ && pair  { pair=0; next }
  { pair=0; print }
  END { if (n) printf "macdeployqt    %d unresolved framework references, pruned below\n", n }'

# What macdeployqt left half-rewritten. It copies a plain dependency
# dylib into Frameworks/ and rewrites the *reference* to it, but when the
# reference was already `@rpath/libfoo.dylib` — which Homebrew's own
# dylibs increasingly are, brotli 1.2.0 among them — there is nothing to
# rewrite, and the copy keeps both its Homebrew install name and
# Homebrew's `@loader_path/../lib` rpath. From Contents/Frameworks that
# rpath points at Contents/lib, where nothing of Qt's lives, so the
# sibling it names resolves nowhere and the first thing to load it (here:
# QtNetwork → libbrotlidec → libbrotlicommon) dies on a machine that has
# no Homebrew to fall back on. So give every plain dylib in there the
# same two things the closure below gives its own: an `@rpath` id, and
# `@loader_path` to find its siblings with.
for f in "$C/Frameworks"/*.dylib; do
  [ -f "$f" ] || continue
  install_name_tool -id "@rpath/$(basename "$f")" "$f" 2>/dev/null || true
  install_name_tool -add_rpath "@loader_path" "$f" 2>/dev/null || true
done

# What macdeployqt collected that can never load. Homebrew's Qt is
# modular — qtbase, qtdeclarative, qtvirtualkeyboard, … each its own
# prefix — and every installed formula symlinks its plugins and QML
# modules into one shared tree, while macdeployqt deploys plugin
# *categories* and QML module *directories* whole. So a launcher that
# imports QtQuick, Controls, Dialogs and Layouts comes out carrying
# QtQuick.VirtualKeyboard, Scene2D/3D, Pdf, Timeline and
# QtQml.StateMachine as well, and the frameworks those name live in
# prefixes macdeployqt never walked into: that is the
# "ERROR: Cannot resolve rpath @rpath/QtVirtualKeyboard.framework/…" it
# prints and carries on from. A plugin whose framework is in no copy of
# the bundle cannot be dlopened on any machine, so drop it rather than
# sign 25 MB of files that would fail their first import. The point is
# not the megabytes: it is that a real unresolved dependency then shows
# up as a missing file in the check below instead of as one more line in
# that scroll. Pruning too much is caught too — the offscreen window
# further down opens on the QML modules that survive this.
#
# The one thing to know about the shape of what it deployed: a QML
# module's plugin under Resources/qml is a **symlink** into PlugIns, not
# a copy. So the binaries are pruned first and the modules left holding a
# dangling link go after them, which is also what keeps a module from
# surviving as .qml files with no plugin — an import would then fail as
# "plugin cannot be loaded" instead of "module is not installed".
missing_fw() {  # frameworks a Mach-O names that the bundle does not carry
  otool -L "$1" | awk '{print $1}' \
    | sed -n 's|^@rpath/\([^/]*\.framework\)/.*|\1|p' | sort -u \
    | while read -r fw; do [ -d "$C/Frameworks/$fw" ] || echo "$fw"; done
}

pruned=0 prunedk=0
while read -r f; do
  miss=$(missing_fw "$f" | tr '\n' ' ')
  [ -n "$miss" ] || continue
  echo "qt prune       ${f#"$C/"} (needs ${miss% })"
  prunedk=$((prunedk + $(du -k "$f" | cut -f1))); pruned=$((pruned+1))
  rm -f "$f"
done < <(find "$C/PlugIns" "$C/Resources/qml" -type f -name '*.dylib' 2>/dev/null | sort)

# The modules those plugins belonged to: a dangling link is a plugin
# that has just gone. Take the whole module directory — but only once
# nothing under it still resolves, so a module that shares a directory
# with a plugin we kept is never taken with it.
while read -r l; do
  [ -L "$l" ] || continue        # its module went with an earlier link
  mod=$(dirname "$l")
  if [ -f "$mod/qmldir" ] && [ "$mod" != "$C/Resources/qml" ] \
     && [ -z "$(find "$mod" -name '*.dylib' -exec test -e {} \; -print -quit)" ]; then
    echo "qt prune       ${mod#"$C/"} (its plugin went)"
    prunedk=$((prunedk + $(du -sk "$mod" | cut -f1)))
    rm -rf "$mod"
  else
    rm -f "$l"
  fi
done < <(find "$C/Resources/qml" -type l ! -exec test -e {} \; -print 2>/dev/null | sort -r)
[ "$pruned" = 0 ] || echo "qt prune       $pruned plugins and their modules, $((prunedk/1024)) MB"

# The offscreen platform plugin, which macdeployqt does not deploy (it
# brings the cocoa one, which is the only one an app needs to run). The
# check below asks the staged launcher for a real window without opening
# one on the packager's screen, and that is the plugin it asks through.
if [ -f "$QT_PLUGINS/platforms/libqoffscreen.dylib" ]; then
  mkdir -p "$C/PlugIns/platforms"
  install -m755 "$QT_PLUGINS/platforms/libqoffscreen.dylib" "$C/PlugIns/platforms/"
else
  warn "no libqoffscreen.dylib in $QT_PLUGINS/platforms; the window check below will be skipped"
fi

# Every plugin has to be able to reach the bundle's own Frameworks, and
# on this Qt not one of them can: they arrive carrying Homebrew's
# `@loader_path/../../../../lib` and nothing else, which from
# Contents/PlugIns/<category> points at the *build directory* — that is
# the path in macdeployqt's own "using QList(…)" complaints. It gets away
# with it for the plugins whose Qt references it rewrote to
# @executable_path; the ones Homebrew already built with @rpath
# references (libqsvg, libqsvgicon, the multimedia plugin here) resolve
# nowhere and fail their dlopen with the framework sitting right there.
# So give each of them both ways in: from itself, and from whatever
# executable loaded it — the second is what covers a QML plugin, which
# Resources/qml reaches through a symlink and whose @loader_path is
# therefore not the directory the file is really in.
while read -r f; do
  install_name_tool -add_rpath "@loader_path/../../Frameworks" "$f" 2>/dev/null || true
  install_name_tool -add_rpath "@executable_path/../Frameworks" "$f" 2>/dev/null || true
done < <(find "$C/PlugIns" -type f -name '*.dylib' 2>/dev/null)

# --- the dylib closure ------------------------------------------------
# Everything outside /usr/lib and /System — Homebrew's glib/pixman/zstd/…,
# XQuartz's libGL (QEMU's opengl feature links it even though the embed
# backend only ever dlsyms OpenGL.framework: CLAUDE.md's macOS gotcha) —
# copied into lib/2ksbox and rewritten to @rpath, transitively.
LIBDIR="$C/lib/2ksbox"
external() { otool -L "$1" | tail -n +2 | awk '{print $1}' | grep -E '^(/opt/|/usr/local/)' || true; }

# Every Mach-O in the bundle, which since Qt arrived is no longer the same
# thing as every executable file in it: a QML module's plugin can be mode
# 644 and it still carries a signature that a rewritten load command
# invalidates — and on arm64 a broken signature is SIGKILL, not a warning.
machos() {
  find "$C" -type f \( -perm +111 -o -name '*.dylib' \) \
    -exec sh -c 'file -b "$1" | grep -q Mach-O && echo "$1"' _ {} \;
}

bundle_deps() {
  local file=$1 dep leaf real
  for dep in $(external "$file"); do
    real=$(python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$dep")
    leaf=$(basename "$dep")
    if [ ! -f "$LIBDIR/$leaf" ]; then
      [ -f "$real" ] || { warn "cannot find $dep, needed by $(basename "$file")"; continue; }
      install -m755 "$real" "$LIBDIR/$leaf"
      install_name_tool -id "@rpath/$leaf" "$LIBDIR/$leaf" 2>/dev/null || true
      # Each bundled library must find its own siblings in this same
      # directory, whatever loaded it.
      install_name_tool -add_rpath "@loader_path" "$LIBDIR/$leaf" 2>/dev/null || true
      bundle_deps "$LIBDIR/$leaf"
    fi
    install_name_tool -change "$dep" "@rpath/$leaf" "$file" 2>/dev/null || true
  done
}

for f in "$LIBDIR"/*.dylib "$C/libexec/2ksbox/qemu-img" "$C/MacOS/2ksbox" "$C/MacOS/2ksbox-player"; do
  [ -f "$f" ] || continue
  bundle_deps "$f"
done
# Our own libraries kept an absolute or build-tree id; @rpath is what the
# things loading them ask for.
for leaf in libqemu-embed-i386.dylib libglide2x.dylib libd3dpt_exec.dylib libdxvk_d3d9.0.dylib libvulkan.1.dylib libvulkan_kosmickrisp.dylib; do
  [ -f "$LIBDIR/$leaf" ] || continue
  install_name_tool -id "@rpath/$leaf" "$LIBDIR/$leaf" 2>/dev/null || true
  install_name_tool -add_rpath "@loader_path" "$LIBDIR/$leaf" 2>/dev/null || true
done

# And the executables. The player already has @loader_path/../lib/2ksbox
# from player/build.rs.
install_name_tool -add_rpath "@loader_path/../lib/2ksbox" "$C/MacOS/2ksbox-player" 2>/dev/null || true
install_name_tool -add_rpath "@loader_path/../../lib/2ksbox" "$C/libexec/2ksbox/qemu-img" 2>/dev/null || true
# Every rpath that points out of the app has to go, from libraries as
# much as from executables: meson gives libqemu-embed one LC_RPATH per
# Homebrew prefix it linked against, they are searched before the
# @loader_path added above, and a bundle that keeps them loads this
# machine's Homebrew instead of its own copies — passing every check
# here and failing on the first machine that has no Homebrew at all.
while read -r f; do
  while read -r rp; do
    case "$rp" in "$ROOT"/*|/opt/*|/usr/local/*)
      install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null || true ;;
    esac
  done < <(otool -l "$f" | awk '/LC_RPATH/{r=1} r&&/path /{print $2; r=0}')
done < <(machos)

# Rewriting a load command breaks the signature every arm64 binary must
# have, and the kernel answers a broken one with SIGKILL and nothing else
# — which is what the checks below would run into. So re-sign ad hoc now.
# The real Developer ID signature replaces this further down; here it only
# has to make the staged app runnable.
while read -r f; do codesign --force --sign - "$f" >/dev/null 2>&1 || true; done \
  < <(machos)

# --- icon -------------------------------------------------------------
# The same PNGs the Linux package installs (`scripts/gen-icons.sh`), so
# the three platforms draw one icon from one master. Nothing is
# rasterized here: an .icns is a container, and iconutil is happy with a
# partial set — 512@2x would need a 1024 the artwork does not have.
set=$(mktemp -d)/2ksbox.iconset; mkdir -p "$set"
for s in 16 32 64 128 256 512; do
  cp "packaging/icon/2ksbox-$s.png" "$set/icon_${s}x${s}.png"
done
# The @2x names Apple wants are the next size up under the previous name.
for s in 16 32 128 256; do cp "$set/icon_$((s*2))x$((s*2)).png" "$set/icon_${s}x${s}@2x.png"; done
rm -f "$set/icon_64x64.png"
iconutil -c icns "$set" -o "$C/Resources/2ksbox.icns"

# --- Info.plist -------------------------------------------------------
# The floor is whatever the bundle's own Mach-O files require, which is
# usually set by a dependency and not by us.
minos=$(machos | while read -r f; do
  otool -l "$f" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}'
done | sort -V | tail -1)
minos=${minos:-13.0}
write_plist "$minos"
echo "minimum macOS $minos"

# --- the check --------------------------------------------------------
# The same question package-linux.sh asks, in the form a Mac can answer:
# does anything in here still reach outside the bundle? `env -i` from /,
# so no LAUNCHER_*/PLAYER_*/DYLD_* of this shell is what makes it work.
fail=0
while read -r f; do
  out=$(external "$f")
  [ -z "$out" ] || { echo "package-macos.sh: $f still links $(echo "$out" | tr '\n' ' ')" >&2; fail=1; }
done < <(machos)

# And every @rpath dependency must resolve through the binary's own
# rpaths, inside the bundle. A file that fails this cannot load on any
# machine — but on *this* one it is invisible, because the Homebrew copy
# it was built against is still on disk for the parts of the loader that
# would otherwise complain. It is what the prune above leaves behind if
# it missed something, and what a half-rewritten install name (see the
# Frameworks pass) looks like from here.
# dyld expands @rpath with the rpaths of every image in the chain that
# led to the load, not only the one holding the reference — which is why
# a Qt plugin whose own rpath is Homebrew's stale
# `@loader_path/../../../../lib` still finds QtSvg: the executable that
# dlopened it has `@executable_path/../Frameworks`. So the check has to
# know the same thing, or it fails files that demonstrably work.
exe_rpaths=""
for x in "$C/MacOS/2ksbox" "$C/MacOS/2ksbox-player"; do
  [ -f "$x" ] || continue
  while read -r rp; do
    rp=${rp//@loader_path/$C\/MacOS}; rp=${rp//@executable_path/$C\/MacOS}
    exe_rpaths="$exe_rpaths $rp"
  done < <(otool -l "$x" | awk '/LC_RPATH/{r=1} r&&/path /{print $2; r=0}')
done

unresolved() {
  local f=$1 id rps dep rp cand ok
  id=$(otool -D "$f" | tail -n +2)
  rps=$(otool -l "$f" | awk '/LC_RPATH/{r=1} r&&/path /{print $2; r=0}')
  # `|| true`: a binary with no @rpath dependency at all is the normal
  # case, and grep's empty-handed 1 would end the script under pipefail.
  otool -L "$f" | tail -n +2 | awk '{print $1}' | { grep '^@rpath/' || true; } | sort -u \
    | while read -r dep; do
        if [ "$dep" != "$id" ]; then      # its own install name is no dependency
          ok=0
          for rp in $rps $exe_rpaths; do
            cand=${rp//@loader_path/$(dirname "$f")}
            cand=${cand//@executable_path/$C\/MacOS}
            if [ -e "$cand/${dep#@rpath/}" ]; then ok=1; break; fi
          done
          [ "$ok" = 1 ] || echo "${dep#@rpath/}"
        fi
      done
}
while read -r f; do
  miss=$(unresolved "$f" | tr '\n' ' ')
  [ -z "$miss" ] || { echo "package-macos.sh: $f needs ${miss% }, which its rpaths do not reach" >&2; fail=1; }
done < <(machos)

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
resolved=$(cd / && env -i HOME="$scratch" LAUNCHER_LIBRARY_DIR="$scratch/machines" "$C/MacOS/2ksbox" --paths)
echo "$resolved"
while read -r what path; do
  case "$what" in player|qemu-img|pc-bios|guest-tools|prefix) ;; *) continue ;; esac
  case "$path" in "("*) continue ;; esac
  case "$path" in "$APP"|"$APP"/*) ;; *) echo "package-macos.sh: $what resolved outside the app: $path" >&2; fail=1 ;; esac
done <<< "$resolved"

# What the *loader* actually did, which is the question a friend's Mac
# will ask. Not one image outside the app and the system may be loaded:
# an installed app has no build/qemu, so the @loader_path rpath
# (player/build.rs) is all there is to find libqemu-embed with, and every
# Homebrew or XQuartz library reached from here would be one this machine
# has and theirs does not. `--mode-sweep` is the display path end to end
# without a guest, and it is enough to pull the embed library in.
loaded=$(cd / && env -i DYLD_PRINT_LIBRARIES=1 \
  "$C/MacOS/2ksbox-player" --mode-sweep "$scratch/sweep" 2>&1 \
  | sed -n 's|^dyld\[[0-9]*\]: <[^>]*> ||p')
case "$loaded" in
  *libqemu-embed-i386.dylib*) ;;
  *) echo "package-macos.sh: the packaged player never loaded libqemu-embed" >&2; fail=1 ;;
esac
outside=$(printf '%s\n' "$loaded" | grep -v -e "^$APP/" -e '^/usr/lib/' -e '^/System/' || true)
if [ -n "$outside" ]; then
  printf '%s\n' "$outside" | sed 's/^/  /' >&2
  echo "package-macos.sh: the packaged player loaded the above from outside the app" >&2
  fail=1
else
  echo "loader         $(printf '%s\n' "$loaded" | grep -c "^$APP/") images from the app, the rest from the system"
fi

# The window, which `--paths` never opens and the closure could never
# have checked: Qt loads its platform plugin and every QtQuick module by
# name at run time, out of PlugIns/ and Resources/qml, and nothing names
# them in a load command. So open one — offscreen, so it does not appear
# on the packager's screen — and ask the loader the same question again
# while it happens. A PNG out of `LAUNCHER_QT_SHOT` (doc 07) means the
# QML engine really ran; the image list means it ran on our copy of Qt
# and not on the Homebrew one this Mac happens to have.
if [ -f "$C/PlugIns/platforms/libqoffscreen.dylib" ]; then
  shot="$scratch/window.png"
  qtloaded=$(cd / && env -i HOME="$scratch" LAUNCHER_LIBRARY_DIR="$scratch/machines" \
    QT_QPA_PLATFORM=offscreen LAUNCHER_QT_SHOT="$shot" LAUNCHER_QT_DELAY=1500 \
    DYLD_PRINT_LIBRARIES=1 "$C/MacOS/2ksbox" 2>&1 \
    | sed -n 's|^dyld\[[0-9]*\]: <[^>]*> ||p')
  if [ -s "$shot" ]; then
    echo "window         $(du -h "$shot" | cut -f1) grabbed offscreen: QML, plugins and all"
  else
    echo "package-macos.sh: the staged launcher opened no window offscreen (Qt plugins or QML modules missing)" >&2
    fail=1
  fi
  outside=$(printf '%s\n' "$qtloaded" | grep -v -e "^$APP/" -e '^/usr/lib/' -e '^/System/' || true)
  if [ -n "$outside" ]; then
    printf '%s\n' "$outside" | sed 's/^/  /' >&2
    echo "package-macos.sh: the staged launcher loaded the above from outside the app" >&2
    fail=1
  fi
else
  echo "window         (no offscreen plugin staged; not checked)"
fi

# The bundle-creating path end to end, as package-linux.sh does it.
(cd / && env -i HOME="$scratch" LAUNCHER_LIBRARY_DIR="$scratch/machines" \
  "$C/MacOS/2ksbox" --wizard-new xp "Package check" 1 >/dev/null)
args=$(cd / && env -i HOME="$scratch" "$C/MacOS/2ksbox" --print-args "$scratch/machines/package-check/machine.toml")
case "$args" in
  *"-L $C/share/2ksbox/pc-bios"*) echo "qemu args      -L inside the app" ;;
  *) echo "package-macos.sh: --print-args did not point at the packaged firmware" >&2; fail=1 ;;
esac
[ -s "$scratch/machines/package-check/disk.qcow2" ] \
  || { echo "package-macos.sh: the packaged qemu-img did not create a disk" >&2; fail=1; }
[ "$fail" = 0 ] || exit 1
echo "checks passed"

# --- sign -------------------------------------------------------------
if [ "$SIGN" = 1 ]; then
  if [ -z "$IDENTITY" ]; then
    IDENTITY=$(security find-identity -v -p codesigning | sed -n 's/.*"\(Developer ID Application: .*\)"/\1/p' | head -1)
    [ -n "$IDENTITY" ] || { echo "package-macos.sh: no Developer ID Application identity; --identity or --no-sign" >&2; exit 1; }
  fi
  echo "signing as $IDENTITY"
  # --options runtime is the hardened runtime notarization requires, and
  # the entitlement beside it is what lets TCG keep its JIT under it.
  #
  # --timestamp is not optional either (notarization rejects a signature
  # without one), and Apple's timestamp service goes away for a few
  # seconds often enough that a bundle with this many Mach-O files will
  # hit it: "The timestamp service is not available". That is worth
  # retrying and nothing else here is, so the retry is narrow.
  sign_one() {
    local i out
    for i in 1 2 3 4 5; do
      if out=$(codesign --force --timestamp --options runtime \
                 --entitlements packaging/macos/2ksbox.entitlements \
                 --sign "$IDENTITY" "$1" 2>&1); then
        return 0
      fi
      case "$out" in
        *"timestamp service is not available"*|*"timestamp"*"unavailable"*)
          warn "timestamp service unavailable signing $(basename "$1"); retry $i"
          sleep 5 ;;
        *) printf '%s\n' "$out" >&2; return 1 ;;
      esac
    done
    printf '%s\n' "$out" >&2
    return 1
  }
  # Inside-out: every nested Mach-O before the thing that seals it, and a
  # framework signed as the bundle it is rather than as the file inside
  # it — `codesign` seals a framework by its directory, and one sealed
  # only at Versions/A/QtCore is what `--verify --deep --strict` rejects.
  while read -r f; do
    case "$f" in
      "$C"/Frameworks/*.framework/*) continue ;;   # signed as a bundle below
      "$C"/MacOS/2ksbox) continue ;;               # the app's own executable, last
    esac
    sign_one "$f"
  done < <(machos | sort -u)
  for fw in "$C"/Frameworks/*.framework; do
    [ -d "$fw" ] && sign_one "$fw"
  done
  sign_one "$C/MacOS/2ksbox"
  sign_one "$APP"
  codesign --verify --deep --strict --verbose=2 "$APP"
  # The question Gatekeeper will actually ask on the other Mac. Before
  # notarization it answers "not notarized", which is not a failure here
  # — it is the one remaining step — but any other rejection is.
  spctl --assess --type execute --verbose=4 "$APP" 2>&1 | sed 's/^/  /' || true
fi

# --- notarize ---------------------------------------------------------
staple_check() { xcrun stapler validate "$1" >/dev/null && echo "stapled        $1"; }
if [ "$NOTARIZE" = 1 ]; then
  zip="$OUT/2ksbox-$VERSION.zip"
  rm -f "$zip"
  # ditto, not zip(1): the bundle's symlinks and signature must survive.
  /usr/bin/ditto -c -k --keepParent "$APP" "$zip"
  echo "submitting $(du -h "$zip" | cut -f1) to notarytool as profile '$PROFILE'"
  xcrun notarytool submit "$zip" --keychain-profile "$PROFILE" --wait
  xcrun stapler staple "$APP"
  staple_check "$APP"
  rm -f "$zip"
fi

# --- disk image -------------------------------------------------------
if [ "$DMG" = 1 ]; then
  dmgroot=$(mktemp -d)
  cp -a "$APP" "$dmgroot/"
  ln -s /Applications "$dmgroot/Applications"
  dmg="$OUT/2ksbox-$VERSION-macos-$(uname -m).dmg"
  rm -f "$dmg"
  hdiutil create -volname "2ksbox $VERSION" -srcfolder "$dmgroot" -ov -format UDZO -quiet "$dmg"
  rm -rf "$dmgroot"
  if [ "$SIGN" = 1 ]; then
    codesign --force --timestamp --sign "$IDENTITY" "$dmg"
  fi
  if [ "$NOTARIZE" = 1 ]; then
    xcrun notarytool submit "$dmg" --keychain-profile "$PROFILE" --wait
    xcrun stapler staple "$dmg"
    staple_check "$dmg"
  fi
  du -h "$dmg" | sed 's/^/image   /'
fi

du -sh "$APP" | sed 's/^/app     /'
echo "package: $APP"
