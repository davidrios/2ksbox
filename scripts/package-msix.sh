#!/usr/bin/env bash
# The Windows package as an MSIX, for the Microsoft Store and for a
# sideload (docs/build-windows.md, "The Store package").
#
#   scripts/package-msix.sh <staged-package>            # layout, then pack
#   scripts/package-msix.sh <staged-package> --no-pack  # the layout only
#   scripts/package-msix.sh <staged-package> --pfx cert.pfx [--pfx-password P]
#                                                       # ... and sign, for a sideload
#   --out DIR                       default build/win/package
#   --version A.B.C.D               default Cargo.toml's version, then ".0" for a
#                                   Store identity and a revision that counts up
#                                   per pack (build/win/package/msix-revision)
#                                   for the development one, since Windows will
#                                   not install a version it already has
#   --identity NAME                 Partner Center's package identity name
#   --publisher "CN=..."            Partner Center's publisher
#   --publisher-display NAME        Partner Center's publisher display name
#
# The identity triple can also come from MSIX_IDENTITY, MSIX_PUBLISHER and
# MSIX_PUBLISHER_DISPLAY. Without any, the package gets a development
# identity that only installs sideloaded, signed with a test certificate
# whose subject is that publisher; the Store refuses it.
#
# <staged-package> is the one-folder tree scripts/package-windows.sh
# stages (or the unzipped zip): `2ksbox.exe` at the top, `pc-bios` beside
# it. An MSIX is that same tree plus `AppxManifest.xml` and the Store's
# four logos, so the layout is a copy of it with those added, minus
# `2ksbox-debug.bat` (it writes its log beside itself, and a Store
# install's folder is read-only).
#
# Packing needs `makeappx.exe`, which only the Windows SDK has, so on a
# Linux build host this script stops after the layout and says how to
# finish on a PC; `package-windows.sh --msix` calls it that way. On the
# PC (Git Bash or MSYS2, the SDK installed) it finds makeappx and signtool
# under `Windows Kits`, or takes MAKEAPPX= / SIGNTOOL=.
#
# The Store signs its own uploads, so an upload is left unsigned. A
# sideload must be signed (`--pfx`) and the certificate trusted on the
# PC, which the doc's recipe covers.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

STAGE= OUT="$ROOT/build/win/package" PACK=1 PFX= PFXPW= VERSION=
IDENTITY=${MSIX_IDENTITY:-} PUBLISHER=${MSIX_PUBLISHER:-} PUBDISPLAY=${MSIX_PUBLISHER_DISPLAY:-}
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT=$2; shift 2 ;;
    --no-pack) PACK=0; shift ;;
    --pfx) PFX=$2; shift 2 ;;
    --pfx-password) PFXPW=$2; shift 2 ;;
    --version) VERSION=$2; shift 2 ;;
    --identity) IDENTITY=$2; shift 2 ;;
    --publisher) PUBLISHER=$2; shift 2 ;;
    --publisher-display) PUBDISPLAY=$2; shift 2 ;;
    -h|--help) sed -n '2,36p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "package-msix.sh: unknown argument: $1" >&2; exit 2 ;;
    *) [ -z "$STAGE" ] || { echo "package-msix.sh: one staged package, not two" >&2; exit 2; }
       STAGE=$1; shift ;;
  esac
done
[ -n "$STAGE" ] || { echo "package-msix.sh: which staged package? (scripts/package-windows.sh --no-zip stages one)" >&2; exit 2; }
STAGE="$(cd "$STAGE" && pwd)"

need() { [ -e "$1" ] || { echo "package-msix.sh: missing $1${2:+ ($2)}" >&2; exit 1; }; }
need "$STAGE/2ksbox.exe" "not a staged package"
need "$STAGE/pc-bios"    "not a staged package: the launcher finds its prefix by this directory"
need packaging/windows/AppxManifest.xml.in
for a in Square44x44Logo Square150x150Logo Wide310x150Logo StoreLogo; do
  need "packaging/windows/Assets/$a.png" "scripts/gen-icons.sh"
done

# --- identity ---------------------------------------------------------
dev=0
if [ -z "$IDENTITY$PUBLISHER$PUBDISPLAY" ]; then
  dev=1
  IDENTITY=2ksbox.dev PUBLISHER="CN=2ksbox-dev" PUBDISPLAY="2ksbox (development)"
elif [ -z "$IDENTITY" ] || [ -z "$PUBLISHER" ] || [ -z "$PUBDISPLAY" ]; then
  echo "package-msix.sh: the identity is three values or none (--identity, --publisher, --publisher-display)" >&2
  exit 2
fi
case "$PUBLISHER" in CN=*) ;; *) echo "package-msix.sh: the publisher is a certificate subject (CN=...)" >&2; exit 2 ;; esac

if [ -z "$VERSION" ]; then
  base="$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)"
  if [ "$dev" = 1 ]; then
    # A development package is installed over and over on one PC, and
    # Windows refuses a package whose version it already has, so each
    # pack takes the next revision. The counter lives beside the
    # packages, outside the layout this script recreates.
    mkdir -p "$OUT"
    rev=$(( $(cat "$OUT/msix-revision" 2>/dev/null || echo 0) + 1 ))
    printf '%s\n' "$rev" > "$OUT/msix-revision"
    VERSION="$base.$rev"
  else
    # The Store keeps the fourth number for itself: 0 on every upload.
    VERSION="$base.0"
  fi
fi
case "$VERSION" in
  *[!0-9.]*|*..*|.*|*.) bad=1 ;;
  *) bad=0; n=$(printf '%s' "$VERSION" | tr -cd . | wc -c); [ "$n" = 3 ] || bad=1 ;;
esac
[ "$bad" = 0 ] || { echo "package-msix.sh: an MSIX version is four numbers (A.B.C.D), not $VERSION" >&2; exit 2; }
# The Store's own rule ("App package requirements", version numbering):
# the first number cannot be 0, and the fourth must be 0. A development
# package is bound by neither.
if [ "$dev" = 0 ]; then
  case "$VERSION" in
    0.*) echo "package-msix.sh: the Store refuses a version whose first number is 0 ($VERSION): bump Cargo.toml's version to 1.0.0 or later, or pass --version" >&2; exit 2 ;;
    *.0) ;;
    *) echo "package-msix.sh: a Store upload's fourth number must be 0, not $VERSION (the Store keeps it for itself)" >&2; exit 2 ;;
  esac
fi
NAME="2ksbox-${VERSION%.0}-windows-x64"

# --- the layout -------------------------------------------------------
LAYOUT="$OUT/msix"
rm -rf "$LAYOUT"
mkdir -p "$LAYOUT/Assets"
# Everything staged, verbatim. The MSIX's file system is the package's
# and read-only, and nothing in the tree writes beside itself except the
# debug console, which stays with the zip.
tar -c -C "$STAGE" --exclude=2ksbox-debug.bat . | tar -x -C "$LAYOUT"
install -m644 packaging/windows/Assets/*.png "$LAYOUT/Assets/"

manifest=$(<packaging/windows/AppxManifest.xml.in)
manifest=${manifest//@IDENTITY_NAME@/$IDENTITY}
manifest=${manifest//@PUBLISHER@/$PUBLISHER}
manifest=${manifest//@PUBLISHER_DISPLAY_NAME@/$PUBDISPLAY}
manifest=${manifest//@VERSION@/$VERSION}
printf '%s\n' "$manifest" > "$LAYOUT/AppxManifest.xml"
grep -q '@[A-Z_]*@' "$LAYOUT/AppxManifest.xml" && { echo "package-msix.sh: a field of the manifest template was not filled" >&2; exit 1; }

echo "identity       $IDENTITY, $PUBLISHER, \"$PUBDISPLAY\"$([ $dev = 1 ] && echo ' (development: sideload only)')"
echo "version        $VERSION"
echo "layout         $LAYOUT ($(du -sh "$LAYOUT" | cut -f1))"
[ "$PACK" = 1 ] || exit 0

# --- the pack ---------------------------------------------------------
# makeappx validates the manifest against its schema and every path it
# names against the layout, so a pack that succeeds is a package the
# Store's upload check accepts structurally. The Windows SDK installs
# one copy per SDK version; the newest is taken.
sdk_tool() { # name
  local t
  t=$(ls -d "/c/Program Files (x86)/Windows Kits/10/bin/"*/x64/"$1.exe" 2>/dev/null | sort -V | tail -1 || true)
  [ -n "$t" ] || t=$(command -v "$1.exe" 2>/dev/null || command -v "$1" 2>/dev/null || true)
  printf '%s' "$t"
}
MAKEAPPX=${MAKEAPPX:-$(sdk_tool makeappx)}
if [ -z "$MAKEAPPX" ]; then
  echo "makeappx       none here (the Windows SDK's); the layout is ready to pack on a PC:"
  echo "               scripts/package-msix.sh <this staged package>"
  exit 0
fi
# Windows paths for a Windows tool. `cygpath` is MSYS2's and Git Bash's, and
# MSYS2_ARG_CONV_EXCL stops either from rewriting `/o` into a drive `O:/`.
export MSYS2_ARG_CONV_EXCL="*"
wpath() { if command -v cygpath >/dev/null; then cygpath -w "$1"; else printf '%s' "$1"; fi; }

MSIX="$OUT/$NAME.msix"
# Only the newest package is kept (and the signed copy
# scripts/win-sideload.ps1 makes of it): with a revision in the name, a
# pile of older ones would otherwise build up and the sideload script's
# "newest" would be a matter of timestamps.
rm -f "$OUT"/2ksbox-*-windows-x64*.msix
"$MAKEAPPX" pack /o /d "$(wpath "$LAYOUT")" /p "$(wpath "$MSIX")" > "$OUT/makeappx.log" 2>&1 \
  || { cat "$OUT/makeappx.log" >&2; echo "package-msix.sh: makeappx failed (see above)" >&2; exit 1; }
[ -s "$MSIX" ] || { echo "package-msix.sh: makeappx wrote no package" >&2; exit 1; }
echo "msix           $MSIX ($(du -h "$MSIX" | cut -f1)), manifest validated by makeappx"

if [ -n "$PFX" ]; then
  SIGNTOOL=${SIGNTOOL:-$(sdk_tool signtool)}
  [ -n "$SIGNTOOL" ] || { echo "package-msix.sh: no signtool.exe (SIGNTOOL=)" >&2; exit 1; }
  "$SIGNTOOL" sign /fd SHA256 /f "$(wpath "$PFX")" ${PFXPW:+/p "$PFXPW"} "$(wpath "$MSIX")" > "$OUT/signtool.log" 2>&1 \
    || { cat "$OUT/signtool.log" >&2; echo "package-msix.sh: signtool failed; the certificate's subject must be exactly $PUBLISHER" >&2; exit 1; }
  echo "signed         with $PFX; install with: Add-AppxPackage '$(wpath "$MSIX")'"
else
  echo "unsigned       a Store upload stays so; a sideload needs --pfx (recipe: docs/build-windows.md)"
fi
