#!/usr/bin/env bash
# The oldest macOS a Mac build of 2ksbox runs on, which is Homebrew's own
# (HOMEBREW_MACOS_OLDEST_SUPPORTED in its brew.sh), in the form
# MACOSX_DEPLOYMENT_TARGET takes:
#
#   scripts/macos-floor.sh                 14.0
#   scripts/macos-floor.sh --tag [14.0]    arm64_sonoma, Homebrew's bottle tag
#
# Why Homebrew's (docs/build-macos.md, "The floor"): the app carries
# Homebrew's libraries, and Homebrew publishes a bottle of each for every
# macOS it supports and for none before that. So its floor is the lowest
# the app can have, and `scripts/build.sh` builds everything of ours for
# that same version so that nothing we build raises it.
# `scripts/package-macos.sh` then swaps the libraries it copied, which are
# this Mac's bottles, for the floor's (`scripts/macos-bottles.py`).
set -euo pipefail

floor() {
  local repo v
  if repo=$(brew --repository 2>/dev/null); then
    v=$(sed -n 's/^HOMEBREW_MACOS_OLDEST_SUPPORTED="\([0-9][0-9]*\)".*/\1/p' \
          "$repo/Library/Homebrew/brew.sh" 2>/dev/null | head -1)
    if [ -n "$v" ]; then echo "$v.0"; return; fi
    echo "macos-floor.sh: no HOMEBREW_MACOS_OLDEST_SUPPORTED in $repo/Library/Homebrew/brew.sh; using this Mac's macOS" >&2
  else
    echo "macos-floor.sh: no Homebrew; using this Mac's macOS" >&2
  fi
  sw_vers -productVersion | cut -d. -f1,2
}

case "${1:-}" in
  --tag)
    v=${2:-$(floor)}
    major=${v%%.*}
    case "$major" in ''|*[!0-9]*) echo "macos-floor.sh: not a macOS version: $v" >&2; exit 2 ;; esac
    # Homebrew's own name for the release, not a table of ours: a new
    # macOS is a new codename, and brew is what knows it.
    name=$(brew ruby -e "puts MacOSVersion.new('$major').to_sym")
    echo "arm64_$name"
    ;;
  '') floor ;;
  *) echo "usage: macos-floor.sh [--tag [version]]" >&2; exit 2 ;;
esac
