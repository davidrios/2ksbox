#!/usr/bin/env bash
# Build everything this host can build, in dependency order, with the
# per-platform flags:
#
#   scripts/build.sh                 everything this host can build
#   scripts/build.sh qemu rust       only those stages
#   scripts/build.sh --test          everything, then scripts/test.sh host
#
# Stages, in the order they must run:
#
#   qemu    prepare-qemu.sh (overlay + patch queue) -> configure-qemu.sh
#           -> ninja: qemu-system-i386, qemu-img, qemu-io,
#           libqemu-embed-i386.{so,dylib}
#   rust    cargo build --release: player, libdisc/discx, launcher-core
#           (with its `launcherx` verb binary), qemu-embed, shader-chain.
#           Runs after `qemu`, because the player links libqemu-embed from
#           build/qemu. Then `cargo check --release --workspace` keeps the
#           one non-default member, `launcher-capi`, compiling.
#   qt      cargo build --release in launcher-qt/ (its own workspace):
#           the Qt 6 / QML launcher that every package ships (ADR-015).
#           Needs Qt 6 development files. Without them the stage is
#           skipped and this host can build no package.
#   dxvk    prepare-dxvk.sh -> configure-dxvk.sh -> ninja
#   exec    build-d3dpt-exec.sh: libd3dpt_exec, the D3D executor. Runs
#           after `dxvk`, whose headers it compiles against.
#   guest   guest-tools/build-wrappers.sh: the guest-tools ISO (it also
#           calls build-driver.sh for the XP display driver)
#
# A stage whose tools are missing is skipped with the reason, or fails if
# it was named on the command line. The summary at the end lists what
# this host built and what it could not.
#
# A partial rebuild is the project's most expensive mistake. A stale
# libd3dpt_exec or guest-tools ISO after a D3DPT_PROTO_VERSION bump reads
# as "the guest will not boot", not as "you forgot a command".
#
# With everything up to date a run takes a couple of seconds thanks to the
# stamps below, and needs no network.
#
# `launcher-qt/` stays its own cargo workspace (ADR-015), so a plain
# `cargo build` at the root never needs Qt 6. The `rust` stage, the test
# suite and a Mac or CI checkout then work on a host with no Qt.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

case "$(uname -s)" in Darwin) SO=dylib;; *) SO=so;; esac

JOBS=()
RUN_TEST=""
FORCE=""
STAGES=()

usage() {
  sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'
  cat <<EOF

Options:
  -j N            parallel jobs for ninja and cargo (default: auto)
  -f, --force     re-run every prepare and configure step, ignoring the
                  stamps (use it after editing a tree by hand)
  -t, --test      run scripts/test.sh host when the build succeeds
  -h, --help      this text
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    -j) JOBS=(-j "$2"); shift 2 ;;
    -j*) JOBS=(-j "${1#-j}"); shift ;;
    -f|--force) FORCE=1; shift ;;
    -t|--test) RUN_TEST=1; shift ;;
    -h|--help) usage; exit 0 ;;
    qemu|rust|qt|dxvk|exec|guest) STAGES+=("$1"); shift ;;
    *) echo "build.sh: unknown argument '$1' (try --help)" >&2; exit 2 ;;
  esac
done

EXPLICIT=""
if [ ${#STAGES[@]} -eq 0 ]; then
  STAGES=(qemu rust qt dxvk exec guest)
else
  EXPLICIT=1
fi

BUILT=(); SKIPPED=(); T0=$SECONDS

want() { local s; for s in "${STAGES[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }
say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# A stage this host cannot run: an error when it was asked for by name, a
# recorded skip when we were just building everything.
skip() { # stage, reason
  if [ -n "$EXPLICIT" ]; then echo "build.sh: cannot build '$1': $2" >&2; exit 1; fi
  echo "    SKIP $1 - $2"
  SKIPPED+=("$1 ($2)")
  return 1
}

# --- stamps -----------------------------------------------------------
# Every "prepare" step here restores its tree and re-applies a patch
# queue, which hands the build system a few thousand fresh mtimes and a
# from-scratch rebuild every run (four minutes for a tree with nothing to
# do). So each is guarded by a hash of what feeds it: the patch queue, the
# overlaid sources, the submodule commits and the prepare script. Skipping
# a prepare leaves the tree as the last one left it, which the "never git
# checkout inside qemu/ by hand" rule already protects. -f is the escape
# hatch for when that rule was broken.
if have sha256sum; then SHA=(sha256sum); else SHA=(shasum -a 256); fi

stamp_value() { # $STAMP_GITS holds git repos to pin; arguments are paths
  {
    local g
    for g in ${STAMP_GITS:-}; do git -C "$g" rev-parse HEAD 2>/dev/null || echo none; done
    find "$@" -type f 2>/dev/null | LC_ALL=C sort | tr '\n' '\0' | xargs -0 cat 2>/dev/null
  } | "${SHA[@]}" | cut -d' ' -f1
}

# usage: if stamp_stale <name> <paths...>; then <prepare>; stamp_save; fi
stamp_stale() {
  STAMP_FILE="build/.stamp-$1"; shift
  mkdir -p build
  STAMP_VALUE="$(stamp_value "$@")"
  [ -n "$FORCE" ] && return 0
  [ "$(cat "$STAMP_FILE" 2>/dev/null || true)" != "$STAMP_VALUE" ]
}
stamp_save() { printf '%s\n' "$STAMP_VALUE" > "$STAMP_FILE"; }

# macOS: everything is built for the oldest macOS Homebrew still supports
# (scripts/macos-floor.sh), because the app carries Homebrew's libraries
# and runs nowhere older than they do (docs/build-macos.md, "The floor").
# Every stage (configure-qemu.sh, cargo, DXVK, the executor, the wrapper)
# gets the same value, or ld warns "built for newer macOS version" on every
# link. A preset MACOSX_DEPLOYMENT_TARGET wins.
if [ "$(uname -s)" = Darwin ]; then
  MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-$(scripts/macos-floor.sh)}"
  case "$MACOSX_DEPLOYMENT_TARGET" in *.*) ;; *) MACOSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET.0" ;; esac
  export MACOSX_DEPLOYMENT_TARGET
  echo "==> MACOSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET (every stage alike)"
  # The target is not part of cargo's fingerprint, so a tree built for a
  # newer macOS keeps objects that may call what the floor lacks. When a
  # binary was linked for a *newer* macOS than the target, its workspace
  # starts over. Older is fine (without this variable cargo links for
  # rustc's default, 11.0). A workspace this run will not rebuild is left
  # alone, since cleaning it would leave no binary at all (`build.sh guest`
  # once took the player with it).
  for spec in rust:target/release/player qt:launcher-qt/target/release/launcher-qt; do
    bin=${spec#*:}
    want "${spec%%:*}" && [ -f "$bin" ] || continue
    built=$(otool -l "$bin" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')
    if [ -n "$built" ] && [ "$(printf '%s\n' "$built" "$MACOSX_DEPLOYMENT_TARGET" | sort -V | tail -1)" != "$MACOSX_DEPLOYMENT_TARGET" ]; then
      ws=${bin%%target/*}; ws=${ws:-.}
      echo "==> $bin was built for macOS $built: cargo clean --release in $ws"
      (cd "$ws" && cargo clean --release)
    fi
  done
fi

# --- submodules -------------------------------------------------------
# Cloning without --recurse-submodules is the most common first failure.
if [ ! -f qemu/VERSION ] || [ ! -f third_party/qemu-3dfx/00-qemu92x-mesa-glide.patch ]; then
  say "git submodule update --init (qemu, qemu-3dfx)"
  git submodule update --init --depth 1 qemu third_party/qemu-3dfx
fi

# --- qemu -------------------------------------------------------------
if want qemu; then
  if ! have ninja; then skip qemu "no ninja" || true
  else
    say "qemu"
    if STAMP_GITS="qemu third_party/qemu-3dfx" \
       stamp_stale qemu-prepare patches/qemu embed d3dpt/hw d3dpt/d3dpt_proto.h \
         d3dpt/d3dpt_fb.h d3dpt/exec/d3dpt_exec.h libdisc/qemu libdisc/libdisc.h \
         libsynth/qemu libsynth/libsynth.h gamepad/qemu voodoo firmware \
         scripts/prepare-qemu.sh third_party/qemu-3dfx/00-qemu92x-mesa-glide.patch; then
      scripts/prepare-qemu.sh
      stamp_save
    else
      echo "    patch queue, overlays and submodules unchanged - skipping prepare"
    fi

    # QEMU 9.2 carries no slirp of its own, so `-netdev user`, which every
    # machine this project makes uses, exists only if libslirp was there at
    # configure time. Meson's `slirp` option is `auto`, so its absence is
    # silent until a guest has no network and QEMU refuses the launcher's
    # command line.
    slirp_pkg=""; have pkg-config && pkg-config --exists slirp && slirp_pkg=1
    slirp_built=""
    grep -q '^#define CONFIG_SLIRP' build/qemu/config-host.h 2>/dev/null && slirp_built=1

    # configure resets meson options and is slow, so only when needed.
    # prepare-qemu.sh deliberately preserves meson.build mtimes when the
    # content is unchanged, which is what makes this comparison meaningful.
    needs_configure=""
    [ -n "$FORCE" ] && needs_configure=1
    [ -f build/qemu/build.ninja ] || needs_configure=1
    # libslirp installed since the last configure: configure again, or
    # installing it would look like it had done nothing.
    [ -n "$slirp_pkg" ] && [ -z "$slirp_built" ] && [ -f build/qemu/build.ninja ] && needs_configure=1
    for f in qemu/meson.build qemu/hw/3dfx/meson.build qemu/hw/mesa/meson.build; do
      if [ -f "$f" ] && [ -f build/qemu/build.ninja ] && [ "$f" -nt build/qemu/build.ninja ]; then
        needs_configure=1
      fi
    done
    # Configured for another macOS: configure-qemu.sh passes the target as
    # a compiler flag, so configuring again is what recompiles for it.
    if [ "$(uname -s)" = Darwin ] && [ -f build/qemu/config-meson.cross ] \
       && ! grep -q -- "'-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET'" build/qemu/config-meson.cross; then
      needs_configure=1
    fi
    if [ -n "$needs_configure" ]; then
      if [ -z "${QEMU_PYTHON:-}" ] && ! have uv; then
        skip qemu "configure needs uv (or QEMU_PYTHON=<python 3.8-3.13>)" || true
      else
        say "qemu: configure"
        scripts/configure-qemu.sh
      fi
    else
      echo "    build/qemu is configured and no meson file moved - skipping configure"
    fi

    if [ -z "$slirp_pkg" ]; then
      case "$(uname -s)" in
        Darwin) echo "    no libslirp: guests will have no networking (brew install libslirp, then re-run)";;
        *)      echo "    no libslirp: guests will have no networking (install libslirp-dev / libslirp-devel, then re-run)";;
      esac
    fi

    if [ -f build/qemu/build.ninja ]; then
      # block/cdimage.c links libdisc's Rust staticlib (patch 50), and meson
      # takes it from target/release, which the rust stage below writes too
      # late for this link. So build that crate here. Otherwise a pull that
      # only touched libdisc/src leaves qemu-system-i386 on the previous
      # staticlib until the *next* build.sh, and it fails at run time, not
      # at the link ("isodir: I/O error: Is a directory" is what an old
      # libdisc says about a folder disc). libsynth (patch 60) likewise, and
      # a `cargo clean` above for a new macOS target removed both.
      if have cargo; then
        say "qemu: cargo build --release -p libdisc -p libsynth (linked into qemu)"
        cargo build --release -p libdisc -p libsynth ${JOBS[@]+"${JOBS[@]}"}
      fi
      say "qemu: ninja"
      ninja -C build/qemu ${JOBS[@]+"${JOBS[@]}"} \
        qemu-system-i386 qemu-img qemu-io "libqemu-embed-i386.$SO"
      BUILT+=(qemu)
    fi
  fi
fi

# --- rust -------------------------------------------------------------
# After qemu: the player links libqemu-embed with an rpath into
# build/qemu, and qemu-embed/build.rs warns when qemu/embed/ is a stale
# copy of embed/ (which is why prepare-qemu.sh ran first).
if want rust; then
  if ! have cargo; then skip rust "no cargo" || true
  else
    say "rust: cargo build --release (default members)"
    cargo build --release ${JOBS[@]+"${JOBS[@]}"}
    # The member that is not a default member (Cargo.toml):
    # `launcher-capi`, a cdylib + staticlib of the whole launcher that
    # nothing installs. Checked rather than built, so it cannot rot.
    say "rust: cargo check --release --workspace (launcher-capi)"
    cargo check --release --workspace ${JOBS[@]+"${JOBS[@]}"}
    BUILT+=(rust)
  fi
fi

# --- qt ---------------------------------------------------------------
# The launcher every package ships (ADR-015). Its own cargo workspace, so
# it is a stage of its own rather than a member of the one above, which
# keeps Qt 6 off the default build path. There is no CMake step.
# cxx-qt-build finds Qt through `qmake6` and drives moc and
# qmltyperegistrar itself, so the tool to look for is qmake6.
if want qt; then
  if ! have cargo; then skip qt "no cargo" || true
  elif ! have qmake6 && [ -z "${QMAKE:-}" ]; then
    case "$(uname -s)" in
      Darwin) skip qt "no qmake6 (brew install qt)" || true ;;
      *)      skip qt "no qmake6 (qt6-base + qt6-declarative)" || true ;;
    esac
  else
    say "qt: cargo build --release (launcher-qt)"
    ( cd launcher-qt && cargo build --release ${JOBS[@]+"${JOBS[@]}"} )
    BUILT+=(qt)
  fi
fi

# --- dxvk -------------------------------------------------------------
if want dxvk; then
  if ! have meson || ! have ninja; then skip dxvk "needs meson and ninja" || true
  elif ! have glslangValidator && ! have glslang; then
    skip dxvk "needs glslang (vulkan-headers, vulkan-loader, glslang)" || true
  else
    say "dxvk"
    if STAMP_GITS="third_party/dxvk" \
       stamp_stale dxvk-prepare patches/dxvk scripts/prepare-dxvk.sh; then
      scripts/prepare-dxvk.sh
      stamp_save
    else
      echo "    patch queue and submodule unchanged - skipping prepare"
    fi
    # Configured for another macOS: configure-dxvk.sh passes the target as
    # a compiler flag, which build.ninja therefore names.
    dxvk_retarget=""
    if [ "$(uname -s)" = Darwin ] && [ -f build/dxvk/build.ninja ] \
       && ! grep -q -- "-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" build/dxvk/build.ninja; then
      dxvk_retarget=1
    fi
    if [ -n "$FORCE" ] || [ ! -f build/dxvk/build.ninja ] || [ -n "$dxvk_retarget" ]; then
      say "dxvk: configure"
      scripts/configure-dxvk.sh
    fi
    say "dxvk: ninja"
    ninja -C build/dxvk ${JOBS[@]+"${JOBS[@]}"}
    BUILT+=(dxvk)
  fi
fi

# --- exec -------------------------------------------------------------
# Compiles against third_party/dxvk's native headers; the DXVK library
# itself is dlopened at runtime, so this needs the tree prepared, not built.
if want exec; then
  if [ ! -f third_party/dxvk/include/native/windows/windows_base.h ]; then
    skip exec "third_party/dxvk not prepared (run the dxvk stage first)" || true
  else
    say "exec: libd3dpt_exec (the D3D decoder + DXVK executor)"
    scripts/build-d3dpt-exec.sh
    BUILT+=(exec)
  fi
fi

# --- guest ------------------------------------------------------------
# The guest-tools ISO carries the D3DPT guest DLLs, so a protocol bump
# makes it stale exactly as it does the executor. build-wrappers.sh also
# calls build-driver.sh, so the XP display driver rides along.
GUEST_STALE=""
if want guest; then
  # the stamp is computed before the tool check, so a host that cannot
  # build the ISO can still say whether the one it has is out of date
  if STAMP_GITS="third_party/qemu-3dfx" \
     stamp_stale guest-tools guest-tools/src d3dpt/d3dpt_proto.h \
       d3dpt/d3dpt_fb.h cdshelf/cdshelf_proto.h \
       guest-tools/build-wrappers.sh guest-tools/build-driver.sh \
       guest-tools/build-driver9x.sh; then
    GUEST_STALE=1
  fi
  # a stamp is no good without the artifacts it claims are current
  [ -e guest-tools/out/d3dpt-driver.iso ] || GUEST_STALE=1
  ls guest-tools/out/guest-tools-*.iso >/dev/null 2>&1 || GUEST_STALE=1

  if ! have i686-w64-mingw32-gcc; then
    skip guest "needs mingw-w64 (i686-w64-mingw32-gcc)" || true
  elif ! have xorriso && ! have genisoimage && ! have mkisofs; then
    skip guest "needs xorriso (or genisoimage/mkisofs) for the ISO" || true
  elif [ -n "$GUEST_STALE" ]; then
    say "guest: guest-tools ISO + XP display driver"
    guest-tools/build-wrappers.sh
    stamp_save
    GUEST_STALE=""
    BUILT+=(guest)
  else
    say "guest"
    echo "    guest sources, protocol headers and qemu-3dfx unchanged - skipping"
  fi
fi

# --- summary ----------------------------------------------------------
say "summary after $((SECONDS - T0)) s"
if [ ${#BUILT[@]} -gt 0 ]; then printf '    built: %s\n' "${BUILT[*]}"; fi
if [ ${#SKIPPED[@]} -gt 0 ]; then
  printf '    skipped:\n'
  printf '      %s\n' "${SKIPPED[@]}"
fi

# Warn about artifacts left behind by a stage this host could not run. A
# stage that ran refreshed its own output; a skipped one is the case nobody
# looks at. A guest-tools ISO older than the protocol it speaks does not
# announce itself. It reads as an XP guest that will not attach.
stale=()
if [ -n "$GUEST_STALE" ]; then
  for iso in guest-tools/out/*.iso; do
    [ -e "$iso" ] || continue
    stale+=("$iso (guest-tools/build-wrappers.sh)")
  done
fi
for s in ${SKIPPED[@]+"${SKIPPED[@]}"}; do
  case "$s" in
    exec*) [ -f "build/d3dpt/libd3dpt_exec.$SO" ] \
             && stale+=("build/d3dpt/libd3dpt_exec.$SO (scripts/build-d3dpt-exec.sh)") ;;
    # A host with no Qt builds everything except the front end the
    # packages install, and nothing else here would say so.
    qt*) echo "    note: no Qt 6, so no launcher and no package from this host" ;;
  esac
done
if [ ${#stale[@]} -gt 0 ]; then
  printf '\n    WARNING: these are older than the sources they are built from, and\n'
  printf '    this host cannot rebuild them. A stale guest-tools ISO after a\n'
  printf '    D3DPT_PROTO_VERSION bump reads as a guest that will not attach:\n'
  printf '      %s\n' "${stale[@]}"
fi

if [ -n "$RUN_TEST" ]; then
  say "scripts/test.sh host"
  exec scripts/test.sh host
fi

echo
echo "    next: scripts/test.sh        (host stage, ~30 s)"
echo "          scripts/test.sh all    (adds the XP and DOS guests, ~2 min)"
