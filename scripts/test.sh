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
#   qt-wizard      what the Qt wizard's fields *show* on each family, beside what
#                  the shared form says: a spin box bounds the value it is
#                  handed against the range it has at that moment, and a model
#                  that republishes a form nobody caught up puts a stale name
#                  back over a typed one, and an optimization box clicked by
#                  hand stopped following Turn all on / off — all
#                  disagreements between the control and the model that
#                  nothing which asks the model would ever notice (only if a
#                  launcher-qt has been built)
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
#   qt-shelf       the Qt disc shelf's "Add disc" field, driven through the half a
#                  real file dialog cannot be: a disc *picked* goes on the shelf
#                  by itself and leaves the field empty, rather than waiting for
#                  a second click on "Add to shelf" — the model is right either
#                  way, so only a probe that asks the window can tell them apart
#                  (only if a launcher-qt has been built)
#   qt-firstrun    the Qt first-run shader offer, driven: Qt's own MessageDialog
#                  is really up on a launcher with no preset collection (it is
#                  shown on a property that has to be published before the first
#                  frame), application-modal, with the platform's Yes/No and the
#                  shared model's words in it; No answers the offer and the next
#                  start comes up with nothing over the grid; and Yes leads to a
#                  download that is *not* in a dialog and then to a Retry/Cancel
#                  result dialog — the sequence that broke when one dialog
#                  followed the model, since accept()/close() both emit
#                  rejected() (only if a launcher-qt has been built)
#   shader-defaults the first-run shader offer without a toolkit: a launcher with
#                  no collection asks and one with a collection does not, "Not
#                  now" is remembered so the question is asked exactly once, and
#                  a "yes" writes the three starter profiles — each naming a
#                  preset librashader really parses, by absolute path, with no
#                  parameter overrides — and writes them only once
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
#   voodoo2        the wizard's Voodoo 2 switch (doc 21): a new machine on
#                  any family has no card, the checkbox adds `-device
#                  voodoo2` in the slot after the sound card's and removes
#                  it again, our QEMU accepts the machine with it, and the
#                  device is on the bus of the machine it booted
#   pad            the gamepad (M13 step 0): a new machine on every family
#                  ignores a controller, neither setting adds anything to the
#                  QEMU command line, a bundle naming a setting from a later
#                  launcher still loads, and the host end's deadzone and
#                  two-threshold hysteresis behave under PLAYER_PAD_SCRIPT
#                  (no machine running this suite has a controller)
#   hpet           a Win98 machine has no HPET (98 has no driver for one, so
#                  it was an Unknown Device in Device Manager) and an XP one
#                  keeps it, asked of our QEMU's own device tree
#   family-other   the "Other" family (doc 06): a machine for an era OS that is
#                  neither Windows nor DOS gets standard hardware and none of
#                  ours — the Bochs VGA rather than d3dpt-vga, no network card
#                  until someone asks for one and an ES1370 at its pinned slot,
#                  no USB tablet — the RTL8139 arrives at 0x03 when the box is
#                  ticked, the sound card stays put when the NIC goes, and our
#                  QEMU accepts the line
#   display-adapter the wizard's display-adapter picker (doc 06): each family
#                  offers the adapters it has a question about and starts on the
#                  right one — a driver question on the three families that have
#                  drivers, and on DOS which VESA BIOS the title finds — an
#                  adapter a family doesn't offer is refused rather than written,
#                  the cards below it don't move when it changes, and our QEMU
#                  accepts every one of them
#   libsynth       synthx selftest (doc 20 §7): the three music engines through the
#                  C API the QEMU devices drive them through — the AdLib detection
#                  sequence a game runs before it will play a note, a 440 Hz FM
#                  note measured against its neighbours, the same note through the
#                  General MIDI bank the packages ship, and a running-status
#                  note-off with a real-time byte wedged inside the note-on
#   music          the sound-card and MIDI-port pickers (doc 20 §6) from a combo
#                  box to a real QEMU — each family's default is the card it
#                  always had, the FM chip follows the card that carried one, a
#                  card a family doesn't offer is refused rather than written, an
#                  MT-32 with no ROMs is refused at the form, the Win98 machine's
#                  "Ensoniq AudioPCI beside it" checkbox puts a second card in
#                  its own slot beside the SB16 and its OPL3 (refused on the
#                  other families, on the bus of the machine it booted) — and
#                  then the two devices *sounding*: the monitor writes the ports
#                  a guest would and the note has to be in the wav QEMU recorded
#   sb-mixer       the SB16's mixer volumes, applied (patch 61): the FM note at
#                  unity and again with the card's FM volume, its master volume
#                  and the SB Pro's FM register each at -12 dB — QEMU's own wav
#                  has to come out 12 dB down all three ways. QEMU stored these
#                  registers and applied none, so Windows' sliders reached
#                  nothing and a game's effects over CD music clipped
#   sb16-irq       the Sound Blaster's interrupt line (patch 25), asked of the
#                  card and the PIC: a DSP reset clears the pending interrupt
#                  and makes none, and a silence block's is one the driver's
#                  read of the status port can acknowledge — an assertion that
#                  cannot be acknowledged holds the line and every interrupt
#                  after it is lost to the edge-triggered i8259
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
#   midi-guest     tools/midi-guest-test.py: the music devices as a *guest* meets
#                  them (doc 20) — a DOS program runs the AdLib detection sequence,
#                  plays 440 Hz on the OPL3, then resets an MPU-401, puts it in UART
#                  mode and plays A4 through it; the wav QEMU's own backend recorded
#                  is what is checked, so a device that takes every write and plays
#                  nothing fails here. Two boots, ~11 s
#   voodoo-guest   tools/voodoo-guest-test.py: the Voodoo 2 device (doc 21, M14) as
#                  a guest meets it with no 3dfx driver — a DOS program finds
#                  121a:0002 in configuration space, maps the BAR, reads the
#                  Voodoo 2 strap, runs the chip's init sequence, fills the back
#                  buffer red through the LFB, reads a pixel back, swaps; a
#                  screendump must be the 640x480 red frame while the Voodoo has
#                  the monitor and the VGA's text screen after it lets go. ~10 s
#   pit-guest      tools/pit-guest-test.py: the PIT as a DOS game's clock meets it
#                  (patch 34) — QCLOCK.COM, DOS Quake's Sys_FloatTime (the BIOS
#                  tick word plus counter 0) read in a tight loop beside the TSC:
#                  no backward reading and every window at 100 %, where upstream's
#                  late IRQ 0 edge made it 200 %. The overdue-irq=off control is
#                  reported, not required. Two boots, ~35 s
#   vbe-palette    VBEPAL=1 tools/vga-dirty-guest-test.py vesa: a VESA game's palette
#                  through the VGA BIOS (patches/seabios, firmware/) — 4F09h sets 65
#                  entries in banked 640x480x8 on std and cirrus, the pages must read
#                  back in those greys, and one entry with three different channels
#                  must reach the DAC as red/green/blue and come back from a 4F09h
#                  get. QEMU's own ROMs answer 0100 (DOS Quake quits, Duke's colours
#                  are wrong). Two boots, ~12 s
#   pad-guest      tools/pad-guest-test.py: the gameport as a DOS guest reads it
#                  (M13 path B) — one write arms four one-shots and the axes are
#                  the counts before each bit falls, with a scripted pad moving
#                  the stick, the d-pad and the buttons. Runs the **player**, so
#                  it skips without a display
#   pad-guest-xp   tools/pad-guest-test.py xp: the USB HID pad as a Windows game
#                  finds it (M13 path A) — through *both* APIs a title of the era
#                  can call: DirectInput enumerates it, every axis on the report's
#                  own 0..255 range, the POV hat's null state and the buttons; and
#                  winmm's joyGetPosEx reads the same pad, on the same range, with
#                  the two columns required to agree. Its own XP boot, ~60 s
#   pad-guest-98   the same on Windows 98, against a launcher *machine* whose
#                  Windows has had the pad's driver bound once (98 asks for its
#                  own source files the first time); WIN98_PAD_MACHINE names it.
#                  The winmm half is what closed M13's last item: a Windows game
#                  on 98 gets its joystick from the USB pad through VJOYD too, so
#                  the gameport's 9x driver half was dropped rather than built
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
  # The cargo builds below link for the same macOS as everything else
  # (Homebrew's floor, scripts/macos-floor.sh), not for rustc's default.
  MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-$(scripts/macos-floor.sh)}"
  export MACOSX_DEPLOYMENT_TARGET
  # DXVK dlopens the Vulkan loader by leaf name; a DYLD_* variable handed
  # to this script is stripped by SIP at the `#!/usr/bin/env` exec, so set
  # the documented macOS run environment here (docs/build-macos.md,
  # patches/dxvk/README.md): Homebrew's loader, and the LunarG SDK's
  # KosmicKrisp ICD unless the caller chose one.
  # The loader's own keg, never all of `/opt/homebrew/lib`: DYLD_LIBRARY_PATH
  # is searched by leaf name ahead of the path an image asks for, and ImageIO
  # `dlopen`s its codecs as `libGIF.dylib` / `libPng.dylib` / `libTIFF.dylib`
  # / `libJPEG.dylib`, every one of which that directory answers on a
  # case-insensitive filesystem (docs/00-status.md, 2026-09-08).
  VKLIB=/opt/homebrew/opt/vulkan-loader/lib
  [ -d "$VKLIB" ] || VKLIB=/opt/homebrew/lib
  export DYLD_LIBRARY_PATH="$VKLIB${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
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
shaderdefaults_check() { # the first-run shader offer and its starter profiles (doc 07)
  local rc=0 dir="$OUT/shaderdefaults" o preset n
  rm -rf "$dir"; mkdir -p "$dir/profiles" "$dir/empty"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  # Everything here except the 50 MB itself: the download is the one part
  # that needs the network, and `shader_source::fetch` is the same code
  # the profile manager's button has always run. What is new — and what
  # goes wrong quietly — is the question's *once-only* rule and the
  # profiles written after a "yes".

  # A launcher with no collection asks, and says what it will do.
  export LAUNCHER_SHADERS_DIR="$dir/empty"
  o="$(target/release/launcherx --first-run status)" || { echo "--first-run status failed"; return 1; }
  case "$o" in asking:*) ;; *) echo "a launcher with no presets did not ask: $o"; rc=1;; esac
  case "$o" in *"$dir/empty"*) ;; *) echo "the question does not name where the collection would land"; echo "$o"; rc=1;; esac
  # It names the profiles it is offering, so the sentence and
  # `DEFAULT_PROFILES` cannot drift apart.
  for n in "CRT Aperture" "CRT Royale" "Apple II"; do
    case "$o" in *"$n"*) ;; *) echo "the question does not mention the $n profile"; rc=1;; esac
  done

  # A launcher that *has* one never asks — which is why nobody working in
  # a checkout has ever seen this dialog (the submodule is a collection).
  o="$(LAUNCHER_SHADERS_DIR=third_party/slang-shaders target/release/launcherx --first-run status)"
  [ "$o" = idle ] || { echo "a launcher with a collection asked anyway: $o"; rc=1; }

  # "Not now" is remembered: answered once, and once only, or the offer
  # becomes a thing that greets you on every start forever.
  target/release/launcherx --first-run decline >/dev/null || { echo "--first-run decline failed"; rc=1; }
  [ -f "$dir/profiles/first-run.txt" ] || { echo "declining wrote no marker"; rc=1; }
  o="$(target/release/launcherx --first-run status)"
  [ "$o" = idle ] || { echo "the offer came back after being declined: $o"; rc=1; }

  # The other half of a "yes", against the collection this checkout has:
  # three profiles, each naming a preset that really is one (librashader
  # parses it — a profile pointing at a missing or unreadable `.slangp`
  # is only a parse error deferred to whoever opens it) and each at the
  # preset's own defaults, which is an *empty* override table.
  o="$(target/release/launcherx --default-profiles third_party/slang-shaders)" \
    || { echo "--default-profiles failed"; return 1; }
  # `-eq`, not `=`: BSD `wc` pads its count with spaces and the string
  # compare then fails on macOS for a library that is exactly right.
  [ "$(printf '%s\n' "$o" | wc -l)" -eq 3 ] || { echo "not three profiles: $o"; rc=1; }
  for n in crt-aperture crt-royale apple-ii; do
    if [ ! -f "$dir/profiles/$n.toml" ]; then echo "no $n.toml"; rc=1; continue; fi
    o="$(sed -n '/^\[params\]/,$p' "$dir/profiles/$n.toml" | grep -c '=' || true)"
    [ "$o" = 0 ] || { echo "$n came out with $o parameter overrides, not the preset's defaults"; rc=1; }
    preset="$(sed -n 's/^preset = "\(.*\)"/\1/p' "$dir/profiles/$n.toml")"
    case "$preset" in /*) ;; *) echo "$n's preset path is relative ($preset)"; rc=1;; esac
    target/release/launcherx --list-shader-params "$preset" >/dev/null 2>&1 \
      || { echo "$n names something librashader will not parse: $preset"; rc=1; }
  done

  # And running it again adds nothing. `shader_library::create` would
  # otherwise deduplicate the *slug* and hand back a second "CRT Royale"
  # as `crt-royale-2` — a second download, or a second launcher start,
  # slowly filling the library with copies.
  o="$(target/release/launcherx --default-profiles third_party/slang-shaders)"
  case "$o" in "(nothing to add"*) ;; *) echo "a second run added profiles again: $o"; rc=1;; esac
  [ "$(ls "$dir/profiles"/*.toml | wc -l)" -eq 3 ] || { echo "the profile library is not still three"; rc=1; }
  return $rc
}
qtfirstrun_check() { # the Qt first-run offer, driven (doc 07)
  local rc=0 dir="$OUT/qtfirstrun" bin="launcher-qt/target/release/launcher-qt" o
  rm -rf "$dir"; mkdir -p "$dir/library" "$dir/profiles" "$dir/empty"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles" LAUNCHER_SHADERS_DIR="$dir/empty"
  export QT_QPA_PLATFORM=offscreen
  # Like `qt-wizard` and `qt-profile`, this asks the *window*: the model
  # can be perfectly right about there being no presets and the dialog
  # still never appear (it is shown on a property that has to be
  # published before the first frame — the trap the whole port is written
  # around), or appear and never go away again.
  o="$(timeout 120 env LAUNCHER_QT_SCREEN=firstrun LAUNCHER_QT_ARG=decline LAUNCHER_QT_DELAY=300 \
       "$bin" 2>&1 | sed -n 's/^\[diag\] firstrun/firstrun/p')"
  [ -n "$o" ] || { echo "the probe printed no firstrun line"; return 1; }
  printf '%s\n' "$o" | sed 's/^/  /'
  printf '%s' "$o" | grep -q "firstrun: open=true, dialog=true, step=asking" \
    || { echo "the dialog was not up on a launcher with no presets"; rc=1; }
  # It is Qt's own confirmation dialog, application-modal (2), with the
  # platform's Yes (0x4000) and No (0x10000) — 81920 together. Neither
  # the modality nor the buttons are things this project draws, and a
  # hand-built row of buttons in a popup is what this replaced.
  printf '%s' "$o" | grep -q "modality=2, buttons=81920" \
    || { echo "not an application-modal Yes/No dialog"; rc=1; }
  # The words in it are the shared model's (ADR-014): the egui build
  # shows the same two strings, and a sentence typed into QML is exactly
  # what used to drift between the two front ends.
  printf '%s' "$o" | grep -q "firstrun text: There are no CRT shader presets" \
    || { echo "the dialog's text is not the model's headline"; rc=1; }
  printf '%s' "$o" | grep -q "slang-shaders (~50 MB) into $dir/empty" \
    || { echo "the dialog does not say what it will download or where"; rc=1; }
  # No, through the dialog's own rejected signal — the wiring from a
  # standard button to the model's verb, not a call into the model.
  printf '%s' "$o" | grep -q "firstrun declined: open=false, step=$" \
    || { echo "the dialog's No did not answer the offer"; rc=1; }
  [ -f "$dir/profiles/first-run.txt" ] || { echo "declining through the window wrote no marker"; rc=1; }

  # Asked once: the next start comes up on the grid, with nothing over it.
  o="$(timeout 120 env LAUNCHER_QT_SCREEN=firstrun LAUNCHER_QT_DELAY=300 "$bin" 2>&1 \
       | sed -n 's/^\[diag\] firstrun: //p')"
  case "$o" in "open=false, dialog=false"*) ;; *) echo "the offer came back on the next start: $o"; rc=1;; esac

  # Yes, and then what replaces the question. The download is pointed at
  # a path that cannot be created, so it fails at once and the run needs
  # no network: what is being checked is the *sequence* — the question
  # answered, the download not in a dialog at all (`busy`, which is what
  # the header shows), and then a second dialog with its own words and
  # the platform's Retry (0x80000) + Cancel (0x400000) = 4718592. It is
  # the transition a single dialog followed a model through until
  # 2026-09-09, when following one turned out to answer it: a
  # MessageDialog's `accept()` and `close()` both emit `rejected()`.
  rm -rf "$dir/profiles"; mkdir -p "$dir/profiles"
  o="$(timeout 120 env LAUNCHER_SHADERS_DIR=/proc/nowhere/shaders LAUNCHER_QT_SCREEN=firstrun \
       LAUNCHER_QT_ARG=accept LAUNCHER_QT_DELAY=300 "$bin" 2>&1 | sed -n 's/^\[diag\] firstrun/firstrun/p')"
  printf '%s\n' "$o" | sed 's/^/  /'
  printf '%s' "$o" | grep -q "firstrun accepted: dialog=true, step=running, busy=true" \
    || { echo "Yes did not start the download"; rc=1; }
  printf '%s' "$o" | grep -q "firstrun settled: result=true, step=failed, buttons=4718592" \
    || { echo "the failure did not come back as a Retry/Cancel dialog"; rc=1; }
  printf '%s' "$o" | grep -q "text=Couldn't download the shader presets" \
    || { echo "the result dialog is not showing the model's failure line"; rc=1; }
  return $rc
}
qtwizard_check() { # what the Qt wizard's memory field *shows* (doc 07)
  local rc=0 dir="$OUT/qtwizard" bin="launcher-qt/target/release/launcher-qt" f o out shown model lo hi n all step line
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
    out="$(timeout 120 env LAUNCHER_QT_SCREEN=wizard LAUNCHER_QT_ARG="$f" LAUNCHER_QT_DELAY=250 \
           "$bin" 2>&1)"
    o="$(printf '%s\n' "$out" | sed -n 's/^\[diag\] wizard memory: //p')"
    if [ -z "$o" ]; then echo "$f: the wizard printed no memory line"; rc=1; continue; fi
    shown="$(printf '%s' "$o" | sed -n 's/^shown \([0-9]*\).*/\1/p')"
    model="$(printf '%s' "$o" | sed -n 's/.*model \([0-9]*\).*/\1/p')"
    lo="$(printf '%s' "$o" | sed -n 's/.*range \([0-9]*\)\.\..*/\1/p')"
    hi="$(printf '%s' "$o" | sed -n 's/.*range [0-9]*\.\.\([0-9]*\).*/\1/p')"
    [ "$shown" = "$model" ] || { echo "$f: the memory field shows $shown, the form says $model"; rc=1; }
    [ "$model" -ge "$lo" ] && [ "$model" -le "$hi" ] \
      || { echo "$f: $model is outside the family's own range $lo..$hi"; rc=1; }
    echo "  $f: $o"
    # The same class of bug from the other side, and the one a user hit:
    # a name typed into the field and then a combo box touched. A text
    # field writes the model *property* alone, so a verb that republishes
    # the form without catching it up first puts the form's own (empty)
    # name back, and the name disappears from a window that never asked
    # it to (2026-09-08).
    o="$(printf '%s\n' "$out" | sed -n 's/^\[diag\] wizard name: //p')"
    shown="$(printf '%s' "$o" | sed -n 's/^shown \[\(.*\)\] model \[.*\]$/\1/p')"
    model="$(printf '%s' "$o" | sed -n 's/^shown \[.*\] model \[\(.*\)\]$/\1/p')"
    [ "$shown" = "Typed name" ] || { echo "$f: the name field lost what was typed (shows: $shown)"; rc=1; }
    [ "$model" = "Typed name" ] || { echo "$f: the model lost the typed name (holds: $model)"; rc=1; }
  done
  # The optimization shortcuts beside boxes that were clicked by hand
  # (user, 2026-09-12: "Turn all on / off does nothing" after three boxes
  # had been unticked). The model moved every time, so again only the
  # window can say whether the boxes did: every step must show what the
  # form says, and the form must be where the button said.
  out="$(timeout 120 env LAUNCHER_QT_SCREEN=optall LAUNCHER_QT_DELAY=250 "$bin" 2>&1)"
  o="$(printf '%s\n' "$out" | sed -n 's/^\[diag\] optall //p')"
  n="$(printf '%s\n' "$o" | sed -n 's/^boxes \([0-9]*\)$/\1/p')"
  [ -n "$n" ] && [ "$n" -gt 0 ] || { echo "optall: the probe printed no box count"; return 1; }
  all=$(( (1 << n) - 1 ))
  for step in clicked on off defaults; do
    line="$(printf '%s\n' "$o" | sed -n "s/^$step: //p")"
    [ -n "$line" ] || { echo "optall: no '$step' line"; rc=1; continue; }
    shown="$(printf '%s' "$line" | sed -n 's/^shown \([0-9]*\) model.*/\1/p')"
    model="$(printf '%s' "$line" | sed -n 's/.* model \([0-9]*\)$/\1/p')"
    echo "  optall $step: $line"
    [ "$shown" = "$model" ] || { echo "optall $step: the boxes show $shown, the form says $model"; rc=1; }
    [ "$step" != on ] || [ "$model" = "$all" ] || { echo "optall: Turn all on left the form at $model"; rc=1; }
    [ "$step" != off ] || [ "$model" = 0 ] || { echo "optall: Turn all off left the form at $model"; rc=1; }
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
qtshelf_check() { # the Qt disc shelf's "Add disc" field, driven (doc 07)
  local rc=0 dir="$OUT/qtshelf" bin="launcher-qt/target/release/launcher-qt" o count field
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles" QT_QPA_PLATFORM=offscreen
  : > "$dir/game.iso"
  # A file dialog belongs to the window system and cannot be opened
  # offscreen, so the probe hands the field the path the dialog would
  # have — every line of the wiring under test is downstream of that.
  # What it guards: a picked disc is on the shelf without a second click
  # (user-reported, 2026-09-09, "Browse… only fills the field"), and the
  # field it came through is left empty, so the button beside it goes
  # back to being for typing.
  o="$(timeout 120 env LAUNCHER_QT_SCREEN=pickdisc LAUNCHER_QT_ARG="$dir/game.iso" LAUNCHER_QT_DELAY=300 \
       "$bin" 2>&1 | sed -n 's/^\[diag\] pickdisc: //p')"
  [ -n "$o" ] || { echo "the probe printed no pickdisc line"; return 1; }
  echo "  $o"
  count="$(printf '%s' "$o" | sed -n 's/^shelf \([0-9]*\),.*/\1/p')"
  field="$(printf '%s' "$o" | sed -n 's/.*field \[\(.*\)\], status.*/\1/p')"
  [ "$count" = 1 ] || { echo "the picked disc did not reach the shelf (it holds $count)"; rc=1; }
  [ -z "$field" ] || { echo "the picked path was left in the field ($field)"; rc=1; }
  grep -q "game.iso" "$dir/discs.toml" 2>/dev/null \
    || { echo "the shelf file never gained the disc"; rc=1; }
  # Every dialog backend on Linux matches its globs case-sensitively, so
  # a lower-case-only filter hid `GAME.CUE` (user-reported, 2026-09-11):
  # the dialog must be handed both spellings (`browse::extensions`).
  printf '%s' "$o" | grep -q 'filters \[Disc images (.*\*\.cue \*\.CUE' \
    || { echo "the disc dialog's filter has no upper-case globs"; rc=1; }
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

pad_check() { # the gamepad host end (M13 step 0) and the machine setting behind it
  local rc=0 dir="$OUT/pad" bundle dos args o held
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  bundle="$(target/release/launcherx --new xp pad "$dir/disk.qcow2")" || { echo "--new failed"; return 1; }
  dos="$(target/release/launcherx --new dos pad-dos "$dir/disk.qcow2")" || { echo "--new dos failed"; return 1; }
  # A new machine ignores a controller, on every family. Not a detail: a
  # stick that rests a little off centre would otherwise hold an arrow
  # key down on a desktop nobody was playing a game on.
  for b in "$bundle" "$dos"; do
    grep -q '^pad = "none"' "$b" || { echo "a new machine did not come out with the pad off"; grep '^pad' "$b"; rc=1; }
  done
  # Neither setting is a device, so neither may add anything to the
  # *guest's* command line. This is what makes the track shippable a path
  # at a time: the `usb` and `gameport` entries do not exist yet, and
  # until their devices do, the launcher cannot write a line QEMU would
  # refuse.
  args="$(target/release/launcherx --print-args "$bundle")"
  local before="$args"
  # A machine with the pad off says nothing to the player either.
  o="$(target/release/launcherx --print-player-args "$bundle")"
  [ -z "$o" ] || { echo "a machine with the pad off still passed the player something: $o"; rc=1; }
  target/release/launcherx --wizard-edit "$bundle" - - - - - - - - keys >/dev/null \
    || { echo "--wizard-edit keys failed"; rc=1; }
  grep -q '^pad = "keys"' "$bundle" || { echo "the pad setting did not stick"; grep '^pad' "$bundle"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  [ "$args" = "$before" ] || { echo "turning the gamepad on changed the QEMU command line"; diff <(echo "$before") <(echo "$args"); rc=1; }
  # ...and the whole of what it does say is the setting (path C).
  o="$(target/release/launcherx --print-player-args "$bundle")"
  [ "$o" = "--pad keys" ] || { echo "expected '--pad keys' for the player, got: $o"; rc=1; }
  target/release/launcherx --wizard-edit "$bundle" - - - - - - - - none >/dev/null \
    || { echo "--wizard-edit none failed"; rc=1; }
  grep -q '^pad = "none"' "$bundle" || { echo "turning it back off did not stick"; rc=1; }
  # A bundle from a later launcher, naming a setting this build has never
  # heard of. It must load and fall back, not refuse the whole machine
  # over a field about a controller. (`gameport` was the placeholder here
  # until path B landed and it became a real answer — the value has to be
  # one no build knows, or this stops testing anything.)
  sed -i 's/^pad = "none"/pad = "wheel"/' "$bundle"
  args="$(target/release/launcherx --print-args "$bundle" 2>&1)" \
    || { echo "a bundle naming a future pad setting would not load at all"; echo "$args"; rc=1; }
  case "$args" in *usb-gamepad*) echo "an unknown pad setting was treated as usb"; rc=1;; esac
  case "$args" in *"-device gameport"*) echo "an unknown pad setting was treated as a gameport"; rc=1;; esac

  # --- path A: the USB HID gamepad --------------------------------
  # DOS is not offered one: it has no USB stack, so the entry is absent
  # the way the display picker omits an adapter a family has no driver
  # for, rather than being offered and then warned about.
  target/release/launcherx --wizard-edit "$dos" - - - - - - - - usb >/dev/null 2>&1
  grep -q '^pad = "usb"' "$dos" && { echo "a DOS machine accepted the USB gamepad"; rc=1; }
  args="$(target/release/launcherx --print-args "$dos")"
  case "$args" in *usb-gamepad*) echo "a DOS machine got a usb-gamepad"; echo "$args"; rc=1;; esac
  # A Windows machine gets the device, and it is passed to the player too.
  target/release/launcherx --wizard-edit "$bundle" - - - - - - - - usb >/dev/null \
    || { echo "--wizard-edit usb failed"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *"-device usb-gamepad"*) ;; *) echo "an XP machine with the pad on has no usb-gamepad"; echo "$args"; rc=1;; esac
  o="$(target/release/launcherx --print-player-args "$bundle")"
  [ "$o" = "--pad usb" ] || { echo "expected '--pad usb' for the player, got: $o"; rc=1; }
  # The controller comes with it even when the pointer does not want one:
  # a `-device usb-gamepad` with no bus to attach to is a machine that
  # will not start, and turning the seamless mouse off used to take the
  # whole USB bus away with the tablet.
  target/release/launcherx --wizard-edit "$bundle" - - - - - - noseamless - - >/dev/null \
    || { echo "--wizard-edit noseamless failed"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *usb-tablet*) echo "the tablet survived turning the seamless mouse off"; echo "$args"; rc=1;; esac
  case "$args" in *"-usb"*) ;; *) echo "the pad lost its USB controller when the pointer gave one up"; echo "$args"; rc=1;; esac
  case "$args" in *"-device usb-gamepad"*) ;; *) echo "the pad went away with the tablet"; echo "$args"; rc=1;; esac

  # --- path B: the gameport ----------------------------------------
  # The mirror image of path A's asymmetry, and the point of the whole
  # path: DOS is the family that cannot have the USB pad and *can* have
  # this, so a DOS machine takes it...
  target/release/launcherx --wizard-edit "$dos" - - - - - - - - gameport >/dev/null \
    || { echo "--wizard-edit gameport failed on DOS"; rc=1; }
  grep -q '^pad = "gameport"' "$dos" || { echo "a DOS machine would not take the gameport"; grep '^pad' "$dos"; rc=1; }
  args="$(target/release/launcherx --print-args "$dos")"
  case "$args" in *"-device gameport"*) ;; *) echo "a DOS machine with the gameport on has no gameport"; echo "$args"; rc=1;; esac
  # ...and it brings no USB controller with it. The port is an ISA device
  # and a DOS guest has no USB stack to drive one with anyway; a stray
  # `-usb` here would be a device in the machine nothing can use.
  case "$args" in *"-usb"*) echo "the gameport dragged a USB controller in"; echo "$args"; rc=1;; esac
  o="$(target/release/launcherx --print-player-args "$dos")"
  [ "$o" = "--pad gameport" ] || { echo "expected '--pad gameport' for the player, got: $o"; rc=1; }
  # ...while XP is not offered it and must refuse it rather than write a
  # port its guest has no way to enumerate: XP's answer is path A.
  target/release/launcherx --wizard-edit "$bundle" - - - - - - - - gameport >/dev/null 2>&1
  grep -q '^pad = "gameport"' "$bundle" && { echo "an XP machine accepted the gameport"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *"-device gameport"*) echo "an XP machine got a gameport"; echo "$args"; rc=1;; esac
  # Win98 is the family with both stacks and is offered both, one at a
  # time: choosing the gameport takes the HID pad *and* its controller
  # away, or the machine would carry two controllers for one host pad and
  # only one of them would ever move.
  local w98
  w98="$(target/release/launcherx --new win98 pad-98 "$dir/disk.qcow2")" || { echo "--new win98 failed"; return 1; }
  for p in usb gameport; do
    target/release/launcherx --wizard-edit "$w98" - - - - - - - - "$p" >/dev/null \
      || { echo "--wizard-edit $p failed on win98"; rc=1; }
    grep -q "^pad = \"$p\"" "$w98" || { echo "a Win98 machine would not take $p"; grep '^pad' "$w98"; rc=1; }
  done
  args="$(target/release/launcherx --print-args "$w98")"
  case "$args" in *"-device gameport"*) ;; *) echo "a Win98 machine with the gameport on has no gameport"; echo "$args"; rc=1;; esac
  case "$args" in *usb-gamepad*) echo "the Win98 machine kept its usb-gamepad after switching to the gameport"; echo "$args"; rc=1;; esac
  # The host end itself, against the scripted pad — no controller, no
  # guest, no window. What it proves is the shaping: the deadzone
  # swallows a resting stick, and the press/release pair has a gap in it
  # so an axis held between them does not chatter.
  if [ -x target/release/player ]; then
    # raw 0.25 is inside the 0.30 deadzone and must produce nothing at all.
    o="$(PLAYER_PAD_SCRIPT='5:lx=0.25' target/release/player --pad-sweep 10 2>&1)" || { echo "$o"; rc=1; }
    case "$o" in *"0 events"*) ;; *) echo "a stick inside the deadzone produced an event"; echo "$o"; rc=1;; esac
    # The hysteresis, as three readings: 0.450 shaped is under the press
    # threshold, 0.600 is over it, and 0.450 *again* must stay held. One
    # threshold instead of two would release on the third and the guest
    # would see a key repeating at the poll rate.
    o="$(PLAYER_PAD_SCRIPT='5:lx=0.615,10:lx=0.72,15:lx=0.615' target/release/player --pad-sweep 20 2>&1)" || { echo "$o"; rc=1; }
    # Per line, not over the whole output: a glob across it matches the
    # word "press" from any *other* frame's line and the check passes for
    # the wrong reason (it did, first time out).
    echo "$o" | grep -q '^\[pad\] frame 5 lx .* press$' \
      && { echo "an axis under the press threshold was called pressed"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^\[pad\] frame 10 lx .* press$' \
      || { echo "an axis over the press threshold was not pressed"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^\[pad\] frame 15 lx .* release$' \
      && { echo "an axis inside the hysteresis band chattered"; echo "$o"; rc=1; }
    # `lx+`, not `lx`: the state is per *half*, because the two ends of a
    # stick are two different keys (path C) and a magnitude test cannot
    # tell them apart.
    held="$(echo "$o" | sed -n 's/^pad-sweep: held at end: //p')"
    [ "$held" = "lx+" ] || { echo "expected lx+ still held at the end, got: $held"; echo "$o"; rc=1; }
    # A malformed script is refused loudly. A typo here otherwise reads
    # exactly like a pad that does not work.
    for bad in '5:nope=1' '5:lx=2.0' '5:south=0.5' 'bad'; do
      if o="$(PLAYER_PAD_SCRIPT="$bad" target/release/player --pad-sweep 10 2>&1)"; then
        echo "PLAYER_PAD_SCRIPT=$bad was accepted"; echo "$o"; rc=1
      fi
    done
    # And that the binary can say what it can read, which is the only
    # place a sandbox with no input access reports itself.
    o="$(target/release/player --pads 2>&1)" || { echo "--pads failed"; echo "$o"; rc=1; }
    case "$o" in gamepads:*) ;; *) echo "--pads said something unexpected: $o"; rc=1;; esac

    # --- path C: the pad presses keys -------------------------------
    # Two controls on one key. The default map puts both the d-pad and
    # the left stick on the arrows, so `left` has two holders: pressing
    # the second must not press the key again, and releasing the *first*
    # must not release it. Counting instead of unioning gets this wrong,
    # and the guest is left with an arrow key stuck down.
    o="$(PLAYER_PAD=keys PLAYER_PAD_SCRIPT='5:dpad_left=1,10:lx=-1.0,15:dpad_left=0,20:lx=0.0' \
         target/release/player --pad-sweep 25 2>&1)" || { echo "$o"; rc=1; }
    [ "$(echo "$o" | grep -c '^pad-key: ')" = 2 ] \
      || { echo "two controls on one key did not produce exactly one down and one up"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-key: frame 5 left .* down$' \
      || { echo "the first holder did not press the key"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-key: frame 20 left .* up$' \
      || { echo "the key was not released when the last holder let go"; echo "$o"; rc=1; }
    # A stick swung across centre inside one poll: the key being left has
    # to go up *before* the key being entered goes down, or a guest that
    # samples between them sees both arrows held.
    o="$(PLAYER_PAD=keys PLAYER_PAD_SCRIPT='5:lx=-1.0,10:lx=1.0,15:lx=0.0' \
         target/release/player --pad-sweep 20 2>&1)" || { echo "$o"; rc=1; }
    [ "$(echo "$o" | grep '^pad-key: frame 10 ' | head -1 | grep -c ' left .* up$')" = 1 ] \
      || { echo "crossing centre did not release the old direction first"; echo "$o"; rc=1; }
    [ "$(echo "$o" | grep '^pad-key: frame 10 ' | tail -1 | grep -c ' right .* down$')" = 1 ] \
      || { echo "crossing centre did not press the new direction"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-sweep: keys down at end: none$' \
      || { echo "a script that let go left keys down in the guest"; echo "$o"; rc=1; }
    # The whole default map reaches real scancodes. `up` is the one to
    # check by number: the stick's negative Y is up on the screen and +1
    # on the wire, so a missing flip here sends the guest `down`.
    o="$(PLAYER_PAD=keys PLAYER_PAD_SCRIPT='2:ly=-1.0,4:south=1' \
         target/release/player --pad-sweep 6 2>&1)" || { echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-key: frame 2 up 0xe048 down$' \
      || { echo "stick up did not send the up arrow (0xe048)"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-key: frame 4 ctrl 0x001d down$' \
      || { echo "the bottom face button did not send ctrl (0x1d)"; echo "$o"; rc=1; }
    # With the pad off nothing is mapped, whatever the controller does.
    o="$(PLAYER_PAD_SCRIPT='5:south=1' target/release/player --pad-sweep 10 2>&1)" || { echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-key: ' \
      && { echo "a machine with the pad off still pressed a key"; echo "$o"; rc=1; }

    # --- path A: the report the guest is handed -----------------------
    # The packing, without a guest: this is the same hid_state() the
    # player sends through qemu_embed_pad_state, so what is checked here
    # is the bytes a driver would parse.
    o="$(PLAYER_PAD=usb PLAYER_PAD_SCRIPT='3:lx=1.0,6:ly=-1.0,9:dpad_up=1,12:dpad_right=1,15:dpad_up=0,18:south=1,21:start=1' \
         target/release/player --pad-sweep 24 2>&1)" || { echo "$o"; rc=1; }
    # A pad nobody has touched reads centred with the hat released. A
    # driver that never gets this shows the stick in a corner.
    echo "$o" | grep -q '^pad-hid: frame 1 axes 80 80 80 80 hat 8 buttons 000000000000$' \
      || { echo "the pad does not start centred with the hat released"; echo "$o"; rc=1; }
    # Stick up is a *low* Y: the screen convention, which is what a guest
    # expects. The sign is flipped once, in the gilrs source; getting it
    # wrong here inverts every game's steering.
    echo "$o" | grep -q '^pad-hid: frame 6 axes ff 01 80 80 hat 8 ' \
      || { echo "stick right/up did not give X=ff, Y=01"; echo "$o"; rc=1; }
    # The hat walks north -> north-east -> east as the d-pad is pressed.
    echo "$o" | grep -q '^pad-hid: frame 9 .* hat 0 ' || { echo "d-pad up is not hat 0"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-hid: frame 12 .* hat 1 ' || { echo "d-pad up+right is not hat 1"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-hid: frame 15 .* hat 2 ' || { echo "d-pad right is not hat 2"; echo "$o"; rc=1; }
    # Buttons land where gamepad::HID_BUTTONS says: south is button 1
    # (bit 0), start is button 10 (bit 9). This order is what a person
    # sees in joy.cpl and what every configured game is bound against.
    echo "$o" | grep -q '^pad-hid: frame 18 .* buttons 000000000001$' \
      || { echo "the bottom face button is not button 1"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-hid: frame 21 .* buttons 001000000001$' \
      || { echo "start is not button 10"; echo "$o"; rc=1; }
    # Opposite directions cancel. A real d-pad cannot press both, and a
    # guest handed "north and south" has to invent an answer.
    o="$(PLAYER_PAD=usb PLAYER_PAD_SCRIPT='3:dpad_up=1,6:dpad_down=1' \
         target/release/player --pad-sweep 8 2>&1)" || { echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-hid: frame 6 .* hat 8 ' \
      || { echo "up and down together did not cancel to the null position"; echo "$o"; rc=1; }
  else
    echo "  (no target/release/player: the machine setting was checked, the host end was not)"
  fi

  # The HID report descriptor: the bytes a guest's driver parses, which
  # nothing on this side reads, so a wrong one shows up only as a device
  # that enumerates and has no axes.
  if command -v python3 >/dev/null; then
    python3 tools/hid-descriptor-check.py >"$OUT/pad-hid-desc.log" 2>&1 \
      || { echo "the usb-gamepad report descriptor is wrong"; cat "$OUT/pad-hid-desc.log"; rc=1; }
    # ...and that the checker can still fail, which is the only thing
    # that makes the line above worth anything. Two mutations, each of
    # which produces a device that looks fine and is not.
    local mut="$dir/hid"; mkdir -p "$mut"
    sed 's/0x81, 0x42,/0x81, 0x02,/' gamepad/qemu/dev-gamepad.c >"$mut/nonull.c"
    sed 's/0x95, 0x04,/0x95, 0x03,/' gamepad/qemu/dev-gamepad.c >"$mut/short.c"
    for m in nonull short; do
      if python3 tools/hid-descriptor-check.py "$mut/$m.c" >/dev/null 2>&1; then
        echo "the descriptor checker passed a deliberately broken descriptor ($m)"; rc=1
      fi
    done
  fi

  # And the point of all of it: our own QEMU takes the machine, and the
  # device really attaches to the bus rather than merely being accepted
  # on the command line. `info usb` is the guest's own view of it.
  if [ -x build/qemu/qemu-system-i386 ] && [ -x build/qemu/qemu-img ]; then
    build/qemu/qemu-img create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    args="$(target/release/launcherx --print-args "$bundle")"
    # shellcheck disable=SC2086
    o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"human-monitor-command","arguments":{"command-line":"info usb"}}\n{"execute":"quit"}\n' \
         | timeout 30 build/qemu/qemu-system-i386 $args \
             -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
      || { echo "our QEMU refused a machine with a usb-gamepad"; echo "$o" | tail -3; rc=1; }
    case "$o" in *"2ksbox USB Gamepad"*) ;; *) echo "the usb-gamepad did not attach to the bus"; echo "$o" | tail -5; rc=1;; esac
    # A second one is refused outright rather than silently ignored: the
    # device drives a single host pad, and two would leave whichever the
    # lookup found first as the only live one.
    # shellcheck disable=SC2086
    o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"quit"}\n' \
         | timeout 30 build/qemu/qemu-system-i386 $args -device usb-gamepad \
             -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)"
    case "$o" in *"only one usb-gamepad"*) ;; *) echo "a second usb-gamepad was not refused"; echo "$o" | tail -3; rc=1;; esac

    # --- path B: the port, as a guest reads it ----------------------
    # The DOS machine's own line, and then the port itself through the
    # human monitor, which is the one way to read 0x201 with no guest.
    # Three readings, and the middle one is the whole timing model:
    #
    #   idle          f0 — the four one-shots expired, no button held.
    #                      Not ff: that is what an *absent* port reads
    #                      off the open bus, and a game uses it to
    #                      decide there is no joystick.
    #   armed         ff — every axis still charging. Read with the VM
    #                      stopped, where the virtual clock does not
    #                      move at all, so this is exact rather than a
    #                      race against a 576 us pulse.
    #   a second on   f0 — and they end. A model that armed and never
    #                      expired would leave every game counting to
    #                      its own timeout, which reads as a stick
    #                      jammed at one extreme.
    args="$(target/release/launcherx --print-args "$dos")"
    # shellcheck disable=SC2086
    o="$({ echo 'i /b 0x201'; echo 'o /b 0x201 0'; echo 'i /b 0x201'; echo cont; sleep 1; \
           echo 'i /b 0x201'; echo 'info qtree'; echo quit; } \
         | timeout 40 build/qemu/qemu-system-i386 $args \
             -audiodev none,id=embed0 -display none -S -monitor stdio -serial none 2>&1)" \
      || { echo "our QEMU refused a machine with a gameport"; echo "$o" | tail -3; rc=1; }
    case "$o" in *"dev: gameport"*) ;; *) echo "the gameport did not attach to the bus"; echo "$o" | tail -5; rc=1;; esac
    local reads
    reads="$(echo "$o" | grep -o 'portb\[0x0201\] = 0x[0-9a-f]*' | sed 's/.*= //' | tr '\n' ' ')"
    [ "$reads" = "0xf0 0xff 0xf0 " ] \
      || { echo "the gameport's one-shots read wrong (idle/armed/expired): $reads"; rc=1; }
    # And one at a time, for the reason the USB pad is: the host drives a
    # single controller, and two ports would answer the same addresses.
    # shellcheck disable=SC2086
    o="$(printf 'quit\n' \
         | timeout 30 build/qemu/qemu-system-i386 $args -device gameport \
             -audiodev none,id=embed0 -display none -S -monitor stdio -serial none 2>&1)"
    case "$o" in *"only one gameport"*) ;; *) echo "a second gameport was not refused"; echo "$o" | tail -3; rc=1;; esac
  else
    echo "  (no build/qemu: the command line was checked but not run)"
  fi
  return $rc
}

voodoo2_check() { # the wizard's Voodoo 2 switch (doc 21), from a checkbox to a real QEMU
  local rc=0 dir="$OUT/voodoo2" bundle dos args o
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  bundle="$(target/release/launcherx --new win98 voodoo2 "$dir/disk.qcow2")" || { echo "--new failed"; return 1; }
  dos="$(target/release/launcherx --new dos voodoo2-dos "$dir/disk.qcow2")" || { echo "--new dos failed"; return 1; }
  # Off unless picked, on every family: a card the guest has no driver
  # for is a New Hardware wizard on every boot.
  for b in "$bundle" "$dos"; do
    args="$(target/release/launcherx --print-args "$b")"
    # (`-device voodoo2`, not the bare name: the scratch disk's path has it)
    case "$args" in *"-device voodoo2"*) echo "a new machine has a Voodoo 2 nobody picked"; echo "$args"; rc=1;; esac
  done
  # The switch through the real form, on the Win98 machine (ours
  # adapter + the chip is the pairing) and on DOS (3dfx's own overlay).
  for b in "$bundle" "$dos"; do
    target/release/launcherx --wizard-edit "$b" - - - - - - - - - voodoo >/dev/null \
      || { echo "--wizard-edit voodoo failed on $b"; rc=1; }
    args="$(target/release/launcherx --print-args "$b")"
    case "$args" in *"-device voodoo2,addr=0x05"*) ;; *) echo "picking the Voodoo 2 added no device"; echo "$args"; rc=1;; esac
    # ...and the bundle says so in the field a newer launcher reads back
    grep -q '^voodoo2 = true' "$b" || { echo "the bundle does not record the card"; rc=1; }
  done
  target/release/launcherx --wizard-edit "$bundle" - - - - - - - - - novoodoo >/dev/null \
    || { echo "--wizard-edit novoodoo failed"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *"-device voodoo2"*) echo "turning the Voodoo 2 off left it on the bus"; echo "$args"; rc=1;; esac
  # Our QEMU accepts the DOS machine with the card, and the card is on
  # its bus: started paused, asked over QMP, told to quit.
  if [ -x build/qemu/qemu-system-i386 ] && [ -x build/qemu/qemu-img ]; then
    build/qemu/qemu-img create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    args="$(target/release/launcherx --print-args "$dos")"
    # shellcheck disable=SC2086
    o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"query-pci"}\n{"execute":"quit"}\n' \
         | timeout 30 build/qemu/qemu-system-i386 $args \
             -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
      || { echo "our QEMU refused the machine with a Voodoo 2"; echo "$o" | tail -3; rc=1; }
    case "$o" in *'"vendor": 4634'*) ;; *) echo "no 3dfx (121a) function on the bus"; echo "$o" | tail -3; rc=1;; esac
  else
    echo "  (no build/qemu: the QEMU half skipped)"
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

libsynth_check() { # the music engines through their C API (doc 20 §7)
  local dir="$OUT/libsynth"
  rm -rf "$dir"; mkdir -p "$dir"
  # The bank the packages ship is what the General MIDI cases play
  # through, deliberately: a truncated or unreadable bank in a package is
  # exactly the failure a fixture written for the occasion never sees.
  target/release/synthx selftest "$dir" --sf2 soundfonts/TimGM6mb.sf2 ${MT32_ROMS:+--roms "$MT32_ROMS"}
}

# One MPU-401 or OPL3 port write, as the human monitor spells it.
port_write() { printf 'o /b %s %s\n' "$1" "$2"; }

# The register writes an AdLib driver makes to hold a 440 Hz note: OPL3
# mode on, one channel of two operators, additive, both outputs, key on.
# fnum 580 at block 4 is 440 Hz on a chip clocked at 49716 Hz.
opl_note_script() {
  port_write 0x38a 0x05; port_write 0x38b 0x01
  port_write 0x388 0x01; port_write 0x389 0x20
  local op
  for op in 0 3; do
    port_write 0x388 "$((0x20 + op))"; port_write 0x389 0x01
    port_write 0x388 "$((0x40 + op))"; port_write 0x389 0x00
    port_write 0x388 "$((0x60 + op))"; port_write 0x389 0xf0
    port_write 0x388 "$((0x80 + op))"; port_write 0x389 0x77
  done
  port_write 0x388 0xc0; port_write 0x389 0x31
  port_write 0x388 0xa0; port_write 0x389 0x44
  port_write 0x388 0xb0; port_write 0x389 0x32
}

sb_mixer_check() { # the SB16's mixer volumes reach the FM chip (patch 61)
  local dir="$OUT/sb-mixer" v rc=0
  rm -rf "$dir"; mkdir -p "$dir"
  # One card with its FM chip, as a machine has them; the mixer is written
  # the way a driver writes it (index at base+4, data at base+5) before
  # the note. 0xc8 is 5-bit level 25 = -12 dB; the SB Pro's 0xcc is 4-bit
  # 12 a side, which a CT1745 reads as the same 25.
  for v in unity fm master sbpro; do
    { case $v in
        fm)     port_write 0x224 0x34; port_write 0x225 0xc8
                port_write 0x224 0x35; port_write 0x225 0xc8 ;;
        master) port_write 0x224 0x30; port_write 0x225 0xc8
                port_write 0x224 0x31; port_write 0x225 0xc8 ;;
        sbpro)  port_write 0x224 0x26; port_write 0x225 0xcc ;;
      esac
      opl_note_script; sleep 2; echo quit; } \
      | timeout 60 build/qemu/qemu-system-i386 -display none -monitor stdio \
          -audiodev "wav,id=w,path=$dir/$v.wav" \
          -device sb16,audiodev=w -device opl3,audiodev=w,sbbase=0x220 >/dev/null 2>&1
  done
  python3 - "$dir" <<'PY' || rc=1
import math, struct, sys, wave
d = sys.argv[1]
def peak(name):
    w = wave.open("%s/%s.wav" % (d, name))
    raw = w.readframes(w.getnframes())
    v = struct.unpack("<%dh" % (len(raw) // 2), raw)
    return max(abs(x) for x in v) if v else 0
ref = peak("unity")
if ref < 1000:
    sys.exit("no FM note at unity (peak %d): nothing to measure against" % ref)
bad = 0
for name in ("fm", "master", "sbpro"):
    p = peak(name)
    db = 20 * math.log10(p / ref) if p else -99
    ok = abs(db + 12) <= 1
    print("  %-6s %+5.1f dB against unity (want -12)%s" % (name, db, "" if ok else "  <- wrong"))
    bad += not ok
sys.exit(1 if bad else 0)
PY
  return $rc
}

# What a driver writes to an MPU-401: reset, UART mode, then a program
# change and a note-on for A4 — the note the checks measure.
mpu_note_script() {
  port_write 0x331 0xff
  port_write 0x331 0x3f
  port_write 0x330 0xc0; port_write 0x330 0x00
  port_write 0x330 0x90; port_write 0x330 0x45; port_write 0x330 0x64
}

# The Sound Blaster's interrupt, asked of the card and the PIC and
# nothing else (patch 25). Every count below is a *rising edge* of IRQ 5
# — `info irq` only counts 0→1 — which is the whole point: the card holds
# its line until the DSP status port is read, so an assertion nobody can
# acknowledge holds it for good and every block after it is a level 1
# into an already-high line, an edge-triggered i8259 sees nothing, and
# the card is deaf until the next reset. Duke Nukem 3D's SETUP.EXE plays
# its "Test Sound FX Card" once and says "Playback failed, possibly due
# to an invalid or conflicting IRQ" every time after.
sb16_irq_check() {
  local rc=0 o n1 n2 n3 n4
  # A block size first: `0x1c` with none set leaves the device with
  # block_size -1, and a DMA that then ran would spin in sb16.c's
  # left_till_irq wrap. The channel is masked at power-up, so nothing
  # transfers here — `0x1c` is only how a guest says "auto-init", which
  # is the state the old reset fabricated an interrupt out of.
  o="$( { port_write 0x22c 0x48; port_write 0x22c 0xff; port_write 0x22c 0x01
          port_write 0x22c 0x1c;               echo "info irq"
          port_write 0x226 0x01; port_write 0x226 0x00
                                               echo "info irq"
          # A one-sample silence block (DSP 0x80): its end is an ordinary
          # 8-bit interrupt and must be acknowledgeable, so the same
          # block a second time has to reach the PIC a second time.
          port_write 0x22c 0x80; port_write 0x22c 0x00; port_write 0x22c 0x00
                                               echo "info irq"
          printf 'i /b 0x22e\n'
          port_write 0x22c 0x80; port_write 0x22c 0x00; port_write 0x22c 0x00
                                               echo "info irq"; echo quit
        } | timeout 60 build/qemu/qemu-system-i386 -display none -monitor stdio \
              -audiodev none,id=w -device sb16,audiodev=w 2>&1 \
            | tr '\r' '\n' \
            | awk '/^IRQ statistics for/ { isa = ($0 ~ /isa-i8259/)
                                 if (isa) { b++; v[b] = 0 }
                                 next }
                   /^ 5:/ && isa { v[b] = $2 }
                   END { for (i = 1; i <= b; i++) print v[i] }')"
  # Four readings, one per `info irq`, the master PIC's: after the DMA
  # command, after the DSP reset, after one silence block, after the
  # second. An absent line is no interrupt at all, which is 0.
  set -- $(printf '%s\n' "$o")
  n1="${1:-0}"; n2="${2:-0}"; n3="${3:-0}"; n4="${4:-0}"
  if [ "$n1" != 0 ]; then
    echo "the sb16 raised IRQ 5 on an auto-init DMA command alone ($n1)"; rc=1
  fi
  if [ "$n2" != "$n1" ]; then
    echo "a DSP reset raised IRQ 5 (count $n1 -> $n2):"
    echo "  hardware clears the pending interrupt there, it does not make one,"
    echo "  and the guest resetting the DSP has its own IRQ masked — the edge"
    echo "  is latched in the PIC, unowned, and Windows never unmasks again"
    rc=1
  fi
  if [ "$n3" != "$((n2 + 1))" ]; then
    echo "a silence block (DSP 0x80) did not raise IRQ 5 (count $n2 -> $n3)"; rc=1
  fi
  if [ "$n4" != "$((n3 + 1))" ]; then
    echo "the second silence block never reached the PIC (count $n3 -> $n4):"
    echo "  the first one's interrupt sets no status bit, so the driver's read"
    echo "  of the DSP status port cannot lower the line and no edge follows"
    rc=1
  fi
  return $rc
}

music_check() { # the two pickers, and then the devices actually sounding
  local rc=0 dir="$OUT/music" bundle args f want o irr
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  # What each family starts on. Every family keeps the card it already
  # had — 98 and DOS the Sound Blaster, XP the AC'97, Other the Ensoniq —
  # so opening an existing machine changes no hardware; what is new is
  # the MIDI port on the two families that have no synthesizer of their
  # own, and the OPL3 that comes with the cards that carried one.
  for f in win98:sb16 dos:sb16 xp:AC97 other:ES1370; do
    want="${f#*:}"; f="${f%%:*}"
    bundle="$(target/release/launcherx --new "$f" "music-$f" "$dir/disk.qcow2")" || { echo "--new $f failed"; return 1; }
    args="$(target/release/launcherx --print-args "$bundle")"
    case "$args" in *"$want,audiodev=embed0"*) ;; *) echo "a new $f machine has no $want"; echo "$args"; rc=1;; esac
  done
  # The FM chip follows the card, the way buying one did: an SB16 carries
  # an OPL3 (and mirrors it at the card's own base, where an SB-aware
  # driver looks), an AC'97 and an Ensoniq carry none.
  for f in win98 dos; do
    args="$(target/release/launcherx --print-args "$dir/library/music-$f/machine.toml")"
    case "$args" in *"opl3,audiodev=embed0,sbbase=0x220"*) ;; *) echo "$f: the SB16 came without its OPL3"; echo "$args"; rc=1;; esac
    case "$args" in *"mpu401,audiodev=embed0,synth=gm"*) ;; *) echo "$f: no General MIDI port on a family that has no synthesizer of its own"; echo "$args"; rc=1;; esac
  done
  for f in xp other; do
    args="$(target/release/launcherx --print-args "$dir/library/music-$f/machine.toml")"
    case "$args" in *opl3*) echo "$f: an FM chip arrived with a card that never had one"; echo "$args"; rc=1;; esac
    case "$args" in *mpu401*) echo "$f: a MIDI port arrived on a family whose default is none"; echo "$args"; rc=1;; esac
  done
  # The switch itself, on the 98 machine: to the AC'97 (which takes the
  # SB16 *and* its FM away, and lands in the pinned PCI slot), to the
  # Gravis, and back.
  bundle="$dir/library/music-win98/machine.toml"
  target/release/launcherx --music "$bundle" ac97 >/dev/null || { echo "--music ac97 failed"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *"AC97,audiodev=embed0,addr=0x04"*) ;; *) echo "98: the AC'97 did not arrive at its pinned slot"; echo "$args"; rc=1;; esac
  case "$args" in *sb16*|*opl3*) echo "98: the SB16 or its FM is still there beside the AC'97"; echo "$args"; rc=1;; esac
  target/release/launcherx --music "$bundle" gus none >/dev/null || { echo "--music gus failed"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *"gus,audiodev=embed0"*) ;; *) echo "98: no Gravis"; echo "$args"; rc=1;; esac
  case "$args" in *mpu401*) echo "98: the MIDI port survived being turned off"; echo "$args"; rc=1;; esac
  # A card this family does not offer is refused rather than written: an
  # ES1370 on Windows is a card 98 has no driver for, and a stray field
  # should not be able to produce one.
  target/release/launcherx --music "$bundle" es1370 >/dev/null || { echo "--music es1370 failed"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *ES1370*) echo "98: was given the Ensoniq, which is not on offer there"; echo "$args"; rc=1;; esac
  target/release/launcherx --music "$bundle" sb16 gm >/dev/null || { echo "--music sb16 gm failed"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *"sb16,audiodev=embed0"*) ;; *) echo "98: the SB16 did not come back"; echo "$args"; rc=1;; esac
  # The Ensoniq *beside* the SB16 (doc 20 §6), 98 only: off unless picked,
  # the checkbox adds `-device ES1370` in its own pinned slot and takes
  # nothing away — the SB16 and its OPL3 stay for the DOS box — the bundle
  # records it, and off removes it again. Another family ignores the pick
  # the way it ignores a card it does not offer.
  case "$args" in *ES1370*) echo "98: an AudioPCI nobody picked"; echo "$args"; rc=1;; esac
  target/release/launcherx --music "$bundle" - - - - audiopci >/dev/null || { echo "--music audiopci failed"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *"ES1370,audiodev=embed0,addr=0x06"*) ;; *) echo "98: picking the AudioPCI added no device"; echo "$args"; rc=1;; esac
  case "$args" in *"sb16,audiodev=embed0"*"opl3,audiodev=embed0,sbbase=0x220"*) ;; *) echo "98: the AudioPCI took the SB16 or its OPL3 away"; echo "$args"; rc=1;; esac
  grep -q '^audiopci = true' "$bundle" || { echo "98: the bundle does not record the AudioPCI"; rc=1; }
  target/release/launcherx --music "$bundle" - - - - noaudiopci >/dev/null || { echo "--music noaudiopci failed"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *ES1370*) echo "98: turning the AudioPCI off left it on the bus"; echo "$args"; rc=1;; esac
  for f in dos xp; do
    target/release/launcherx --music "$dir/library/music-$f/machine.toml" - - - - audiopci >/dev/null || { echo "$f: --music audiopci failed"; rc=1; }
    args="$(target/release/launcherx --print-args "$dir/library/music-$f/machine.toml")"
    case "$args" in *ES1370*) echo "$f: was given an AudioPCI beside its card, which is not on offer there"; echo "$args"; rc=1;; esac
    grep -q '^audiopci = true' "$dir/library/music-$f/machine.toml" && { echo "$f: the bundle recorded an AudioPCI the family cannot have"; rc=1; }
  done
  # The MT-32 has no default and no fallback: nothing of Roland's ships,
  # so a machine asked for one without ROMs must be refused at the form
  # rather than at the guest's first note.
  if target/release/launcherx --music "$bundle" - mt32 >/dev/null 2>&1; then
    echo "98: an MT-32 machine with no ROM directory was saved"; rc=1
  fi
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *mt32*) echo "98: the refused MT-32 was written anyway"; echo "$args"; rc=1;; esac
  target/release/launcherx --music "$bundle" - mt32 - "$dir/roms" >/dev/null || { echo "--music mt32 with a directory failed"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  case "$args" in *"synth=mt32,romdir=$dir/roms"*) ;; *) echo "98: the MT-32's ROM directory did not reach the device"; echo "$args"; rc=1;; esac
  target/release/launcherx --music "$bundle" - gm >/dev/null || { echo "--music gm failed"; rc=1; }

  if [ ! -x build/qemu/qemu-system-i386 ] || [ ! -x build/qemu/qemu-img ]; then
    echo "  (no build/qemu: the command lines were checked but not run)"
    return $rc
  fi
  build/qemu/qemu-img create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
  # Every card on every family, on the real binary: started paused and
  # told to quit, so a machine QEMU will not build is an exit code. The
  # bank is named the way the player names it (companions.rs), because a
  # machine that says synth=gm and nothing else is the normal case.
  export LIBSYNTH_SF2="$PWD/soundfonts/TimGM6mb.sf2"
  for f in win98:sb16 win98:ac97 win98:gus win98:none dos:sb16 dos:gus dos:adlib xp:ac97 xp:sb16 other:es1370 other:ac97; do
    want="${f#*:}"; f="${f%%:*}"
    bundle="$dir/library/music-$f/machine.toml"
    target/release/launcherx --music "$bundle" "$want" >/dev/null || { echo "$f: --music $want failed"; rc=1; continue; }
    args="$(target/release/launcherx --print-args "$bundle")"
    # shellcheck disable=SC2086
    o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"quit"}\n' \
         | timeout 30 build/qemu/qemu-system-i386 $args \
             -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
      || { echo "our QEMU refused the $f machine with the $want card"; echo "$o" | tail -3; rc=1; }
  done
  # ...and the 98 machine with both cards, the Ensoniq on its bus
  # (`query-pci`: vendor 0x1274 is Ensoniq), then back to one card.
  bundle="$dir/library/music-win98/machine.toml"
  target/release/launcherx --music "$bundle" sb16 - - - audiopci >/dev/null || { echo "--music sb16 audiopci failed"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  # shellcheck disable=SC2086
  o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"query-pci"}\n{"execute":"quit"}\n' \
       | timeout 30 build/qemu/qemu-system-i386 $args \
           -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
    || { echo "our QEMU refused the 98 machine with the SB16 and the AudioPCI"; echo "$o" | tail -3; rc=1; }
  case "$o" in *'"vendor": 4724'*) ;; *) echo "no Ensoniq (1274) function on the bus beside the SB16"; echo "$o" | tail -3; rc=1;; esac
  target/release/launcherx --music "$bundle" - - - - noaudiopci >/dev/null || { echo "--music noaudiopci failed"; rc=1; }
  # And the half no command line can show: the devices *sounding*. The
  # monitor writes the same ports a guest would, QEMU's own wav backend
  # records what its mixer produced, and the note has to be in the file —
  # a device that accepts every write and plays nothing passes everything
  # above and fails here.
  rm -f "$dir/opl.wav" "$dir/midi.wav"
  { opl_note_script; sleep 2; echo quit; } \
    | timeout 60 build/qemu/qemu-system-i386 -display none -monitor stdio \
        -audiodev "wav,id=w,path=$dir/opl.wav" -device opl3,audiodev=w >/dev/null 2>&1
  target/release/synthx wavtone "$dir/opl.wav" 440 || rc=1
  { mpu_note_script; sleep 2; echo quit; } \
    | timeout 60 build/qemu/qemu-system-i386 -display none -monitor stdio \
        -audiodev "wav,id=w,path=$dir/midi.wav" \
        -device "mpu401,audiodev=w,synth=gm,soundfont=$PWD/soundfonts/TimGM6mb.sf2" >/dev/null 2>&1
  target/release/synthx wavtone "$dir/midi.wav" 440 || rc=1
  # And the interrupt the MIDI port must *not* raise (doc 20 §5.1,
  # 2026-09-09). A real MPU-401's line is IRQ 2/9; QEMU's PIIX4 puts the
  # ACPI SCI on IRQ 9, and an ACPI Windows 98 owns it, so the ACK a
  # driver's reset queues is an interrupt no handler can acknowledge —
  # the line stays high, the handler is re-entered on every IRET, and
  # the guest triple-faults. Duke Nukem 3D's SETUP rebooted a machine
  # doing exactly this. The reset is written the way a driver writes it
  # and the PIC is asked what is pending; a guest's own answer to that
  # is a spontaneous reboot, which no headless run could tell from a
  # hang, so it is asked here of the hardware instead.
  o="$(printf 'o /b 0x331 0xff\ninfo pic\nquit\n' \
       | timeout 30 build/qemu/qemu-system-i386 -display none -monitor stdio \
           -audiodev none,id=w \
           -device "mpu401,audiodev=w,synth=gm,soundfont=$PWD/soundfonts/TimGM6mb.sf2" 2>&1)"
  irr="$(printf '%s\n' "$o" | sed -n 's/.*pic1: irr=\([0-9a-f]*\).*/\1/p' | tail -1)"
  if [ -z "$irr" ]; then
    echo "could not read the slave PIC back after an MPU-401 reset"; rc=1
  elif [ $(( 0x$irr & 2 )) -ne 0 ]; then
    echo "the MPU-401 left IRQ 9 asserted after a reset (pic1 irr=$irr):"
    echo "  an ACPI Win98 guest triple-faults on it — doc 20 §5.1"
    rc=1
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

hpet_check() { # no HPET on Win98, from the bundle to our QEMU's device tree
  local rc=0 dir="$OUT/hpet" w98 xp args o
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  w98="$(target/release/launcherx --new win98 hpet-98 "$dir/disk.qcow2")" || { echo "--new win98 failed"; return 1; }
  xp="$(target/release/launcherx --new xp hpet-xp "$dir/disk.qcow2")" || { echo "--new xp failed"; return 1; }
  # Windows 98 has no driver for an HPET and never uses one: with it the
  # guest's Device Manager shows an Unknown Device (ACPI\*PNP0103) with a
  # yellow mark on every machine. XP is left alone.
  args="$(target/release/launcherx --print-args "$w98")"
  case "$args" in *"-machine pc,hpet=off "*) ;; *) echo "a Win98 machine still has an HPET"; echo "$args"; rc=1;; esac
  args="$(target/release/launcherx --print-args "$xp")"
  case "$args" in *"-machine pc "*) ;; *) echo "an XP machine's board changed"; echo "$args"; rc=1;; esac
  # And the device itself, asked of the real binary: the property's name
  # is QEMU's to change, and a misspelt one would be an exit code, but a
  # property that stopped removing the device would be neither.
  if [ -x build/qemu/qemu-system-i386 ] && [ -x build/qemu/qemu-img ]; then
    build/qemu/qemu-img create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    for b in "$w98" "$xp"; do
      args="$(target/release/launcherx --print-args "$b")"
      # shellcheck disable=SC2086
      o="$(printf '%s\n' '{"execute":"qmp_capabilities"}' \
             '{"execute":"human-monitor-command","arguments":{"command-line":"info qtree"}}' \
             '{"execute":"quit"}' \
           | timeout 30 build/qemu/qemu-system-i386 $args \
               -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
        || { echo "our QEMU refused $b"; echo "$o" | tail -3; rc=1; continue; }
      case "$b:$o" in
        "$w98":*'dev: hpet'*) echo "our QEMU built a Win98 machine with an HPET"; rc=1;;
        "$xp":*'dev: hpet'*) ;;
        "$xp":*) echo "no HPET in the XP machine, so the Win98 answer proves nothing"; rc=1;;
      esac
    done
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
  # back on; DOS on the standard VGA too, since 2026-09-09 (user
  # decision): the fuller of the two VESA BIOSes, where the hardcoded
  # line it replaced said cirrus.
  for f in win98:"-vga cirrus" xp:"-device d3dpt-vga,addr=0x02" other:"-vga std" dos:"-vga std"; do
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
  # DOS has the picker too since 2026-09-09, and it is the one family
  # where the question is not "which driver": its titles program the
  # adapter themselves, so what changes is which VESA BIOS the game
  # finds. Same three demands as above — the new one arrives, the old one
  # is *gone* rather than sitting beside it, and it comes back — plus the
  # one that is specific here: our own adapter is refused, because there
  # is no DOS driver for it anywhere and a DOS machine on it would have
  # the plain VGA and nothing else.
  bundle="$dir/library/adapter-dos/machine.toml"
  if target/release/launcherx --wizard-edit "$bundle" - - - - - - - cirrus >/dev/null; then
    args="$(target/release/launcherx --print-args "$bundle")"
    case "$args" in *"-vga cirrus"*) ;; *) echo "dos: the Cirrus did not arrive"; echo "$args"; rc=1;; esac
    case "$args" in *"-vga std"*) echo "dos: the standard VGA is still there beside the Cirrus"; echo "$args"; rc=1;; esac
  else
    echo "dos: --wizard-edit cirrus failed"; rc=1
  fi
  if target/release/launcherx --wizard-edit "$bundle" - - - - - - - d3dpt >/dev/null; then
    args="$(target/release/launcherx --print-args "$bundle")"
    case "$args" in *d3dpt-vga*) echo "dos: was given our own adapter, which has no DOS driver"; echo "$args"; rc=1;; esac
  else
    echo "dos: --wizard-edit d3dpt failed"; rc=1
  fi
  if target/release/launcherx --wizard-edit "$bundle" - - - - - - - std >/dev/null; then
    args="$(target/release/launcherx --print-args "$bundle")"
    case "$args" in *"-vga std"*) ;; *) echo "dos: the standard VGA did not come back"; echo "$args"; rc=1;; esac
  else
    echo "dos: --wizard-edit std failed"; rc=1
  fi
  # Every adapter on every family, on the real binary: started paused and
  # told to quit, so a machine QEMU will not build is an exit code.
  if [ -x build/qemu/qemu-system-i386 ] && [ -x build/qemu/qemu-img ]; then
    build/qemu/qemu-img create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    for f in win98:d3dpt win98:cirrus xp:d3dpt xp:cirrus other:std other:cirrus dos:std dos:cirrus; do
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
    smc-same-value off soft-imm off inline-lookup off \
    tb-invalidate-fast off tlb-floor off tls-hot-paths off jump-cache-keep off \
    eob-chain off tlb-retire off pinned-regs on >"$OUT/optimizations-set.log" 2>&1 \
    || { echo "--optimizations failed"; cat "$OUT/optimizations-set.log"; rc=1; }
  args="$(target/release/launcherx --print-args "$bundle")"
  for p in x87-fast=off sse-fast=off simd-fast=off rep-fast=off; do
    case "$args" in *"-cpu pentium3,"*"$p"*) ;; *) echo "$p is not on -cpu"; echo "$args"; rc=1;; esac
  done
  # Since 2026-09-10 the three that had no switch have one, which is what
  # makes "turn everything off" a control run rather than eight of
  # eleven (fourteen since patches 42-44): a guest that is still wrong with these off has cleared our
  # tree, and before this it had not.
  for p in smc-same-value=off soft-imm=off inline-lookup=off \
           tb-invalidate-fast=off tlb-floor=off tls-hot-paths=off jump-cache-keep=off \
           eob-chain=off tlb-retire=off pinned-regs=on; do
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
  # The first-run shader offer and the starter profiles behind it. Needs
  # the preset collection to check what a "yes" writes, so it is skipped
  # on a checkout without the submodule rather than downloading 50 MB
  # inside the suite.
  if [ -x target/release/launcherx ] && [ -f third_party/slang-shaders/crt/crt-aperture.slangp ]; then
    run_check shader-defaults shader-defaults.log shaderdefaults_check || true
  else
    skip shader-defaults "needs target/release/launcherx and the slang-shaders submodule"
  fi
  # The launcher's own window (ADR-015: the Qt build is the one every
  # package installs). Still conditional, because it is its own cargo
  # workspace and a host with no Qt 6 builds everything else.
  if [ -x launcher-qt/target/release/launcher-qt ]; then
    run_check qt-wizard qt-wizard.log qtwizard_check || true
    run_check qt-close qt-close.log qtclose_check || true
    run_check qt-profile qt-profile.log qtprofile_check || true
    run_check qt-shelf qt-shelf.log qtshelf_check || true
    run_check qt-firstrun qt-firstrun.log qtfirstrun_check || true
  else
    skip qt-wizard "needs launcher-qt/target/release/launcher-qt (scripts/build.sh qt)"
    skip qt-close "needs launcher-qt/target/release/launcher-qt (scripts/build.sh qt)"
    skip qt-profile "needs launcher-qt/target/release/launcher-qt (scripts/build.sh qt)"
    skip qt-shelf "needs launcher-qt/target/release/launcher-qt (scripts/build.sh qt)"
    skip qt-firstrun "needs launcher-qt/target/release/launcher-qt (scripts/build.sh qt)"
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
    run_check voodoo2 voodoo2.log voodoo2_check || true
  fi

  # the gamepad (M13 step 0): a new machine ignores a controller on every
  # family, neither setting puts anything on the QEMU command line yet,
  # a bundle from a later launcher still loads, and the host end's
  # shaping — the deadzone and the two-threshold hysteresis — behaves,
  # driven by the scripted pad because no machine running this suite has
  # a controller plugged into it.
  if [ -x target/release/launcherx ]; then
    run_check pad pad.log pad_check || true
  fi

  # the "Other" family (doc 06): the machine for an era OS that is neither
  # Windows nor DOS is defined by what it does *not* get — our display
  # adapter, whose driver is a Windows driver — so the check is that a new
  # one comes out on standard hardware, with the cards pinned where an
  # installed guest will not see them move.
  if [ -x target/release/launcherx ]; then
    run_check family-other family-other.log family_other_check || true
  fi

  # no HPET on a Win98 machine: 98 has no driver for one and showed it as
  # an Unknown Device in Device Manager; asked of our QEMU's device tree.
  if [ -x target/release/launcherx ]; then
    run_check hpet hpet.log hpet_check || true
  fi

  # the music engines (doc 20): the three of them through the same C API
  # the two QEMU devices drive them through, including the bank the
  # packages ship — no guest, no QEMU, ~3 s.
  if [ -x target/release/synthx ]; then
    run_check libsynth libsynth.log libsynth_check || true
  else
    skip libsynth "needs target/release/synthx (cargo build --release -p libsynth)"
  fi

  # the sound-card and MIDI-port pickers (doc 20 §6), and then the two
  # devices sounding into a wav QEMU recorded itself.
  if [ -x target/release/launcherx ] && [ -x target/release/synthx ]; then
    run_check music music.log music_check || true
  fi
  if [ -x build/qemu/qemu-system-i386 ]; then
    run_check sb-mixer sb-mixer.log sb_mixer_check || true
  fi

  # The Sound Blaster's interrupt line (patch 25): no guest, ~1 s. The
  # card and the PIC are asked directly, because what breaks is invisible
  # from the command line and shows up two programs later.
  if [ -x build/qemu/qemu-system-i386 ]; then
    run_check sb16-irq sb16-irq.log sb16_irq_check || true
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
      run_check midi-guest midi-guest.log python3 tools/midi-guest-test.py || true
      run_check pit-guest pit-guest.log python3 tools/pit-guest-test.py || true
      run_check voodoo-guest voodoo-guest.log python3 tools/voodoo-guest-test.py || true
      run_check vbe-palette vbe-palette.log env VBEPAL=1 python3 tools/vga-dirty-guest-test.py vesa || true
      # The gameport as a DOS guest reads it (M13 path B). Unlike its
      # neighbours this one runs the **player**, because the pad reaches a
      # guest through the embed library and a bare QEMU has a gameport
      # nothing ever moves — so it wants a display for the player's window
      # and skips rather than fails without one.
      if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && [ -x target/release/player ]; then
        run_check pad-guest pad-guest.log python3 tools/pad-guest-test.py || true
      else
        skip pad-guest "needs target/release/player and a display (it runs the player)"
      fi
    # **One skip per battery, not one skip standing for five.** Every DOS
    # battery is gated on the same floppy, and this used to report the whole
    # group as a single `SKIP x87-guest` — so a fresh worktree, which has no
    # `build/images/144m/x86BOOT.img` until something fetches it, came back
    # "37 passed, 1 skipped" while the main checkout ran the same suite as
    # "42 passed, 0 skipped". The two numbers look like two different suites
    # and are in fact the same one, minus everything that needs a DOS guest —
    # including `atapi-guest`, which is the only check that reads a disc from
    # inside a guest at all (found 2026-09-09, committing the SafeDisc 1.x
    # weak-sector rule, which that battery is the regression guard for).
    else for c in x87-guest rep-guest smc-guest sse-guest atapi-guest midi-guest pit-guest voodoo-guest vbe-palette pad-guest; do
      skip "$c" "no FreeDOS floppy yet: run tools/x87-guest-test.py once to fetch it"
    done; fi
  else for c in x87-guest rep-guest smc-guest sse-guest atapi-guest midi-guest pit-guest voodoo-guest vbe-palette pad-guest; do
    skip "$c" "needs nasm, mtools and build/qemu"
  done; fi
  if [ "$OS" != Linux ]; then skip guest "Linux only for now (mkfs.fat, sfdisk, mtools)"; return; fi
  for t in mkfs.fat sfdisk mcopy mmd; do command -v $t >/dev/null || { skip guest "needs $t"; return; }; done
  [ -f "$img" ] || { skip guest "no XP image at $img (WINXP_IMG)"; return; }
  [ -n "$iso" ] && [ -f "$iso" ] || { skip guest "no guest-tools ISO (guest-tools/build-wrappers.sh)"; return; }
  [ -x build/qemu/qemu-system-i386 ] || { skip guest "no build/qemu/qemu-system-i386"; return; }
  # The USB HID pad as a Windows game finds it (M13 path A): the same
  # scripted pad, this time through XP's own HID stack and DirectInput.
  # Its own XP boot rather than a passenger on the one below, because that
  # machine has no `usb-gamepad` on it and adding one would change the
  # hardware every other guest check runs against. ~60 s.
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

  # The pad in a Windows guest (M13 path A), **last in the stage**. Two
  # more guest boots, and they go at the back because they are the newest
  # checks here: a new check should not be able to perturb an established
  # one by running before it. (What prompted the move was a `guest-cdimage`
  # timeout on 2026-09-10 — which turned out to be another checkout's TCG
  # guest running on the same box, the thing CLAUDE.md warns about, rather
  # than these. The ordering is right either way.)
  local pad98="${WIN98_PAD_MACHINE:-claude98}"
  if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && [ -x target/release/player ]; then
    run_check pad-guest-xp pad-guest-xp.log python3 tools/pad-guest-test.py xp "$img" || true
    # ...and on Windows 98, which is a *machine* rather than an image: the
    # pad's driver has to have been bound once (98 asks for its own source
    # files the first time and there is nobody here to answer), so this
    # names a launcher machine and reads the disk and display adapter out
    # of its bundle. Skipped where that machine does not exist rather than
    # assuming anyone's library looks like this one.
    if [ -f "$HOME/.local/share/2ksbox/machines/$pad98/machine.toml" ]; then
      run_check pad-guest-98 pad-guest-98.log python3 tools/pad-guest-test.py win98 "$pad98" || true
    else
      skip pad-guest-98 "no launcher machine '$pad98' with the pad installed (WIN98_PAD_MACHINE)"
    fi
  else
    skip pad-guest-xp "needs target/release/player and a display (it runs the player)"
    skip pad-guest-98 "needs target/release/player and a display (it runs the player)"
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
