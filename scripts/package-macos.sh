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
    -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
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

[ "$BUILD" = 0 ] || cargo build --release -p launcher -p player
need target/release/launcher
need target/release/player

# --- stage -----------------------------------------------------------
rm -rf "$APP"
mkdir -p "$C"/{MacOS,Resources,lib/2ksbox,libexec/2ksbox,share/2ksbox,share/doc/2ksbox}

install -m755 target/release/launcher "$C/MacOS/2ksbox"
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

# --- the dylib closure ------------------------------------------------
# Everything outside /usr/lib and /System — Homebrew's glib/pixman/zstd/…,
# XQuartz's libGL (QEMU's opengl feature links it even though the embed
# backend only ever dlsyms OpenGL.framework: CLAUDE.md's macOS gotcha) —
# copied into lib/2ksbox and rewritten to @rpath, transitively.
LIBDIR="$C/lib/2ksbox"
external() { otool -L "$1" | tail -n +2 | awk '{print $1}' | grep -E '^(/opt/|/usr/local/)' || true; }

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
# sdl2-compat is not linked to SDL3, it dlopens it — from @loader_path
# first, which is why putting it beside sdl2-compat is enough and why no
# walk of load commands would ever have found it. QEMU's SDL display is
# dead weight in the embed library (the player draws through wgpu), but
# the library is linked in and something in it does call SDL, so the app
# carries the pair rather than finding out on a machine that has neither.
sdl3=""
for d in "$(brew --prefix 2>/dev/null || echo /opt/homebrew)/lib" /opt/homebrew/lib /usr/local/lib; do
  [ -f "$d/libSDL3.dylib" ] || continue
  sdl3=$(python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$d/libSDL3.dylib")
  break
done
if [ -f "$LIBDIR/libSDL2-2.0.0.dylib" ]; then
  if [ -f "$sdl3" ]; then
    install -m755 "$sdl3" "$LIBDIR/libSDL3.dylib"
    install_name_tool -id "@rpath/libSDL3.dylib" "$LIBDIR/libSDL3.dylib" 2>/dev/null || true
    bundle_deps "$LIBDIR/libSDL3.dylib"
  else
    warn "libSDL2 is bundled but SDL3 is not beside it; sdl2-compat will find no SDL3 on another Mac"
  fi
fi

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
done < <(find "$C" -type f -perm +111 -exec sh -c 'file -b "$1" | grep -q Mach-O && echo "$1"' _ {} \;)

# Rewriting a load command breaks the signature every arm64 binary must
# have, and the kernel answers a broken one with SIGKILL and nothing else
# — which is what the checks below would run into. So re-sign ad hoc now.
# The real Developer ID signature replaces this further down; here it only
# has to make the staged app runnable.
while read -r f; do codesign --force --sign - "$f" >/dev/null 2>&1 || true; done \
  < <(find "$C" -type f -perm +111 -exec sh -c 'file -b "$1" | grep -q Mach-O && echo "$1"' _ {} \;)

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
minos=$(find "$C" -type f -perm +111 -exec sh -c 'file -b "$1" | grep -q Mach-O && otool -l "$1" | awk "/LC_BUILD_VERSION/{f=1} f&&/minos/{print \$2; exit}"' _ {} \; \
  | sort -V | tail -1)
minos=${minos:-13.0}
sed -e "s/@VERSION@/$VERSION/" -e "s/@MINOS@/$minos/" packaging/macos/Info.plist.in > "$C/Info.plist"
echo "minimum macOS $minos"

# --- the check --------------------------------------------------------
# The same question package-linux.sh asks, in the form a Mac can answer:
# does anything in here still reach outside the bundle? `env -i` from /,
# so no LAUNCHER_*/PLAYER_*/DYLD_* of this shell is what makes it work.
fail=0
while read -r f; do
  out=$(external "$f")
  [ -z "$out" ] || { echo "package-macos.sh: $f still links $(echo "$out" | tr '\n' ' ')" >&2; fail=1; }
done < <(find "$C" -type f -perm +111 -exec sh -c 'file -b "$1" | grep -q Mach-O && echo "$1"' _ {} \;)

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
  # Inside-out: every nested Mach-O before the bundle that seals it.
  while read -r f; do sign_one "$f"; done \
    < <(find "$C/lib" "$C/libexec" -type f \( -name '*.dylib' -o -perm +111 \) 2>/dev/null | sort -u)
  sign_one "$C/MacOS/2ksbox-player"
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
