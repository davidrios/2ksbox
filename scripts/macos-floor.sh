#!/usr/bin/env bash
# The oldest macOS a Mac build of 2ksbox runs on, in the form
# MACOSX_DEPLOYMENT_TARGET takes:
#
#   scripts/macos-floor.sh                 12.0
#
# Why this number (docs/build-macos.md, "The floor"): the app carries
# nothing of the Mac's package manager any more (scripts/build-deps.sh
# builds QEMU's libraries and Qt from source, user decision 2026-09-23),
# so the floor is set by what those sources support. Qt 6.9 is the newest
# line that still runs on macOS 12 (6.11 needs 13; 6.5 reached 11 but its
# open-source line ended in 2023). QEMU, glib, pixman, libslirp, zstd, the
# LunarG loader, KosmicKrisp and Rust all go lower. `scripts/build.sh`
# builds everything for this target and `package-macos.sh` fails on any
# file above it. A preset MACOSX_DEPLOYMENT_TARGET wins everywhere, for a
# one-off build.
set -euo pipefail
case "${1:-}" in
  '') echo 12.0 ;;
  *) echo "usage: macos-floor.sh" >&2; exit 2 ;;
esac
