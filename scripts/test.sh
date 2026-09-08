#!/usr/bin/env bash
# The regression suite: integration and end-to-end only (CLAUDE.md policy).
#
#   scripts/test.sh            host stage: every check that needs no guest
#   scripts/test.sh guest      XP on the paravirtual Direct3D device, headless
#   scripts/test.sh all        both
#
# Builds nothing big: it expects build/qemu (qemu-system-i386 +
# libqemu-embed), build/dxvk and build/d3dpt/libd3dpt_exec.* to exist (the
# cheat sheet in docs/00-status.md), and only compiles the small tools in
# tools/. Outputs land in build/test/. A check that cannot run here (no
# x86 host, no display, no image) is reported as SKIP, not as a failure.
#
# Host stage
#   x87-fast       tools/x87-fast-test.c: patch 05's fast path vs the real x87
#   libdisc        discx selftest (doc 17 §6.1): synthetic cue/bin, CCD and ISO
#                  images, the CD model's reads, EDC/ECC, Q synthesis and the
#                  MMC responders checked through libdisc's C API
#   guest-dirdisc  the same tree served as a *folder* (isodir:<dir>): XP copies it
#                  through cdrom.sys and every file matches the directory itself
#   dirdisc        a host directory served as a disc (isodir, M5g): discx generates
#                  the ISO 9660 + Joliet volume over a fixture tree, exports it and
#                  xorriso (or bsdtar) reads the folder back out identical, and
#                  bsdtar with Joliet off reads the 8.3 tree a DOS driver sees;
#                  then
#                  qemu-img reads the same bytes through the block layer and
#                  SeaBIOS probing the drive proves the ATAPI path finds the disc
#                  model (a plain .iso on the raw driver is the control)
#   dirshelf       a shared folder as a disc from the launcher's side: on the shelf
#                  under its own name, in the flat shelf file and on the boot
#                  drive as `isodir:`, commas in the path doubled, and our QEMU
#                  opening both folders
#   qt-wizard      what the Qt wizard's memory field *shows* on each family, beside
#                  what the shared form says: a spin box bounds the value it is
#                  handed against the range it has at that moment, so the control
#                  and the model can disagree and nothing that asks the model
#                  would ever notice (only if a launcher-qt has been built)
#   qt-close       the title bar's close button on a Qt dialog: the close event
#                  delivered the way the window system delivers it must reach
#                  the wizard window exactly once and leave no modal window
#                  registered — a second one is `close()` re-entered from the
#                  window's own hide, which on macOS left the main window locked
#                  behind a dialog that was gone (only if a launcher-qt has
#                  been built)
#   qt-profile     the Qt shader-profile windows, driven: a new profile saved from
#                  the editor has to appear in the list behind it, and the next
#                  New profile… has to come up with an *empty* preset field —
#                  both are things only a probe that asks the controls can see,
#                  because the profile is on disk and the model is empty in the
#                  runs that fail (only if a launcher-qt has been built)
#   shelforder     the disc shelf is one list in one order: discs added in the
#                  wrong order come back by label (case-insensitively, and disc
#                  10 after disc 2), a later addition lands where its name
#                  belongs, and the flat file the in-guest CDSHELF program lists
#                  by slot number carries that same order
#   cdimage        the cdimage block driver (patch 50) through QEMU's block layer:
#                  qemu-img probes the cue and the ccd to "cdimage" with the
#                  lead-out × 2048 as the size, the data track dd'd out equals the
#                  ISO, a plain .iso still probes to raw
#   icons          scripts/gen-icons.sh --check: every checked-in size still
#                  matches packaging/icon/2ksbox.png, the one master
#   package        scripts/package-linux.sh (or package-macos.sh on a Mac): the
#                  install layout staged from this build, and the staged launcher
#                  asked with a scrubbed environment whether
#                  player/qemu-img/firmware/guest-tools all resolve inside the
#                  package (doc 07's install layout); the .app is additionally
#                  run and every image its loader touches must be inside it
#   optimizations  the wizard's fast-path switches (patches/qemu/README.md) from a
#                  checkbox to a real QEMU: a default machine's line unchanged,
#                  each switch on the option QEMU looks it up on, our QEMU
#                  accepting the line the launcher writes
#   mode-sweep     the player's display path without a guest (doc 03, M2): every
#                  mode through mode analysis, the geometry stage and a real CRT
#                  preset — and that the device it opened kept
#                  ADDRESS_MODE_CLAMP_TO_BORDER, without which librashader
#                  samples clamp-to-edge and curved presets smear
#   pointer        the wizard's pointer switch (doc 03's grab model): a new
#                  Windows machine gets the USB tablet and a new DOS machine
#                  does not, the checkbox adds and removes the device and its
#                  controller, and our QEMU accepts both machines
#   family-other   the "Other" family (doc 06): a machine for an era OS that is
#                  neither Windows nor DOS gets standard hardware and none of
#                  ours — the Bochs VGA rather than d3dpt-vga, no network card
#                  until someone asks for one and an ES1370 at its pinned slot,
#                  no USB tablet — the RTL8139 arrives at 0x03 when the box is
#                  ticked, the sound card stays put when the NIC goes, and our
#                  QEMU accepts the line
#   display-adapter the wizard's display-adapter picker (doc 06): each family
#                  offers the adapters it has a driver question about and starts
#                  on the right one, an adapter a family doesn't offer is refused
#                  rather than written, the cards below it don't move when it
#                  changes, and our QEMU accepts every one of them
#   capi           launcher-capi/examples/smoke.c: a third front end, in C, over
#                  the same models the egui and Qt builds use — the wizard's
#                  DOS defaults, the disc shelf, snapshots and the profile
#                  editor, driven through include/launcher_core.h (doc 07)
#   preview-anim   the launcher's shader preview keeps drawing (doc 07): a preset
#                  that stands still says so and renders the same picture at any
#                  frame number, one that does not (an interlaced CRT) says so
#                  and renders two different pictures at two frame numbers
#   embed-3d       tools/embed-3d-test.c: the window-less Mesa backend (Linux)
#   glide-host     tools/glide-host-test.cpp: Glide pass-through without a guest
#                  (Linux) — the real host wrapper loaded by hw/3dfx, opened
#                  through glidewnd.c's handshake, a triangle checked in the
#                  frame the frontend receives, orientation included
#   d3dpt-exec     tools/d3dpt-exec-test.cpp: guest encoder → decoder → DXVK,
#                  frames delivered, hostile batch refused
#   d3dpt-dp2      tools/d3dpt-dp2-test.cpp: the display driver's records (doc 15
#                  M7c): VRAM surfaces, a context, the D3D7TEST scene as DP2
#                  tokens, readback pixels checked, hostile records refused
#   d3dgame9-nat   the reference scene natively on DXVK: frame 300 within
#                  D3D_GOLDEN_BUDGET pixels (tolerance 8, HUD masked) of the
#                  rig golden; this frame is the oracle for the guest stage
#   d3dfeat9-nat   the feature test natively: frame + log lines kept as the
#                  oracle for the guest stage
#
# Guest stage
#   sse-guest      tools/sse-guest-test.py: the SSE battery, sse-fast on/off identical (doc 16)
#   atapi-guest    tools/atapi-guest-test.py: a DOS program drives the ATAPI drive
#                  on the selftest's flipped-sector cue by PIO (patch 51); every
#                  reply identical to discx's at byte-count limits 512 and 65534,
#                  then the disc shelf (patch 52) and a second boot running the
#                  real CDSHELF.COM against it
#   x87-guest      tools/x87-guest-test.py: a DOS x87 battery under TCG,
#                  identical with the fast path on and off (needs nasm,
#                  mtools and the FreeDOS floppy the tool fetches on first use)
#   rep-guest      tools/rep-guest-test.py: a DOS rep movs/stos battery (widths,
#                  address sizes, DF, page crossings, overlaps), rep-fast on/off
#                  identical and equal to a model of the instruction (patch 17)
#   smc-guest      tools/smc-guest-test.py: self-modifying code (patched immediates,
#                  same-value rewrites, opcode flips, a crossing store), smc-same-value
#                  on/off both architecturally right (patch 18)
#   guest-cdimage  tools/xp-cdimage-test.sh: XP boots with the guest-tools ISO
#                  converted to a cue (+ a 1 kHz tone track) as its CD-ROM and
#                  copies the whole disc through cdrom.sys; every file must
#                  match the ISO's; with mingw, CDTEST.EXE then plays the tone
#                  through MCI and the drive's audiodev (a wav) must carry it
#   XP (Linux; KVM when /dev/kvm is usable, TCG otherwise):
#   boots WINXP_IMG read-only (snapshot=on) with the newest guest-tools ISO
#   and a fresh FAT32 scratch disk carrying RUN.BAT, drives the Run dialog
#   over QMP, waits for the three programs to detach from the device, shuts
#   XP down (DDVMTEST first: the DirectDraw shim's video-memory answer), and
#   diffs: D3DGAME9 and D3DGAME8 pixel-identical to the native
#   D3DGAME9 frame outside the wall-time HUD (and within budget of the rig
#   golden), D3DFEAT9 byte-identical to the native frame with the same
#   query / getter lines.
#
# Environment: WINXP_IMG (~/vms/winxp.qcow2), GUEST_ISO (newest
# guest-tools/out/guest-tools-3dfx-*.iso), TEST_ACCEL (kvm|tcg),
# D3D_GOLDEN_BUDGET (1200), TEST_BOOT_TIMEOUT (300 s), TEST_KEEP=1 keeps
# the VM running on failure for a look (QMP socket printed).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="$ROOT/build/test"; mkdir -p "$OUT"
STAGE="${1:-host}"
OS="$(uname -s)"; ARCH="$(uname -m)"
DX="$ROOT/third_party/dxvk/include/native"
GOLDEN="$ROOT/reference/d3d/rig-2026-09-03/d3dgame9-w300-ff.bmp"
HUD_MASK="0,368,270,112"
BUDGET="${D3D_GOLDEN_BUDGET:-1200}"
case "$OS" in Darwin) SO=dylib;; *) SO=so;; esac
export D3DPT_EXEC_LIB="${D3DPT_EXEC_LIB:-$ROOT/build/d3dpt/libd3dpt_exec.$SO}"
export D3DPT_DXVK_LIB="${D3DPT_DXVK_LIB:-$ROOT/build/dxvk/src/d3d9/libdxvk_d3d9.$SO$([ "$SO" = so ] && echo .0)}"
if [ "$OS" = Darwin ]; then
  # DXVK dlopens the Vulkan loader by leaf name; a DYLD_* variable handed
  # to this script is stripped by SIP at the `#!/usr/bin/env` exec, so set
  # the documented macOS run environment here (docs/build-macos.md,
  # patches/dxvk/README.md): Homebrew's loader, and the LunarG SDK's
  # KosmicKrisp ICD unless the caller chose one.
  export DYLD_LIBRARY_PATH="/opt/homebrew/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
  if [ -z "${VK_ICD_FILENAMES:-}" ]; then
    for f in "$HOME"/VulkanSDK/*/macOS/share/vulkan/icd.d/libkosmickrisp_icd.json; do
      [ -f "$f" ] && export VK_ICD_FILENAMES="$f"
    done
  fi
fi

PASS=(); FAIL=(); SKIP=()
log() { printf '\n==> %s\n' "$*"; }
skip() { SKIP+=("$1: $2"); printf '  SKIP %s (%s)\n' "$1" "$2"; }
# GNU coreutils' timeout(1) is not on a Mac (nor is gtimeout unless
# someone installed coreutils), and a check that hangs is worse than one
# that fails, so stand one in.
if ! command -v timeout >/dev/null; then
  if command -v gtimeout >/dev/null; then
    timeout() { gtimeout "$@"; }
  else
    timeout() { # seconds, command...
      local s="$1" p w rc; shift
      "$@" & p=$!
      # The watchdog gets none of the command's descriptors: a caller
      # reading the output through a pipe (`o="$(timeout … | sed …)"`)
      # waits for every writer to close it, so a watchdog that inherited
      # stdout held the pipe for the whole limit and every Qt check took
      # its full 120 s on a Mac without coreutils (2026-09-07). Afterwards
      # the sleep is killed with the subshell, or it lives on orphaned.
      ( sleep "$s"; kill -9 "$p" 2>/dev/null ) >/dev/null 2>&1 </dev/null & w=$!
      wait "$p"; rc=$?
      pkill -P "$w" 2>/dev/null; kill "$w" 2>/dev/null
      return $rc
    }
  fi
fi

run_check() { # name, log file, command...
  local name="$1" lf="$OUT/$2"; shift 2
  "$@" >"$lf" 2>&1; local rc=$?
  if [ $rc = 0 ]; then PASS+=("$name"); printf '  PASS %s\n' "$name"; return 0; fi
  if [ $rc = 77 ]; then skip "$name" "$(tail -1 "$lf")"; return 0; fi
  FAIL+=("$name"); printf '  FAIL %s (exit %d) — %s\n' "$name" $rc "$lf"; tail -5 "$lf" | sed 's/^/       /'; return 1
}
dirdisc_check() { # a host directory served as a disc, read back by someone else's ISO 9660 reader
  # discx's own dirdisc case (the libdisc check) proves the model reads
  # the tree back; this one proves the *volume* is one, by handing it to
  # a reader that is not ours and diffing the result against the folder.
  local src="$OUT/dirsrc" ext="$OUT/dirsrc-out" iso="$OUT/dirsrc.iso" rc=0
  # QEMU is given absolute paths: it does not run from here, and $OUT may
  # or may not be absolute already.
  local abs="$src" absiso="$iso"
  case "$abs" in /*) ;; *) abs="$PWD/$abs"; absiso="$PWD/$absiso";; esac
  target/release/discx mktree "$src" || { echo "mktree failed"; return 1; }
  target/release/discx export "isodir:$src" "$iso" >/dev/null || { echo "export failed"; return 1; }
  [ -d "$ext" ] && chmod -R u+w "$ext"; rm -rf "$ext"; mkdir -p "$ext"
  # Both readers are independent of us; xorriso is preferred only because
  # libarchive rewrites names to NFD on macOS, which no guest does.
  local extra=()
  if command -v xorriso >/dev/null; then
    xorriso -osirrox on -indev "$iso" -extract / "$ext" >"$OUT/dirdisc-extract.log" 2>&1 || { echo "xorriso could not read the volume"; return 1; }
  elif command -v bsdtar >/dev/null; then
    bsdtar -xf "$iso" -C "$ext" >"$OUT/dirdisc-extract.log" 2>&1 || { echo "bsdtar could not read the volume"; return 1; }
    [ "$OS" = Darwin ] && extra=(-x 'caf*')
  else
    echo "needs xorriso or bsdtar"; return 77
  fi
  chmod -R u+w "$ext"
  # The two names Joliet cannot hold are excluded here and checked below:
  # everything else must come back exactly as it went in.
  diff -r -x 'semi*' -x 'star*' "${extra[@]}" "$src" "$ext" || { echo "the folder did not come back identical"; rc=1; }
  cmp -s "$src/semi;colon.txt" "$ext/semi_colon.txt" || { echo "semi;colon.txt is not there as semi_colon.txt"; rc=1; }
  cmp -s "$src/star*name.txt" "$ext/star_name.txt" || { echo "star*name.txt is not there as star_name.txt"; rc=1; }

  # The *other* tree: ISO 9660 level 1, which is what a real-mode DOS
  # driver reads and what Windows falls back to. bsdtar with Joliet
  # turned off is the only reader here that will look at it, and the two
  # colliding names are the point — a mangling that crossed their
  # contents would pass every check that only counts files.
  if command -v bsdtar >/dev/null; then
    local dos="$OUT/dirsrc-83"
    [ -d "$dos" ] && chmod -R u+w "$dos"; rm -rf "$dos"; mkdir -p "$dos"
    if bsdtar -xf "$iso" -C "$dos" --options 'iso9660:!joliet,iso9660:!rockridge' 2>>"$OUT/dirdisc-extract.log"; then
      chmod -R u+w "$dos"
      for n in EMPTY.BIN README.TXT PROGRAM_.TXT CAF_.TXT SEMI_COL.TXT STAR_NAM.TXT COLLISIO.TXT COLLIS~1.TXT LLLLLLLL.TXT EXACT204.BIN ODD2049.BIN; do
        [ -f "$dos/$n" ] || { echo "the 8.3 tree has no $n"; rc=1; }
      done
      [ -d "$dos/EMPTY_DI" ] || { echo "the 8.3 tree has no EMPTY_DI directory"; rc=1; }
      grep -q '^one$' "$dos/COLLISIO.TXT" 2>/dev/null || { echo "COLLISIO.TXT is not collision-one.txt"; rc=1; }
      grep -q '^two$' "$dos/COLLIS~1.TXT" 2>/dev/null || { echo "COLLIS~1.TXT is not collision-two.txt"; rc=1; }
      cmp -s "$src/big.bin" "$dos/BIG.BIN" || { echo "BIG.BIN differs in the 8.3 tree"; rc=1; }
    else
      echo "bsdtar could not read the primary tree"; rc=1
    fi
  fi

  # The block layer: the same bytes through QEMU's own read path.
  if [ -x build/qemu/qemu-img ]; then
    local info; info="$(build/qemu/qemu-img info --output=json "isodir:$abs" 2>/dev/null)"
    echo "$info" | grep -q '"format": "isodir"' || { echo "not opened by the isodir driver"; echo "$info"; rc=1; }
    build/qemu/qemu-img convert -O raw "isodir:$abs" "$OUT/dirsrc-qemu.iso" 2>/dev/null \
      && cmp -s "$OUT/dirsrc-qemu.iso" "$iso" || { echo "qemu-img read the folder differently from discx"; rc=1; }
  fi

  # The drive: the ATAPI path has to find the disc model through whatever
  # node graph the block layer built — a protocol driver reached by its
  # filename prefix ends up under a probed `raw` format node, and a
  # cdimage_disc() that misses it fails silently, leaving the guest with
  # QEMU's stock answers. CDIMAGE_TRACE prints a line per packet only
  # when the model is there, so SeaBIOS probing the drive is the proof;
  # the same run on a plain .iso (the raw driver, no model) is the control.
  if [ -x build/qemu/qemu-system-i386 ]; then
    local n c
    n="$(atapi_disc_packets "isodir:$abs" "$OUT/dirdisc-probe.log")"
    c="$(atapi_disc_packets "$absiso" "$OUT/dirdisc-control.log")"
    [ "${n:-0}" -gt 0 ] || { echo "the drive saw no disc model for the folder (cdimage_disc found nothing)"; rc=1; }
    [ "${c:-0}" = 0 ] || { echo "the trace fired for a plain .iso on the raw driver: it proves nothing"; rc=1; }
  fi
  return $rc
}

atapi_disc_packets() { # disc, log -> packets the cdimage disc path saw while SeaBIOS probed the drive
  local disc="$1" out="$2" pid i
  # Both logs are markers this function waits on: a stale one from an
  # earlier run reads as "already finished" and the machine gets killed
  # before it has probed anything.
  rm -f "$out" "$out.bios"
  CDIMAGE_TRACE=1 build/qemu/qemu-system-i386 -L qemu/pc-bios -machine pc -m 64 \
    -display none -net none -boot d -no-reboot \
    -debugcon "file:$out.bios" -global isa-debugcon.iobase=0x402 \
    -drive "if=none,id=cd0,media=cdrom,file=$disc" \
    -device ide-cd,bus=ide.1,id=ide1-cd0,drive=cd0 >"$out" 2>&1 &
  pid=$!
  # SeaBIOS says this once it has probed every drive; it takes well under
  # a second, and the machine would otherwise sit there with no disk.
  for i in $(seq 1 40); do
    grep -q "No bootable device" "$out.bios" 2>/dev/null && break
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.25
  done
  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  grep -c "atapi-disc:" "$out" 2>/dev/null || true
}

cdimage_check() { # the block driver through qemu-img / qemu-io on the selftest images
  local d="$OUT/disc" want=$((6800 * 2048)) rc=0
  for f in mixed.cue mixed.ccd cooked.cue; do
    local info; info="$(build/qemu/qemu-img info --output=json "$d/$f")" || { echo "$f: qemu-img info failed"; return 1; }
    echo "$info" | grep -q '"format": "cdimage"' || { echo "$f: not probed as cdimage"; echo "$info"; rc=1; }
    echo "$info" | grep -q "\"virtual-size\": $want" || { echo "$f: virtual size is not $want"; echo "$info"; rc=1; }
  done
  build/qemu/qemu-img info --output=json "$d/plain.iso" | grep -q '"format": "raw"' || { echo "plain.iso: no longer probes to raw"; rc=1; }
  build/qemu/qemu-img dd -f cdimage -O raw bs=2048 count=2000 "if=$d/mixed.cue" "of=$OUT/cdimage-dd.bin" || rc=1
  cmp "$OUT/cdimage-dd.bin" "$d/plain.iso" || { echo "the data track through the block layer differs from plain.iso"; rc=1; }
  local o
  o="$(build/qemu/qemu-io -r -c "read $((1000 * 2048)) 2048" "$d/lec.cue" 2>&1)"
  case "$o" in *"read failed"*) ;; *) echo "the flipped sector read cleanly"; rc=1;; esac
  o="$(build/qemu/qemu-io -r -c "read $((2000 * 2048)) 2048" "$d/mixed.cue" 2>&1)"
  case "$o" in *"read failed"*) ;; *) echo "an audio sector read as data"; rc=1;; esac
  [ "$(nm -D build/qemu/libqemu-embed-i386.$SO 2>/dev/null | grep -c ' T _ZN3std')" = 0 ] || { echo "Rust std symbols exported from the embed library"; rc=1; }
  return $rc
}
host_check_probe() { # `launcher --host-check` (ADR-013), on any host
  local rc=0 o
  # This host's own answer: either verdict is legal, the report is not.
  o="$(target/release/launcherx --host-check 2>&1)" || true
  case "$o" in *"Vulkan loader:"*) ;; *) echo "the report names no loader"; echo "$o"; rc=1;; esac
  case "$o" in *"Required: a 1.3 device"*) ;; *) echo "the report names no bar"; echo "$o"; rc=1;; esac
  # A host with no Vulkan driver at all, which every host can be made
  # into: both loader variables, since which one is read depends on how
  # old the loader is.
  o="$(VK_DRIVER_FILES=/nonexistent.json VK_ICD_FILENAMES=/nonexistent.json \
       target/release/launcherx --host-check 2>&1)" \
    && { echo "exit 0 with no Vulkan driver"; rc=1; }
  case "$o" in *unavailable*) ;; *) echo "no Vulkan driver, yet not reported unavailable"; echo "$o"; rc=1;; esac
  case "$o" in *WineD3D*) ;; *) echo "no Vulkan driver, yet not pointed at WineD3D"; echo "$o"; rc=1;; esac
  # Where lavapipe is installed, the other half is testable for real: a
  # software driver is *usable* (DXVK ranks a CPU device last but never
  # excludes it), so the verdict is available and the warning is that it
  # will be slow — never a refusal (ADR-013).
  local lvp; lvp="$(ls /usr/share/vulkan/icd.d/lvp_icd*.json 2>/dev/null | head -1)"
  if [ -n "$lvp" ]; then
    o="$(VK_DRIVER_FILES="$lvp" target/release/launcherx --host-check 2>&1)" \
      || { echo "a software driver was refused instead of warned about"; echo "$o"; rc=1; }
    case "$o" in *slow*) ;; *) echo "a software driver was not called slow"; echo "$o"; rc=1;; esac
    case "$o" in *"software, usable but slow"*) ;; *) echo "the software device was not listed as usable"; echo "$o"; rc=1;; esac
  fi
  return $rc
}
shelforder_check() { # the disc shelf is in order by label, all the way to the guest
  local rc=0 dir="$OUT/shelforder" bundle o shelf_file got want
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  bundle="$(target/release/launcherx --new xp shelved "$dir/disk.qcow2")" || { echo "--new failed"; return 1; }
  # Added in an order nobody would want to read them in: mixed case, and a
  # numbered set whose tenth disc a string sort files between 1 and 2.
  for f in "zork.iso" "blood disc 10.cue" "Blood disc 2.cue" "aladdin.iso" "Blood disc 1.cue"; do
    : >"$dir/$f"
  done
  o="$(target/release/launcherx --discs add "$dir/zork.iso" "$dir/blood disc 10.cue" \
        "$dir/Blood disc 2.cue" "$dir/aladdin.iso" "$dir/Blood disc 1.cue")" \
    || { echo "--discs add failed"; rc=1; }
  want="aladdin Blood disc 1 Blood disc 2 blood disc 10 zork"
  got="$(printf '%s\n' "$o" | cut -f1 | tr '\n' ' ')"
  case "$got" in "$want "*) ;; *) echo "the shelf is not in order by label"; echo "  got:  $got"; echo "  want: $want"; rc=1;; esac
  # And the same order in the flat file the guest's own CDSHELF program
  # lists (patch 52) — it is served by slot number, so the order the host
  # writes is the order the guest shows and the numbers a guest loads by.
  shelf_file="$(target/release/launcherx --discs publish "$(dirname "$bundle")" \
                | sed -n 's/^shelf published to //p')"
  if [ -n "$shelf_file" ] && [ -f "$shelf_file" ]; then
    got="$(cut -f1 "$shelf_file" | tr '\n' ' ')"
    case "$got" in "$want "*) ;; *) echo "the guest's shelf file is not in order by label"; echo "  got: $got"; rc=1;; esac
  else
    echo "no shelf file was published"; rc=1
  fi
  # A disc added later lands where its name belongs, not at the end.
  : >"$dir/Age of Empires.iso"
  o="$(target/release/launcherx --discs add "$dir/Age of Empires.iso" | cut -f1 | head -1)"
  [ "$o" = "Age of Empires" ] || { echo "a disc added later did not land in order (first row: $o)"; rc=1; }
  return $rc
}
qtwizard_check() { # what the Qt wizard's memory field *shows* (doc 07)
  local rc=0 dir="$OUT/qtwizard" bin="launcher-qt/target/release/launcher-qt" f o shown model lo hi
  rm -rf "$dir"; mkdir -p "$dir/library"
  # A scratch library, never the user's own — the window lists it on the
  # way up. Offscreen, so a check never throws a window on the desktop.
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles" QT_QPA_PLATFORM=offscreen
  # The model is right and the control disagrees is a whole class of Qt
  # bug (a spin box bounds the value it is handed against the range it
  # has at that moment, and does not revisit it when the range widens),
  # and it is invisible to everything that asks the model — which is what
  # every other launcher check does. So this asks the *window*: it opens
  # the real wizard headlessly on each family and prints what its memory
  # field holds beside what the form says it should.
  for f in win98 xp dos other; do
    o="$(timeout 120 env LAUNCHER_QT_SCREEN=wizard LAUNCHER_QT_ARG="$f" LAUNCHER_QT_DELAY=250 \
         "$bin" 2>&1 | sed -n 's/^\[diag\] wizard memory: //p')"
    if [ -z "$o" ]; then echo "$f: the wizard printed no memory line"; rc=1; continue; fi
    shown="$(printf '%s' "$o" | sed -n 's/^shown \([0-9]*\).*/\1/p')"
    model="$(printf '%s' "$o" | sed -n 's/.*model \([0-9]*\).*/\1/p')"
    lo="$(printf '%s' "$o" | sed -n 's/.*range \([0-9]*\)\.\..*/\1/p')"
    hi="$(printf '%s' "$o" | sed -n 's/.*range [0-9]*\.\.\([0-9]*\).*/\1/p')"
    [ "$shown" = "$model" ] || { echo "$f: the memory field shows $shown, the form says $model"; rc=1; }
    [ "$model" -ge "$lo" ] && [ "$model" -le "$hi" ] \
      || { echo "$f: $model is outside the family's own range $lo..$hi"; rc=1; }
    echo "  $f: $o"
  done
  return $rc
}
qtclose_check() { # the title bar's close button on a Qt dialog (doc 07)
  local dir="$OUT/qtclose" bin="launcher-qt/target/release/launcher-qt" o n modal
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles" QT_QPA_PLATFORM=offscreen
  # Cancel calls `close()`, which Qt guards against re-entry; the title
  # bar's button hands Qt a close *event*, which it does not. A window
  # whose `visibleChanged` clears a model flag that in turn calls
  # `close()` re-enters from inside the first event and gets a second —
  # and the platform hide that ends the modal session on macOS is
  # skipped. The probe sends the event and counts what the window saw.
  o="$(timeout 120 env LAUNCHER_QT_SCREEN=closebox LAUNCHER_QT_DELAY=250        "$bin" 2>&1 | sed -n 's/^\[diag\] closebox: //p')"
  [ -n "$o" ] || { echo "the probe printed no closebox line"; return 1; }
  echo "  $o"
  n="$(printf '%s' "$o" | sed -n 's/^\([0-9]*\) close events.*/\1/p')"
  modal="$(printf '%s' "$o" | sed -n 's/.*modal left=\(-*[0-9]*\).*/\1/p')"
  [ "$n" = 1 ] || { echo "the wizard window saw $n close events for one click; close() re-entered from its own hide"; return 1; }
  [ "$modal" = 0 ] || { echo "a modal window is still registered after the close (modal left=$modal)"; return 1; }
  printf '%s' "$o" | grep -q "open=false, visible=false" \
    || { echo "the wizard's flag or window did not follow the close: $o"; return 1; }
  return 0
}
qtprofile_check() { # the Qt shader-profile windows, driven (doc 07)
  local rc=0 dir="$OUT/qtprofile" bin="launcher-qt/target/release/launcher-qt" o list shown
  rm -rf "$dir"; mkdir -p "$dir/library" "$dir/profiles"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles" QT_QPA_PLATFORM=offscreen
  # Like `qt-wizard`, this asks the *windows*. Both failures it guards
  # against left the model right and the screen wrong (user-reported,
  # 2026-09-07): the editor's Save handler reached for the list window's
  # own `profiles` model, which is not a property of the editor window,
  # and the TypeError took the `changed()` beside it with it — so the
  # profile was written and the list behind it never heard. And the
  # preset field wrote its own bound property, which destroys the
  # binding that feeds it, so a fresh profile's empty path never reached
  # the field. The preset does not have to exist: saving a profile
  # stores the path, and an unreadable one is a parse error the editor
  # shows rather than a refusal.
  o="$(timeout 120 env LAUNCHER_QT_SCREEN=saveprofile LAUNCHER_QT_ARG="$dir/crt.slangp" \
       LAUNCHER_QT_DELAY=300 "$bin" 2>&1 | sed -n 's/^\[diag\] saveprofile: //p')"
  [ -n "$o" ] || { echo "the probe printed no saveprofile line"; return 1; }
  printf '%s\n' "$o" | sed 's/^/  /'
  list="$(printf '%s' "$o" | sed -n 's/.*list \([0-9]*\) -> \([0-9]*\).*/\1 \2/p')"
  [ "$list" = "0 1" ] || { echo "the saved profile did not reach the list (list $list)"; rc=1; }
  printf '%s' "$o" | grep -q "editor open=false" \
    || { echo "the editor stayed open after a successful save"; rc=1; }
  ls "$dir/profiles"/*.toml >/dev/null 2>&1 || { echo "no profile was written at all"; rc=1; }
  shown="$(printf '%s' "$o" | sed -n "s/.*fresh preset field '\([^']*\)'.*/\1/p")"
  [ -z "$shown" ] || { echo "New profile… still shows the last preset ($shown)"; rc=1; }
  return $rc
}
dirshelf_check() { # a shared folder as a disc, from the shelf to a real QEMU (M5g)
  local rc=0 dir="$OUT/dirshelf" bundle args o spaced comma plain shelf_file
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  # Three folders, because the awkward parts of a folder name are what
  # this is about: a space, a comma (which is what separates options in a
  # QEMU option string), and one plain one to compare against. They go
  # through the launcher's own headless verbs — the code the shelf
  # window's buttons run.
  spaced="$dir/Shared Files"; mkdir -p "$spaced"; echo hello > "$spaced/README.TXT"
  comma="$dir/Doom,Quake"; mkdir -p "$comma"; echo hi > "$comma/GAME.TXT"
  plain="$dir/patch13"; mkdir -p "$plain"; echo p > "$plain/PATCH.TXT"
  : >"$dir/disk.qcow2"
  bundle="$(target/release/launcherx --new xp folders "$dir/disk.qcow2")" || { echo "--new failed"; return 1; }

  # On the shelf a folder is labelled by its own name, extension and all
  # (`patch13` would lose its .3 to a file stem).
  o="$(target/release/launcherx --discs add "$spaced" "$comma" "$plain")" || { echo "--discs add failed"; rc=1; }
  for want in "Shared Files	$spaced" "Doom,Quake	$comma" "patch13	$plain"; do
    case "$o" in *"$want"*) ;; *) echo "not on the shelf under its own name: $want"; echo "$o"; rc=1;; esac
  done

  # The flat file the guest's own CDSHELF program reads (patch 52) names a
  # folder the way QEMU has to be told to open one, and only that way.
  shelf_file="$(target/release/launcherx --discs publish "$(dirname "$bundle")" \
                | sed -n 's/^shelf published to //p')"
  if [ -n "$shelf_file" ] && [ -f "$shelf_file" ]; then
    grep -q "	isodir:$spaced\$" "$shelf_file" || { echo "the shelf file does not name the folder as isodir:"; cat "$shelf_file"; rc=1; }
    grep -q "	$spaced\$" "$shelf_file" && { echo "the shelf file names the folder as a plain path too"; rc=1; }
  else
    echo "no shelf file was published"; rc=1
  fi

  # The boot drive: the prefix, and a comma written twice so that the
  # option string survives being parsed.
  target/release/launcherx --boot-disc "$bundle" "$spaced" >/dev/null || { echo "--boot-disc failed"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *"file=isodir:$spaced "*) ;; *) echo "the boot drive does not name the folder as isodir:"; echo "$args"; rc=1;; esac
  target/release/launcherx --boot-disc "$bundle" "$comma" >/dev/null || { echo "--boot-disc (comma) failed"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *"file=isodir:$dir/Doom,,Quake "*) ;; *) echo "the comma in the path is not doubled"; echo "$args"; rc=1;; esac

  # And the point of all of it: our QEMU opens a folder as a disc, and the
  # doubled comma reaches it as one path rather than an unknown option.
  # Only the space-free folders are run: this check has to re-split a flat
  # command line in the shell, which the launcher itself never does (it
  # spawns an argv), so a space in a path is the harness's limit and not
  # the product's.
  if [ -x build/qemu/qemu-system-i386 ] && [ -x build/qemu/qemu-img ]; then
    build/qemu/qemu-img create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    for d in "$plain" "$comma"; do
      target/release/launcherx --boot-disc "$bundle" "$d" >/dev/null || rc=1
      args="$(target/release/launcherx --print-args "$bundle")"
      # shellcheck disable=SC2086
      o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"quit"}\n' \
           | timeout 30 build/qemu/qemu-system-i386 $args \
               -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
        || { echo "our QEMU refused the folder $d"; echo "$o" | tail -3; rc=1; }
    done
  else
    echo "  (no build/qemu: the command line was checked but not run)"
  fi
  return $rc
}

pointer_check() { # the wizard's pointer switch, from a checkbox to a real QEMU
  local rc=0 dir="$OUT/pointer" bundle dos args o
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  bundle="$(target/release/launcherx --new xp pointer "$dir/disk.qcow2")" || { echo "--new failed"; return 1; }
  dos="$(target/release/launcherx --new dos pointer-dos "$dir/disk.qcow2")" || { echo "--new dos failed"; return 1; }
  # A Windows machine gets the tablet, which is what "no grab" is made
  # of; a DOS machine does not, because its mouse drivers read the PS/2
  # controller and would find no pointer at all.
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *"-device usb-tablet"*) ;; *) echo "a new XP machine has no tablet"; echo "$args"; rc=1;; esac
  args="$(target/release/launcherx --print-args "$dos")"
  case "$args" in *usb*) echo "a new DOS machine has a tablet it cannot read"; echo "$args"; rc=1;; esac
  # The switch itself, through the real form: the tablet goes, and the
  # controller goes with it rather than staying behind with nothing on it.
  target/release/launcherx --wizard-edit "$bundle" - - - - - - noseamless >/dev/null \
    || { echo "--wizard-edit noseamless failed"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *usb*) echo "turning the seamless mouse off left USB behind"; echo "$args"; rc=1;; esac
  target/release/launcherx --wizard-edit "$bundle" - - - - - - seamless >/dev/null \
    || { echo "--wizard-edit seamless failed"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *"-device usb-tablet"*) ;; *) echo "turning it back on did not restore the tablet"; echo "$args"; rc=1;; esac
  # And the point of it: our QEMU accepts both machines. Started paused
  # on the real binary and told to quit, so a refused device is an exit
  # code rather than a hung guest.
  if [ -x build/qemu/qemu-system-i386 ] && [ -x build/qemu/qemu-img ]; then
    build/qemu/qemu-img create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    for b in "$bundle" "$dos"; do
      args="$(target/release/launcherx --print-args "$b")"
      # shellcheck disable=SC2086
      o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"quit"}\n' \
           | timeout 30 build/qemu/qemu-system-i386 $args \
               -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
        || { echo "our QEMU refused $b"; echo "$o" | tail -3; rc=1; }
    done
  else
    echo "  (no build/qemu: the command line was checked but not run)"
  fi
  return $rc
}

family_other_check() { # the "Other" family's hardware, from the picker to a real QEMU
  local rc=0 dir="$OUT/family-other" bundle args o
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  bundle="$(target/release/launcherx --new other beos "$dir/disk.qcow2")" || { echo "--new other failed"; return 1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  # Standard hardware, and specifically *not* ours: `d3dpt-vga` needs the
  # display driver from the guest-tools ISO, which is a Windows driver, so
  # a BeOS or Linux guest on it would come up with no display at all.
  case "$args" in *"-vga std"*) ;; *) echo "an Other machine is not on the standard VGA"; echo "$args"; rc=1;; esac
  case "$args" in *d3dpt-vga*) echo "an Other machine got our own adapter, which has no driver for it"; echo "$args"; rc=1;; esac
  # No card at all until someone asks for one — the default for every
  # family since 2026-09-07 (`bundle::default_network`): these guests
  # stopped getting security fixes twenty years ago, so a machine nobody
  # has been asked about is off the network.
  case "$args" in *rtl8139*|*-netdev*) echo "a new machine came with a network card"; echo "$args"; rc=1;; esac
  case "$args" in *"-nic none"*) ;; *) echo "networking off did not emit -nic none, so QEMU supplies a card of its own"; echo "$args"; rc=1;; esac
  case "$args" in *"ES1370,audiodev=embed0,addr=0x04"*) ;; *) echo "no ES1370 at 0x04"; echo "$args"; rc=1;; esac
  # And the card the checkbox turns on is doc 06's, in its own slot.
  target/release/launcherx --wizard-edit "$bundle" - - - net >/dev/null || { echo "--wizard-edit net failed"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *"rtl8139,netdev=n0,addr=0x03"*) ;; *) echo "no RTL8139 at 0x03"; echo "$args"; rc=1;; esac
  # The tablet is off here by default (`default_seamless_mouse`): an
  # absolute pointer needs the guest to agree it is absolute, and these
  # guests get no guest-tools install to make them.
  case "$args" in *usb*) echo "a new Other machine has a tablet it may not be able to read"; echo "$args"; rc=1;; esac
  # And the reason the addresses are written out: removing the NIC must
  # not slide the sound card up into its slot, which an installed guest
  # would see as its card having been swapped.
  target/release/launcherx --wizard-edit "$bundle" - - - nonet >/dev/null || { echo "--wizard-edit nonet failed"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *rtl8139*) echo "turning networking off left the NIC behind"; echo "$args"; rc=1;; esac
  case "$args" in *"ES1370,audiodev=embed0,addr=0x04"*) ;; *) echo "the sound card moved when the NIC went"; echo "$args"; rc=1;; esac
  target/release/launcherx --wizard-edit "$bundle" - - - net >/dev/null || { echo "--wizard-edit net failed"; rc=1; }
  # Started paused on the real binary and told to quit, so a device our
  # QEMU does not have is an exit code rather than a hung guest.
  if [ -x build/qemu/qemu-system-i386 ] && [ -x build/qemu/qemu-img ]; then
    build/qemu/qemu-img create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    args="$(target/release/launcherx --print-args "$bundle")"
    # shellcheck disable=SC2086
    o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"quit"}\n' \
         | timeout 30 build/qemu/qemu-system-i386 $args \
             -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
      || { echo "our QEMU refused the Other machine"; echo "$o" | tail -3; rc=1; }
  else
    echo "  (no build/qemu: the command line was checked but not run)"
  fi
  return $rc
}

# The command line an adapter name lands as (`bundle::Video::args`), so
# the checks below can name the pick rather than repeat its arguments.
vga_args() {
  case "$1" in
    d3dpt) echo "-device d3dpt-vga,addr=0x02";;
    *)     echo "-vga $1";;
  esac
}

display_adapter_check() { # the wizard's adapter picker, from a combo box to a real QEMU
  local rc=0 dir="$OUT/display-adapter" bundle args o f want other first
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  # What each family starts on. XP on our own adapter, because the whole
  # display path is built on it (doc 15); Win98 on the Cirrus and the
  # driver Windows has in the box, ours there being much the newer of the
  # two (doc 19); Other on the standard VGA, the one every guest can fall
  # back on; DOS on the era's Cirrus, which is not a choice at all.
  for f in win98:"-vga cirrus" xp:"-device d3dpt-vga,addr=0x02" other:"-vga std" dos:"-vga cirrus"; do
    want="${f#*:}"; f="${f%%:*}"
    bundle="$(target/release/launcherx --new "$f" "adapter-$f" "$dir/disk.qcow2")" || { echo "--new $f failed"; return 1; }
    args="$(target/release/launcherx --print-args "$bundle")"
    case "$args" in *"$want"*) ;; *) echo "a new $f machine is not on $want"; echo "$args"; rc=1;; esac
  done
  # The switch itself, on a Windows machine: away from the adapter the
  # family starts on and back again — which is a different direction on
  # each of them, since 98 starts on the Cirrus and XP on ours. The one
  # it left must be *gone* — a machine with both would show the guest two
  # displays — and the cards pinned below it must not move, because a
  # card that moves is a hardware change an installed guest re-detects.
  for f in win98:cirrus:d3dpt xp:d3dpt:cirrus; do
    other="${f##*:}"; f="${f%:*}"; first="${f#*:}"; f="${f%%:*}"
    bundle="$dir/library/adapter-$f/machine.toml"
    # A new machine has no NIC (`bundle::default_network`), and the
    # question below is whether the cards *under* the adapter move when
    # it changes — so this one is given the card first.
    target/release/launcherx --wizard-edit "$bundle" - - - net >/dev/null \
      || { echo "$f: --wizard-edit net failed"; rc=1; continue; }
    target/release/launcherx --wizard-edit "$bundle" - - - - - - - "$other" >/dev/null \
      || { echo "$f: --wizard-edit $other failed"; rc=1; continue; }
    args="$(target/release/launcherx --print-args "$bundle")"
    case "$args" in *"$(vga_args "$other")"*) ;; *) echo "$f: the $other adapter did not arrive"; echo "$args"; rc=1;; esac
    case "$args" in *"$(vga_args "$first")"*) echo "$f: the $first adapter is still there beside the $other"; echo "$args"; rc=1;; esac
    case "$args" in *"netdev=n0,addr=0x03"*) ;; *) echo "$f: the NIC moved when the adapter changed"; echo "$args"; rc=1;; esac
    # The standard VGA is not on offer to Windows — XP has no driver for
    # it at all — so asking for it must leave the machine as it was rather
    # than produce a guest with no display.
    target/release/launcherx --wizard-edit "$bundle" - - - - - - - std >/dev/null \
      || { echo "$f: --wizard-edit std failed"; rc=1; continue; }
    args="$(target/release/launcherx --print-args "$bundle")"
    case "$args" in *"-vga std"*) echo "$f: was given the standard VGA, which has no driver there"; echo "$args"; rc=1;; esac
    target/release/launcherx --wizard-edit "$bundle" - - - - - - - "$first" >/dev/null \
      || { echo "$f: --wizard-edit $first failed"; rc=1; continue; }
    args="$(target/release/launcherx --print-args "$bundle")"
    case "$args" in *"$(vga_args "$first")"*) ;; *) echo "$f: the $first adapter did not come back"; echo "$args"; rc=1;; esac
  done
  # Every adapter on every family, on the real binary: started paused and
  # told to quit, so a machine QEMU will not build is an exit code.
  if [ -x build/qemu/qemu-system-i386 ] && [ -x build/qemu/qemu-img ]; then
    build/qemu/qemu-img create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    for f in win98:d3dpt win98:cirrus xp:d3dpt xp:cirrus other:std other:cirrus dos:-; do
      want="${f#*:}"; f="${f%%:*}"
      bundle="$dir/library/adapter-$f/machine.toml"
      target/release/launcherx --wizard-edit "$bundle" - - - - - - - "$want" >/dev/null || { rc=1; continue; }
      args="$(target/release/launcherx --print-args "$bundle")"
      # shellcheck disable=SC2086
      o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"quit"}\n' \
           | timeout 30 build/qemu/qemu-system-i386 $args \
               -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
        || { echo "our QEMU refused $f on $want"; echo "$o" | tail -3; rc=1; }
    done
  else
    echo "  (no build/qemu: the command lines were checked but not run)"
  fi
  return $rc
}

bios_date_check() { # the legacy BIOS date, as a guest reads it out of a real QEMU
  # Windows 98 installs ACPI — and so enumerates the PCI bus at all — only
  # when the date at F000:FFF5 is at least the ACPICheckDate its own
  # machine.inf carries, 12/01/99 (doc 06); an older BIOS has to be one of
  # the four machines in BIOSINFO.INF's [GoodACPIBios], and we are not.
  # SeaBIOS ships 06/23/99, so prepare-qemu.sh stamps every firmware image
  # it finds. A tree that lost the stamp still boots every existing guest,
  # and only shows up weeks later as a *new* Win98 install that came out in
  # PnP-BIOS mode — "Plug and Play BIOS" with a yellow ! and no USB tablet,
  # AC'97 or NIC ever detected. Hence a check on the firmware itself.
  local rc=0 out date key f cur
  out="$(printf '%s\n' '{"execute":"qmp_capabilities"}' \
        '{"execute":"human-monitor-command","arguments":{"command-line":"xp /8c 0xffff5"}}' \
        '{"execute":"quit"}' \
        | timeout 30 build/qemu/qemu-system-i386 -L qemu/pc-bios -machine pc -m 64 \
            -display none -S -qmp stdio -net none 2>&1)" \
    || { echo "QEMU refused to start"; echo "$out" | tail -3; return 1; }
  # the monitor prints the row as quoted characters; the date is the first
  # eight of them, the rest of the row is the model byte and padding.
  date="$(printf '%s' "$out" | sed -n 's/.*ffff5: //p' | tr -d " '" | cut -c1-8)"
  case "$date" in
    [0-9][0-9]/[0-9][0-9]/[0-9][0-9]) ;;
    *) echo "no BIOS date at F000:FFF5 (read \"$date\")"; echo "$out" | tail -3; return 1 ;;
  esac
  echo "BIOS date $date (Win98 wants >= 12/01/99 to install ACPI)"
  # Setup compares a two-digit year, so the key is yymmdd and a 2000s date
  # would sort *below* the cutoff here exactly as it would there.
  key=$(( 10#${date##*/} * 10000 + 10#${date%%/*} * 100 + 10#$(x=${date#*/}; echo "${x%%/*}") ))
  if [ "$key" -lt 991201 ]; then
    echo "the guest reads $date, older than Win98's ACPICheckDate 12/01/99"
    echo "(run scripts/prepare-qemu.sh, or -f if the tree was edited by hand)"
    rc=1
  fi
  # Every image carries the same day: the pc machine maps bios-256k.bin,
  # but a package ships the lot and microvm/bios.bin are one -machine away.
  for f in qemu/pc-bios/bios.bin qemu/pc-bios/bios-256k.bin qemu/pc-bios/bios-microvm.bin; do
    [ -f "$f" ] || continue
    cur="$(dd if="$f" bs=1 skip=$(( $(wc -c < "$f") - 11 )) count=8 2>/dev/null)"
    [ "$cur" = "$date" ] || { echo "$(basename "$f") says $cur, the running firmware says $date"; rc=1; }
  done
  return $rc
}

optimizations_check() { # the wizard's fast-path switches, all the way to a real QEMU
  local rc=0 dir="$OUT/opt-switches" bundle args o
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  bundle="$(target/release/launcherx --new xp opts "$dir/disk.qcow2")" || { echo "--new failed"; return 1; }
  # A machine nobody has touched must produce the command line it always
  # produced: no properties, and no `[optimizations]` table in the file.
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *-cpu\ pentium3\ *) ;; *) echo "a default machine names a CPU property"; echo "$args"; rc=1;; esac
  case "$args" in *=on*|*=off*) echo "a default machine names an optimization"; echo "$args"; rc=1;; esac
  grep -q '^\[optimizations\]' "$bundle" && { echo "a default machine wrote an [optimizations] table"; rc=1; }
  # Every switch, through the real form: off where it ships on, on where
  # it ships off, and each on the option QEMU looks it up on — a CPU
  # property on `-cpu`, an accelerator property on `-accel tcg`.
  target/release/launcherx --optimizations "$bundle" \
    x87-fast off sse-fast off simd-fast off rep-fast off \
    smc-same-value off soft-imm off inline-lookup off pinned-regs on >"$OUT/optimizations-set.log" 2>&1 \
    || { echo "--optimizations failed"; cat "$OUT/optimizations-set.log"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  for p in x87-fast=off sse-fast=off simd-fast=off rep-fast=off; do
    case "$args" in *"-cpu pentium3,"*"$p"*) ;; *) echo "$p is not on -cpu"; echo "$args"; rc=1;; esac
  done
  for p in smc-same-value=off soft-imm=off inline-lookup=off pinned-regs=on; do
    case "$args" in *"-accel tcg,"*"$p"*) ;; *) echo "$p is not on -accel tcg"; echo "$args"; rc=1;; esac
  done
  # The point of the whole thing: our QEMU accepts the line the launcher
  # writes. Started paused on the real binary and told to quit, so a
  # rejected property is an exit code and not a hung guest.
  if [ -x build/qemu/qemu-system-i386 ] && [ -x build/qemu/qemu-img ]; then
    # A real image, because a zero-byte file is not a disk; and
    # `audiodev=embed0` is the player's own backend, which lives inside
    # the embed library and not out here, so a null one takes the name
    # (the same stand-in `tools/dos-guest-test.py` uses) and the
    # machine's device line is run verbatim.
    build/qemu/qemu-img create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    # shellcheck disable=SC2086
    o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"quit"}\n' \
         | timeout 30 build/qemu/qemu-system-i386 $args \
             -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
      || { echo "our QEMU refused the launcher's command line"; echo "$o" | tail -3; rc=1; }
  else
    echo "  (no build/qemu: the command line was checked but not run)"
  fi
  # "All defaults" empties the table again rather than writing every
  # switch out at its shipped value.
  target/release/launcherx --optimizations "$bundle" defaults >/dev/null 2>&1 || rc=1
  grep -q '^\[optimizations\]' "$bundle" \
    && { echo "\"All defaults\" left an [optimizations] table behind"; rc=1; }
  return $rc
}
no_optionals_check() { # the artefacts link only what we chose (2026-09-07)
  # QEMU auto-detects a large optional surface, so what a build links is
  # otherwise decided by which libraries the machine happened to have --
  # which is how this box, the Mac and the Flatpak SDK end up with three
  # different libqemu-embed. scripts/configure-qemu.sh disables the lot and
  # this asks the built artefacts whether it stuck, because a dropped flag
  # re-links silently and every packager starts carrying the library again.
  # Four families, all dead for us:
  #
  #   display  the player is the front end -- it embeds QEMU, the embed
  #            library appends `-display none` itself and registers its own
  #            3D provider (patch 30). SDL, GTK/VTE, Cocoa, curses, spice.
  #   audio    the player's sound is patch 20's `embed` audiodev; the
  #            headless tools use `none`, xp-cdimage-test.sh uses `wav`.
  #   network  a bundle with networking on says `-netdev user` and nothing
  #            else, so slirp stays and AF_XDP/vde go.
  #   block    every drive is a local file -- qcow2, a raw floppy, or a
  #            disc image through our own `cdimage` driver (doc 17). curl,
  #            libssh, iscsi, nfs, rbd, gluster, blkio.
  #
  #   ...plus brlapi, a braille chardev nothing here has ever opened.
  #
  # It looks for a *loaded* name as well as a linked one, because that is
  # how SDL last bit -- sdl2-compat reaching for SDL3 through LoadLibrary,
  # on a user's PC, where no import-table walk could have seen it.
  local rc=0 f
  local names="build/qemu/libqemu-embed-i386.$SO build/qemu/qemu-system-i386"
  names="$names build/dxvk/src/d3d9/libdxvk_d3d9.$SO$([ "$SO" = so ] && echo .0)"
  names="$names build/win/qemu/libqemu-embed-i386.dll build/win/qemu/qemu-system-i386.exe"
  # displays (Cocoa is a framework, matched by name in the same list)
  local linked='libSDL|libgtk-|libgdk-|libvte|libspice-server|libncurses|Cocoa\.framework'
  # host audio
  linked="$linked"'|libasound|libpulse|libjack|libpipewire|libsndio'
  # network backends and network-storage block drivers
  linked="$linked"'|libxdp|libbpf|libvdeplug|libcurl|libssh|libiscsi|libnfs|librbd|librados|libglusterfs|libblkio'
  # braille
  linked="$linked"'|libbrlapi'
  # loaded by name at run time: the SDL DLLs, which is the case that bit
  local loaded='SDL[23][-.0-9]*\.(so|dll|dylib)'
  # compiled in: QAPI generates one AUDIODEV_DRIVER_<X> enumerator per
  # audio backend, each behind its own `if: CONFIG_AUDIO_<X>` (qapi/
  # audio.json), so the name is in the binary exactly when the backend is
  # built. That catches the three with no shared object of their own --
  # OSS, CoreAudio, DirectSound -- and it is how patch 23 was found: on
  # Windows `--disable-dsound` was a no-op and dsoundaudio.c went in
  # anyway. NONE, WAV and our EMBED are the three that must be there.
  local builtin_audio='AUDIODEV_DRIVER_(ALSA|PA|PIPEWIRE|JACK|OSS|SNDIO|COREAUDIO|DSOUND|SDL|SPICE)$'
  for f in $names; do
    [ -f "$f" ] || continue
    local frc=0
    case "$f" in
      *.dll|*.exe) ;;   # no ldd/otool for PE; the strings pass covers it
      *)
        if [ "$OS" = Darwin ]; then
          otool -L "$f" 2>/dev/null | grep -qE "$linked" \
            && { echo "$f links a library we disabled"; otool -L "$f" | grep -E "$linked" | sed 's/^/    /'; rc=1; frc=1; }
        else
          ldd "$f" 2>/dev/null | grep -qE "$linked" \
            && { echo "$f links a library we disabled"; ldd "$f" | grep -E "$linked" | sed 's/^/    /'; rc=1; frc=1; }
        fi;;
    esac
    if command -v strings >/dev/null; then
      strings -a "$f" | grep -qiE "$loaded" \
        && { echo "$f names an SDL library to load at run time"; rc=1; frc=1; }
      local built
      built=$(strings -a "$f" | grep -oE "$builtin_audio" | sort -u | tr '\n' ' ')
      [ -n "$built" ] && { echo "$f has host audio backends compiled in: $built"; rc=1; frc=1; }
    fi
    [ "$frc" = 0 ] && echo "  clean: $f"
  done
  return $rc
}
have_display() { [ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ] || [ "$OS" = Darwin ]; }
preview_anim_check() { # the shader preview keeps drawing (doc 07)
  # Plenty of presets do not stand still: an interlaced CRT draws
  # alternate fields, a phosphor afterglow decays, an NTSC signal
  # shimmers. The editor's preview renders on demand, so unless it knows
  # to keep asking it shows one frozen frame of all that — the bug this
  # guards. Both front ends take the answer from `launcher_core::preview`,
  # so it is asked here through the verb they share.
  local moving=third_party/slang-shaders/crt/crt-beans-vga.slangp
  local still=third_party/slang-shaders/crt/crt-lottes.slangp
  local rc=0
  # A preset that stands still: said to stand still, and the same picture
  # at any frame number. Also the probe — a box with no usable GPU can
  # answer none of this, and that is a skip, not a failure.
  if ! PREVIEW_FRAME=0 target/release/launcherx --preview-shader \
       "$still" "$GOLDEN" "$OUT/preview-still-0.png" >"$OUT/preview-still.txt" 2>&1; then
    sed 's/^/  /' "$OUT/preview-still.txt"
    echo "no usable GPU for a headless preview"
    return 77
  fi
  grep -qx still "$OUT/preview-still.txt" || { echo "$still: reported as animated"; rc=1; }
  PREVIEW_FRAME=7 target/release/launcherx --preview-shader \
    "$still" "$GOLDEN" "$OUT/preview-still-7.png" >>"$OUT/preview-still.txt" 2>&1 || rc=1
  cmp -s "$OUT/preview-still-0.png" "$OUT/preview-still-7.png" \
    || { echo "$still: frames 0 and 7 differ — the frame number reaches a preset that does not read it"; rc=1; }
  # A preset that does not: said to animate, and two frame numbers really
  # are two pictures (this one interlaces, so it is half the frame).
  PREVIEW_FRAME=0 target/release/launcherx --preview-shader \
    "$moving" "$GOLDEN" "$OUT/preview-moving-0.png" >"$OUT/preview-moving.txt" 2>&1 || rc=1
  grep -qx animated "$OUT/preview-moving.txt" || { echo "$moving: reported as still"; rc=1; }
  PREVIEW_FRAME=1 target/release/launcherx --preview-shader \
    "$moving" "$GOLDEN" "$OUT/preview-moving-1.png" >>"$OUT/preview-moving.txt" 2>&1 || rc=1
  if cmp -s "$OUT/preview-moving-0.png" "$OUT/preview-moving-1.png"; then
    echo "$moving: frames 0 and 1 are the same picture — the preview would be frozen"
    rc=1
  fi
  return $rc
}

# ---------------------------------------------------------------- host stage
host_stage() {
  log "host stage ($OS $ARCH)"

  # x87 oracle: only an x86 host has the real x87 to compare against
  if [ "$ARCH" = x86_64 ] || [ "$ARCH" = i686 ]; then
    cc -O2 -std=gnu11 -Iqemu/target/i386/tcg -o build/x87-fast-test tools/x87-fast-test.c -lm \
      && run_check x87-fast x87-fast.log build/x87-fast-test 2000000 \
      || { [ -x build/x87-fast-test ] || { FAIL+=(x87-fast); echo "  FAIL x87-fast (build)"; }; }
  else
    skip x87-fast "x87 oracle needs an x86 host"
  fi

  # the CD-ROM model (M5, doc 17): images written and read back by discx
  cargo build --release -p libdisc -q 2>"$OUT/libdisc-build.log" \
    && run_check libdisc libdisc.log target/release/discx selftest "$OUT/disc" \
    || { [ -x target/release/discx ] || { FAIL+=(libdisc); echo "  FAIL libdisc (build)"; }; }
  if [ -x build/qemu/qemu-img ] && [ -f "$OUT/disc/mixed.cue" ]; then
    run_check cdimage cdimage.log cdimage_check || true
  else skip cdimage "needs build/qemu/qemu-img and the libdisc check's images"; fi
  if [ -x target/release/discx ]; then
    run_check dirdisc dirdisc.log dirdisc_check || true
  else skip dirdisc "needs target/release/discx"; fi
  if [ -x target/release/launcherx ]; then
    run_check dirshelf dirshelf.log dirshelf_check || true
    run_check shelforder shelforder.log shelforder_check || true
  else skip dirshelf "needs target/release/launcherx"; skip shelforder "needs target/release/launcherx"; fi
  # The launcher's own window (ADR-015: the Qt build is the one every
  # package installs). Still conditional, because it is its own cargo
  # workspace and a host with no Qt 6 builds everything else.
  if [ -x launcher-qt/target/release/launcher-qt ]; then
    run_check qt-wizard qt-wizard.log qtwizard_check || true
    run_check qt-close qt-close.log qtclose_check || true
    run_check qt-profile qt-profile.log qtprofile_check || true
  else
    skip qt-wizard "needs launcher-qt/target/release/launcher-qt (scripts/build.sh qt)"
    skip qt-close "needs launcher-qt/target/release/launcher-qt (scripts/build.sh qt)"
    skip qt-profile "needs launcher-qt/target/release/launcher-qt (scripts/build.sh qt)"
  fi

  # the host GPU probe (ADR-013): what the launcher tells someone about 3D
  # before a machine exists. The verdict itself is a property of the box,
  # so what is checked here is the part that has to hold on every box —
  # that a host with no Vulkan driver at all is reported unavailable,
  # exits non-zero and is pointed at the WineD3D path, that a software
  # driver is warned about rather than refused, and that a report always
  # names the loader and the bar it was judged against.
  cargo build --release -p launcher-core --bin launcherx -q 2>"$OUT/host-check-build.log" \
    && run_check host-check host-check.log host_check_probe \
    || { [ -x target/release/launcherx ] || { FAIL+=(host-check); echo "  FAIL host-check (build)"; }; }

  # the wizard's emulation-optimization switches (patches/qemu/README.md):
  # that a machine nobody has touched still produces the command line it
  # always produced, that each switch lands on the option QEMU looks it up
  # on — a CPU property on `-cpu`, an accelerator property on `-accel tcg`
  # — and that our own QEMU actually accepts the line the launcher writes.
  # The switches' *effect* is the guest batteries' job (x87-guest,
  # sse-guest, rep-guest, smc-guest); this is the wiring between them and
  # a checkbox.
  cargo build --release -p launcher-core --bin launcherx -q 2>"$OUT/optimizations-build.log" \
    && run_check optimizations optimizations.log optimizations_check \
    || { [ -x target/release/launcherx ] || { FAIL+=(optimizations); echo "  FAIL optimizations (build)"; }; }

  # the wizard's pointer switch: a new Windows machine gets the USB tablet
  # (absolute — the host pointer is the guest cursor and the window never
  # grabs), a new DOS machine does not (its mouse drivers read the PS/2
  # controller), the checkbox adds and removes the device *and* its
  # controller, and our QEMU accepts both machines.
  if [ -x target/release/launcherx ]; then
    run_check pointer pointer.log pointer_check || true
  fi

  # the "Other" family (doc 06): the machine for an era OS that is neither
  # Windows nor DOS is defined by what it does *not* get — our display
  # adapter, whose driver is a Windows driver — so the check is that a new
  # one comes out on standard hardware, with the cards pinned where an
  # installed guest will not see them move.
  if [ -x target/release/launcherx ]; then
    run_check family-other family-other.log family_other_check || true
  fi

  # the display-adapter picker (doc 06): each family offers the adapters
  # it has a real driver question about — Windows ours against the one it
  # has an in-box driver for, Other the two standard ones, DOS neither —
  # and changing it must not move the cards pinned below it.
  if [ -x target/release/launcherx ]; then
    run_check display-adapter display-adapter.log display_adapter_check || true
  fi

  # the firmware's legacy BIOS date, which decides whether a *new* Win98
  # install comes out ACPI or PnP-BIOS (doc 06). Asked of a running QEMU,
  # not of the file, because the file is only half the path.
  if [ -x build/qemu/qemu-system-i386 ] && [ -d qemu/pc-bios ]; then
    run_check bios-date bios-date.log bios_date_check || true
  else
    skip bios-date "needs build/qemu/qemu-system-i386"
  fi

  # nothing shipped links or loads one of the optional host libraries we
  # disabled (see the function: it is asked of the artefacts, because the
  # configure summary is not what a packager ends up carrying)
  if [ -f "build/qemu/libqemu-embed-i386.$SO" ] || [ -f "$D3DPT_DXVK_LIB" ]; then
    run_check no-optionals no-optionals.log no_optionals_check || true
  else
    skip no-optionals "needs build/qemu or build/dxvk"
  fi

  # the application icon: every size in packaging/icon/ still derived from
  # the one master (doc 07). They are checked in because nothing that
  # needs an icon can draw one — the launcher embeds a PNG at compile
  # time, the Flatpak build is offline, the Windows package is
  # cross-built without ImageMagick — so a master edited without a
  # regenerate would ship the old picture everywhere but the repository.
  if command -v magick >/dev/null; then
    run_check icons icons.log scripts/gen-icons.sh --check || true
  else
    skip icons "needs ImageMagick"
  fi

  # the Linux package (M6 step 6): staged from this build and asked, with a
  # scrubbed environment, whether it resolves its own player, qemu-img,
  # firmware and guest-tools — the launcher's paths are otherwise baked in
  # at compile time and a regression there only shows on someone else's
  # machine. Rolls no tarball (the check is the point, not the archive).
  if [ ! -x launcher-qt/target/release/launcher-qt ]; then
    # The package installs the Qt launcher (ADR-015), so a checkout that
    # has not built it cannot be packaged at all.
    skip package "needs launcher-qt/target/release/launcher-qt (scripts/build.sh qt)"
  elif [ "$OS" = Linux ] && [ -f build/qemu/libqemu-embed-i386.so ] && [ -x build/qemu/qemu-img ] && [ -d qemu/pc-bios ]; then
    run_check package package.log scripts/package-linux.sh --no-tar --out "$OUT/package" || true
  elif [ "$OS" = Darwin ] && [ -f build/qemu/libqemu-embed-i386.dylib ] && [ -x build/qemu/qemu-img ] && [ -d qemu/pc-bios ]; then
    # The same question in the macOS form (docs/build-macos.md): the .app
    # staged, and everything the loader touches when the packaged player
    # actually runs required to be inside it. No signing — a Developer ID
    # is not something a test suite should assume, and the checks it would
    # protect all run before it.
    run_check package package.log scripts/package-macos.sh --no-build --no-sign --no-dmg --out "$OUT/package" || true
  else
    skip package "Linux or macOS with build/qemu (libqemu-embed, qemu-img) and qemu/pc-bios only"
  fi

  # the C ABI (doc 07): `launcher-core` is a library, and this proves it is
  # usable as one — a C program creating a DOS machine through the shared
  # wizard, putting a disc on the shelf and reading both back. It is the
  # only check on the *third* front end's surface, so a rename or a
  # changed default in a model shows up here as well as in the two GUIs.
  # A scratch library and shelf, never the user's own.
  if cargo build -p launcher-capi >"$OUT/capi-build.log" 2>&1; then
    CAPI_LIB=""
    for cand in target/debug/liblauncher_capi.a target/release/liblauncher_capi.a; do
      [ -f "$cand" ] && CAPI_LIB="$cand" && break
    done
    # Which native libraries a Rust staticlib needs is the toolchain's
    # to know, not ours to guess: on macOS this one wants objc, iconv and
    # half a dozen frameworks, and the Linux list is not a subset of it.
    # rustc will say; the old list stays as the fallback.
    CAPI_LIBS="$(cargo rustc -q -p launcher-capi -- --print native-static-libs 2>&1 \
                 | sed -n 's/^note: native-static-libs: //p' | tail -1)"
    [ -n "$CAPI_LIBS" ] || CAPI_LIBS="-lstdc++ -lm -ldl -lpthread"
    # shellcheck disable=SC2086
    if [ -n "$CAPI_LIB" ] && cc -O1 -std=gnu11 -Ilauncher-capi/include \
         -o build/capi-smoke launcher-capi/examples/smoke.c "$CAPI_LIB" \
         $CAPI_LIBS >>"$OUT/capi-build.log" 2>&1; then
      rm -rf "$OUT/capi"; mkdir -p "$OUT/capi/library"
      : >"$OUT/capi/disc.iso"
      run_check capi capi.log env \
        LAUNCHER_LIBRARY_DIR="$OUT/capi/library" \
        LAUNCHER_DISC_LIBRARY="$OUT/capi/discs.toml" \
        LAUNCHER_SHADER_PROFILES_DIR="$OUT/capi/profiles" \
        build/capi-smoke "$OUT/capi/library" "$OUT/capi/disc.iso" || true
    else
      FAIL+=(capi); echo "  FAIL capi (build)"
    fi
  else
    FAIL+=(capi); echo "  FAIL capi (cargo build -p launcher-capi)"
  fi

  # the embed library's Mesa backend, Linux (EGL) only, one VM per process
  if [ "$OS" = Linux ] && [ -f build/qemu/libqemu-embed-i386.so ]; then
    if cc -O1 -std=gnu11 -Iembed -o build/embed-3d-test tools/embed-3d-test.c \
         -Lbuild/qemu -lqemu-embed-i386 -Wl,-rpath,"$ROOT/build/qemu" -lepoxy; then
      run_check embed-3d embed-3d.log build/embed-3d-test || true
    else FAIL+=(embed-3d); echo "  FAIL embed-3d (build)"; fi
  else
    skip embed-3d "Linux with build/qemu/libqemu-embed-i386.so only"
  fi

  # Glide pass-through without a guest: the real host-side wrapper, loaded
  # by hw/3dfx's own dispatcher, rendering into the window-less context.
  # Linux (EGL) only, one VM per process, like embed-3d above.
  if [ "$OS" = Linux ] && [ -f build/qemu/libqemu-embed-i386.so ] \
     && [ -f build/glide/libglide2x.so ]; then
    if c++ -O1 -std=c++17 -w -Iembed -Ithird_party/openglide -Iqemu/hw/3dfx \
         -o build/glide-host-test tools/glide-host-test.cpp \
         -Lbuild/qemu -lqemu-embed-i386 -Wl,-rpath,"$ROOT/build/qemu" -ldl; then
      QEMU_GLIDE_LIB="$ROOT/build/glide/libglide2x.so" \
        GLIDE_TEST_BMP="$OUT/glide-frame.bmp" \
        run_check glide-host glide-host.log build/glide-host-test || true
    else FAIL+=(glide-host); echo "  FAIL glide-host (build)"; fi
  else
    skip glide-host "Linux with build/glide/libglide2x.so only (scripts/build-glide.sh)"
  fi

  # decoder + executor without a guest
  if [ -f "$D3DPT_EXEC_LIB" ] && [ -f "$D3DPT_DXVK_LIB" ]; then
    if c++ -std=c++17 -O2 -o build/d3dpt-exec-test tools/d3dpt-exec-test.cpp \
         -I"$DX" -I"$DX/windows" -I"$DX/directx" -ldl; then
      run_check d3dpt-exec d3dpt-exec.log build/d3dpt-exec-test "$OUT/exec-test.bmp" 120 60 || true
    else FAIL+=(d3dpt-exec); echo "  FAIL d3dpt-exec (build)"; fi
    if c++ -std=c++17 -O2 -o build/d3dpt-dp2-test tools/d3dpt-dp2-test.cpp \
         -I"$DX" -I"$DX/windows" -I"$DX/directx" -ldl; then
      run_check d3dpt-dp2 d3dpt-dp2.log build/d3dpt-dp2-test "$OUT/dp2-test.bmp" || true
    else FAIL+=(d3dpt-dp2); echo "  FAIL d3dpt-dp2 (build)"; fi
  else
    skip d3dpt-exec "needs $D3DPT_EXEC_LIB and $D3DPT_DXVK_LIB"
    skip d3dpt-dp2 "needs $D3DPT_EXEC_LIB and $D3DPT_DXVK_LIB"
  fi

  # the calibration patterns (doc 09): they render at every era mode, and the
  # circle in `grid` comes out round on the tube it is drawn for
  if cc -O2 -w -o build/crtcal-render tools/crtcal-render.c -lm; then
    mkdir -p "$OUT/crtcal"
    run_check crtcal crtcal.log build/crtcal-render "$OUT/crtcal" || true
  else FAIL+=(crtcal); echo "  FAIL crtcal (build)"; fi

  # the player's display path without a guest: mode analysis, the geometry
  # stage and the CRT preset over every mode in the table (doc 03, M2)
  local preset=third_party/slang-shaders/crt/crt-guest-advanced.slangp
  if have_display && [ -f "$preset" ]; then
    if cargo build --release -p player -q 2>"$OUT/player-build.log"; then
      run_check mode-sweep mode-sweep.log \
        target/release/player --shader "$preset" --mode-sweep "$OUT/mode-sweep" || true
      # The chain's border sampling, from the run that just happened: the
      # player names the *reason* it is off, and "although this adapter
      # has it" is the one that is our own descriptor's fault — a device
      # opened without `ADDRESS_MODE_CLAMP_TO_BORDER` makes librashader
      # sample clamp-to-edge, and every curved preset then smears its
      # outermost pixels over everything outside the tube.
      if grep -q "clamp-to-border sampling: off although" "$OUT/mode-sweep.log"; then
        FAIL+=(mode-sweep-border)
        echo "  FAIL mode-sweep-border (the device dropped clamp-to-border)"
      fi
    else FAIL+=(mode-sweep); echo "  FAIL mode-sweep (build)"; tail -5 "$OUT/player-build.log"; fi
  else
    skip mode-sweep "needs a display and the slang-shaders submodule"
  fi

  # the launcher's shader preview, which unlike the player renders only
  # when asked: that it knows which presets it must keep asking about,
  # and that a frame number really does change their picture (doc 07)
  if [ -f third_party/slang-shaders/crt/crt-beans-vga.slangp ] && [ -x target/release/launcherx ]; then
    run_check preview-anim preview-anim.log preview_anim_check || true
  else
    skip preview-anim "needs the slang-shaders submodule and target/release/launcherx"
  fi

  # the reference scene and the feature test natively over DXVK; window-less
  # (tools/d3dgame-native/win32_headless.h), so no display is needed
  if [ -f "$D3DPT_DXVK_LIB" ]; then
    local flags=(-I"$DX" -I"$DX/windows" -I"$DX/directx" -Lbuild/dxvk/src/d3d9 -ldxvk_d3d9 \
                 -Wl,-rpath,"$ROOT/build/dxvk/src/d3d9")
    if c++ -std=c++17 -O2 -o build/d3dgame9-native tools/d3dgame9-native.cpp "${flags[@]}" \
       && c++ -std=c++17 -O2 -o build/d3dfeat9-native tools/d3dfeat9-native.cpp "${flags[@]}"; then
      rm -f "$OUT/d3dgame9.log" "$OUT/d3dfeat9.log"
      ( cd "$OUT" && DXVK_WSI_DRIVER="${DXVK_WSI_DRIVER:-Headless}" ../d3dgame9-native -frames 600 -dump 300 g9-native.bmp ) >"$OUT/d3dgame9-native.log" 2>&1
      if [ -f "$OUT/g9-native.bmp" ]; then
        run_check d3dgame9-nat d3dgame9-golden.log tools/bmpdiff.py "$GOLDEN" "$OUT/g9-native.bmp" \
          --mask "$HUD_MASK" --tolerance 8 --max-over "$BUDGET" -o "$OUT/g9-native-vs-rig.bmp" \
          && sed -n 1,2p "$OUT/d3dgame9-golden.log" | sed 's/^/       /'
      else FAIL+=(d3dgame9-nat); echo "  FAIL d3dgame9-nat (no frame) — $OUT/d3dgame9-native.log"; tail -3 "$OUT/d3dgame9-native.log"; fi
      ( cd "$OUT" && DXVK_WSI_DRIVER="${DXVK_WSI_DRIVER:-Headless}" ../d3dfeat9-native -frames 600 -dump 300 f9-native.bmp ) >"$OUT/d3dfeat9-native.log" 2>&1
      # the occlusion query must have *resolved* (S_OK), not merely been
      # logged: a window-less client that nothing paces runs so far ahead of
      # the CS thread that GetData spins out and reports S_FALSE with 0
      # pixels, and then only the guest-vs-native diff notices (2026-09-07)
      if [ -f "$OUT/f9-native.bmp" ] && grep -q "occlusion query at frame .*: 0x00000000, [1-9]" "$OUT/d3dfeat9.log"; then
        PASS+=(d3dfeat9-nat); echo "  PASS d3dfeat9-nat"
        grep "occlusion query\|getters" "$OUT/d3dfeat9.log" | sed 's/^/       /'
      else FAIL+=(d3dfeat9-nat); echo "  FAIL d3dfeat9-nat — $OUT/d3dfeat9-native.log"; grep "occlusion query" "$OUT/d3dfeat9.log" | sed 's/^/       /'; tail -3 "$OUT/d3dfeat9-native.log"; fi
    else FAIL+=(d3d-native); echo "  FAIL d3d native harness (build)"; fi
  else
    skip d3dgame9-nat "needs build/dxvk"
    skip d3dfeat9-nat "needs build/dxvk"
  fi
}

# --------------------------------------------------------------- guest stage
QEMU_PID=""; SOCK=""
qmp() { python3 tools/qmpc.py "$SOCK" "$@" >/dev/null; }
guest_teardown() {
  [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null || return 0
  if [ "${TEST_KEEP:-0}" = 1 ]; then echo "  TEST_KEEP=1: XP left running, QMP at $SOCK (pid $QEMU_PID)"; return 0; fi
  qmp json '{"execute":"system_powerdown"}' 2>/dev/null
  for _ in $(seq 60); do kill -0 "$QEMU_PID" 2>/dev/null || return 0; sleep 2; done
  echo "  XP did not power down, killing"; kill "$QEMU_PID" 2>/dev/null
}
guest_stage() {
  log "guest stage"
  local img="${WINXP_IMG:-$HOME/vms/winxp.qcow2}"
  local iso="${GUEST_ISO:-$(ls -t guest-tools/out/guest-tools-3dfx-*.iso 2>/dev/null | head -1)}"
  if command -v nasm >/dev/null && command -v mcopy >/dev/null && [ -x build/qemu/qemu-system-i386 ]; then
    if [ -f build/images/144m/x86BOOT.img ]; then
      run_check x87-guest x87-guest.log python3 tools/x87-guest-test.py || true
      run_check rep-guest rep-guest.log python3 tools/rep-guest-test.py || true
      run_check smc-guest smc-guest.log python3 tools/smc-guest-test.py || true
      run_check sse-guest sse-guest.log python3 tools/sse-guest-test.py || true
      run_check atapi-guest atapi-guest.log python3 tools/atapi-guest-test.py || true
    else skip x87-guest "no FreeDOS floppy yet: run tools/x87-guest-test.py once to fetch it"; fi
  else skip x87-guest "needs nasm, mtools and build/qemu"; fi
  if [ "$OS" != Linux ]; then skip guest "Linux only for now (mkfs.fat, sfdisk, mtools)"; return; fi
  for t in mkfs.fat sfdisk mcopy mmd; do command -v $t >/dev/null || { skip guest "needs $t"; return; }; done
  [ -f "$img" ] || { skip guest "no XP image at $img (WINXP_IMG)"; return; }
  [ -n "$iso" ] && [ -f "$iso" ] || { skip guest "no guest-tools ISO (guest-tools/build-wrappers.sh)"; return; }
  [ -x build/qemu/qemu-system-i386 ] || { skip guest "no build/qemu/qemu-system-i386"; return; }
  # the CD-ROM backend: XP copies a converted guest-tools disc through cdrom.sys (doc 17 §6.3)
  if [ -x target/release/discx ] && command -v bsdtar >/dev/null; then
    # bsdtar keeps the ISO's read-only modes: make the previous extraction deletable first
    [ -d "$OUT/gt-iso" ] && chmod -R u+w "$OUT/gt-iso"
    rm -rf "$OUT/gt-iso"; mkdir -p "$OUT/gt-iso" "$OUT/disc"
    if bsdtar -xf "$iso" -C "$OUT/gt-iso" 2>/dev/null && chmod -R u+w "$OUT/gt-iso" \
       && target/release/discx convert "$iso" "$OUT/disc/gt.cue" --audio "$OUT/disc/tone.wav" >/dev/null 2>&1; then
      # with mingw the run also plays the tone track through MCI into a wav (CD-DA, doc 17 §5.4)
      cdtest=""
      if command -v i686-w64-mingw32-gcc >/dev/null && i686-w64-mingw32-gcc -O2 -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
           -march=pentium3 -mtune=generic -o "$OUT/CDTEST.EXE" guest-tools/src/cdtest.c -lwinmm 2>"$OUT/cdtest-build.log"; then
        cdtest="$OUT/CDTEST.EXE"
      else echo "  (no mingw: guest-cdimage runs without the CD audio part)"; fi
      CDTEST="$cdtest" run_check guest-cdimage guest-cdimage.log tools/xp-cdimage-test.sh "$img" "$OUT/disc/gt.cue" "$OUT/gt-iso" "$OUT/cdimage-xp" || true
      # the same tree again, this time served as a folder rather than an
      # image (isodir, M5g): same reference, same comparison, so the disc
      # being generated on the fly is the only difference
      run_check guest-dirdisc guest-dirdisc.log tools/xp-cdimage-test.sh "$img" "isodir:$OUT/gt-iso" "$OUT/gt-iso" "$OUT/dirdisc-xp" || true
    else skip guest-cdimage "could not extract or convert $iso"; fi
  else skip guest-cdimage "needs target/release/discx and bsdtar"; fi
  [ -f "$D3DPT_EXEC_LIB" ] || { skip guest "no $D3DPT_EXEC_LIB"; return; }
  [ -f "$OUT/g9-native.bmp" ] && [ -f "$OUT/f9-native.bmp" ] || { skip guest "run the host stage first (native oracle frames)"; return; }
  local accel="${TEST_ACCEL:-}"
  if [ -z "$accel" ]; then if [ -w /dev/kvm ]; then accel=kvm; else accel=tcg; fi; fi
  local cpu=(-cpu pentium3); [ "$accel" = kvm ] && cpu=(-cpu host)

  # scratch disk: 64 MB FAT32, one partition at 2048, RUN.BAT drives the whole session
  local scratch="$OUT/scratch.img"
  rm -f "$scratch"; truncate -s 64M "$scratch"
  printf 'label: dos\nstart=2048, type=c\n' | sfdisk -q "$scratch" >/dev/null
  mkfs.fat -F 32 --offset 2048 "$scratch" >/dev/null
  local fat="$scratch@@1048576"
  printf '@echo off\r\nxcopy D:\\D3DPT E:\\D3DPT\\ /I /Y\r\nxcopy D:\\TESTS E:\\D3DPT\\ /I /Y\r\nmkdir E:\\OUT\r\ncd /d E:\\D3DPT\r\nDDVMTEST.EXE\r\nD3DGAME9.EXE -frames 600 -dump 300 E:\\OUT\\G9.BMP\r\nD3DGAME8.EXE -frames 600 -dump 300 E:\\OUT\\G8.BMP\r\nD3DFEAT9.EXE -frames 600 -dump 300 E:\\OUT\\F9.BMP\r\necho done > E:\\OUT\\DONE.TXT\r\n' > "$OUT/RUN.BAT"
  mcopy -i "$fat" "$OUT/RUN.BAT" ::/RUN.BAT

  SOCK="$OUT/qmp.sock"; rm -f "$SOCK"
  local qlog="$OUT/qemu.log"
  echo "  XP: $img (snapshot), ISO: $iso, accel: $accel"
  build/qemu/qemu-system-i386 -L qemu/pc-bios -accel "$accel" "${cpu[@]}" -machine pc -m 512 \
    -drive "file=$img,if=ide,index=0,media=disk,snapshot=on" -drive "file=$scratch,format=raw,if=ide,index=1,media=disk" -cdrom "$iso" \
    -vga cirrus -net none -usb -device usb-tablet -display none -serial none -monitor none \
    -qmp "unix:$SOCK,server,nowait" >"$qlog" 2>&1 &
  QEMU_PID=$!
  trap guest_teardown EXIT
  for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.2; done
  [ -S "$SOCK" ] || { FAIL+=(guest-boot); echo "  FAIL guest-boot (no QMP socket) — $qlog"; tail -5 "$qlog"; return; }

  # boot: XP autologs in; the Run dialog is retried until the device sees the first attach
  local deadline=$(( $(date +%s) + ${TEST_BOOT_TIMEOUT:-300} )) t0=$(date +%s)
  sleep 25
  while ! grep -q ", attached (" "$qlog"; do
    if [ "$(date +%s)" -ge "$deadline" ] || ! kill -0 "$QEMU_PID" 2>/dev/null; then
      FAIL+=(guest-boot); echo "  FAIL guest-boot (no d3dpt attach within the timeout) — $qlog"; tail -5 "$qlog"; return; fi
    qmp keys meta_l+r; sleep 1; qmp keys ctrl+a; qmp type 'E:\RUN.BAT'; qmp keys ret
    for _ in $(seq 15); do grep -q ", attached (" "$qlog" && break; sleep 1; done
  done
  echo "  first attach after $(( $(date +%s) - t0 )) s"
  # three programs, three detaches; then DONE.TXT once XP has flushed the FAT
  deadline=$(( $(date +%s) + 300 ))
  while [ "$(grep -c "DLL_PROCESS_DETACH" "$qlog")" -lt 3 ]; do
    if [ "$(date +%s)" -ge "$deadline" ] || ! kill -0 "$QEMU_PID" 2>/dev/null; then
      FAIL+=(guest-run); echo "  FAIL guest-run ($(grep -c "DLL_PROCESS_DETACH" "$qlog") of 3 programs detached) — $qlog"; grep -i "d3dpt:" "$qlog" | tail -8; return; fi
    sleep 2
  done
  for _ in $(seq 30); do mcopy -n -i "$fat" ::/OUT/DONE.TXT "$OUT/DONE.TXT" 2>/dev/null && break; sleep 2; done
  echo "  guest run done after $(( $(date +%s) - t0 )) s; shutting XP down"
  QEMU_KEEP="${TEST_KEEP:-0}"; TEST_KEEP=0 guest_teardown; TEST_KEEP="$QEMU_KEEP"; QEMU_PID=""
  rm -f "$OUT"/G9.BMP "$OUT"/G8.BMP "$OUT"/F9.BMP "$OUT"/guest-*.log
  mcopy -n -i "$fat" ::/OUT/G9.BMP ::/OUT/G8.BMP ::/OUT/F9.BMP "$OUT/" 2>/dev/null
  mcopy -n -i "$fat" ::/D3DPT/d3dgame9.log "$OUT/guest-d3dgame9.log" 2>/dev/null
  mcopy -n -i "$fat" ::/D3DPT/d3dgame8.log "$OUT/guest-d3dgame8.log" 2>/dev/null
  mcopy -n -i "$fat" ::/D3DPT/d3dfeat9.log "$OUT/guest-d3dfeat9.log" 2>/dev/null
  mcopy -n -i "$fat" ::/D3DPT/ddvmtest.log "$OUT/guest-ddvmtest.log" 2>/dev/null
  # the DirectDraw shim next to the EXE: a Vice City-style launcher check passes
  if grep -q "ddraw.dll is E:" "$OUT/guest-ddvmtest.log" 2>/dev/null && grep -q ": enough" "$OUT/guest-ddvmtest.log"; then
    PASS+=(guest-ddvm); echo "  PASS guest-ddvm"; grep "GetAvailableVidMem" "$OUT/guest-ddvmtest.log" | tr -d '\r' | sed 's/^/       /'
  else FAIL+=(guest-ddvm); echo "  FAIL guest-ddvm — $OUT/guest-ddvmtest.log"; cat "$OUT/guest-ddvmtest.log" 2>/dev/null | tr -d '\r' | sed 's/^/       /'; fi

  local f
  for f in G9 G8; do
    if [ ! -f "$OUT/$f.BMP" ]; then FAIL+=("guest-$f"); echo "  FAIL guest-$f (no frame on the scratch disk)"; continue; fi
    run_check "guest-$f=native" "guest-$f-native.log" tools/bmpdiff.py "$OUT/g9-native.bmp" "$OUT/$f.BMP" --mask "$HUD_MASK" || true
    run_check "guest-$f~rig" "guest-$f-rig.log" tools/bmpdiff.py "$GOLDEN" "$OUT/$f.BMP" --mask "$HUD_MASK" --tolerance 8 --max-over "$BUDGET" || true
  done
  if [ ! -f "$OUT/F9.BMP" ]; then FAIL+=(guest-F9); echo "  FAIL guest-F9 (no frame on the scratch disk)"
  else
    run_check "guest-F9=native" guest-F9-native.log cmp "$OUT/f9-native.bmp" "$OUT/F9.BMP" || true
    grep -h "occlusion query\|getters" "$OUT/d3dfeat9.log" | sort > "$OUT/f9-native.lines"
    grep -h "occlusion query\|getters" "$OUT/guest-d3dfeat9.log" 2>/dev/null | tr -d '\r' | sort > "$OUT/f9-guest.lines"
    run_check "guest-F9-log=native" guest-F9-log.log diff "$OUT/f9-native.lines" "$OUT/f9-guest.lines" || true
  fi
}

case "$STAGE" in
  host) host_stage;;
  guest) guest_stage;;
  all) host_stage; guest_stage;;
  *) echo "usage: $0 [host|guest|all]"; exit 2;;
esac

printf '\n%d passed, %d failed, %d skipped\n' ${#PASS[@]} ${#FAIL[@]} ${#SKIP[@]}
for s in "${SKIP[@]}"; do echo "  skip: $s"; done
for f in "${FAIL[@]}"; do echo "  FAIL: $f"; done
[ ${#FAIL[@]} = 0 ]
