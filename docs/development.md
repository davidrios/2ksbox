# Developer guide

The technical companion to the top-level `README.md`, which is written
for people who *use* 2ksbox. Everything here is for people who build,
change, package or debug it.

Start with:

- `docs/00-status.md` — the maintained handoff: state table, build cheat
  sheet, open threads, ordered next steps, gotchas.
- `CLAUDE.md` — the locked decisions, conventions, the testing policy and
  the table of every test tool. It is written for an AI assistant but it
  is the most complete "how this repository works" document there is.
- `docs/tracks/` — one document per parallel work track (scope, owned
  files, state, test loop, next steps).
- `patches/qemu/README.md` — every QEMU patch, what it does, when to drop it.

## What exists vs. what we build

| Piece | Status |
|---|---|
| x86 emulation/virtualization | Exists — QEMU fork (slimmer: 93 shared libraries; custom TCG fast paths for x87, SSE, SIMD, REP string, same-value SMC, and inline TB lookup; KVM/WHPX on x86) |
| Guest 3D acceleration | **We build & integrate** — Paravirtual Direct3D device (`d3dpt`, DXVK native host executor) + qemu-3dfx GL pass-through + OpenGLide host Glide wrapper + an emulated Voodoo 2 (86Box's rasterizer as a QEMU PCI device, doc 21) |
| Guest display drivers | **We build** — Native `d3dpt-vga` drivers: XP miniport + display driver with DirectDraw/Direct3D DX8 DDI; Win98 mini-VDD + 16-bit DIB engine driver; SoftGPU/WineD3D as fallback |
| Guest music | **We build** — an OPL3 and an MPU-401 device in QEMU over `libsynth` (nuked-opl3, rustysynth, a Munt port), doc 20 |
| CRT shader ecosystem | Exists — libretro slang shaders via librashader (library, not RetroArch) |
| **Player: in-process QEMU + pixel-accurate CRT-shaded display** | **We build** (Rust, wgpu + librashader, mode analysis, event-driven geometry, low-latency audio) |
| **Companion launcher (library, creation wizard, disc shelf)** | **We build** (Rust: `launcher-core` library; shipped `launcher-qt` in Qt 6 / QML via cxx-qt; `launcher-capi` for C/Swift) |
| **Raw CD-ROM backend (cue/bin, subchannel, C2, CD-DA, dir-as-CD)** | **We build** (Rust "libdisc"; ATAPI patches; live disc shelf; `isodir:` directory mounting) |
| **Guest machine families** | **We build** — Win98, XP, DOS (with cycle-throttled CPU rates), and Other (BeOS, period Linux, OS/2) |

Authentic-hardware Win98 emulation (real S3, cycle-accurate chipsets) is
86Box's territory and explicitly **out of scope** — we don't duplicate
that work. The one exception is the Voodoo 2, whose rasterizer is
vendored verbatim from 86Box because a real chip is the only way to run
Glide 3 and statically linked Glide titles (ADR-016).

## Design docs

0. [**Status and how to resume**](00-status.md) — read first
1. [Goals and non-goals](01-goals.md)
2. [Architecture: in-process QEMU, process model, threading](02-architecture.md)
3. [Display pipeline: pixel accuracy, CRT shaders, latency](03-display-pipeline.md)
4. [3D acceleration: qemu-3dfx, paravirtual D3D, and guest drivers](04-3d-acceleration.md)
5. [CD-ROM backend: raw images, copy protection, and directory discs](05-cdrom-backend.md)
6. [Guest machines: Win98, XP, DOS, and Other reference configs](06-guest-machines.md)
7. [Frontend: machine library, UX, input, audio, and packaging](07-frontend.md)
8. [Roadmap and milestones](08-roadmap.md)
9. [Reference hardware rig](09-reference-hardware.md)
10. [Decision records (ADRs)](10-decisions.md)
11. [M1 embed API design](11-m1-embed-api.md)
12. [M3 window-less GL and Glide context provider design](12-m3-context-provider.md)
13. [x87 shadow doubles: the FPU stack as host doubles in TCG](13-x87-inline-tcg.md)
14. [Paravirtual Direct3D device for XP and Win98](14-d3d-paravirt.md)
15. [A real XP display driver: d3dpt-vga, miniport + Direct3D DDI](15-guest-display-driver.md)
16. [SSE on the host FPU: scalar and packed ops inline in TCG](16-sse-inline-tcg.md)
17. [CD-ROM backend: implementation specification](17-cdrom-implementation.md)
18. [Pinned guest registers: the x86 register file in TCG](18-pinned-guest-registers.md)
19. [A native Win98 display driver: d3dpt9x](19-win9x-display-driver.md)
20. [Music: the OPL3 and MPU-401 devices and their engines](20-music.md)
21. [The Voodoo 2 device](21-voodoo2.md)
22. [The CPU-benchmark evaluation of the patch queue](22-tcg-evaluation.md)
23. [Dynamic binary translation: a literature survey](23-dbt-literature.md)

Platform build guides and tracks:

- [Building and packaging on macOS (Apple Silicon)](build-macos.md)
- [Building and packaging for Windows (cross-build from Linux)](build-windows.md)
- [Parallel development tracks](tracks/) (M4 to M14)

## The build, stage by stage

`scripts/build.sh` is the one command (the README has the walkthrough).
`--help` lists the stages; `-f` re-runs every prepare and configure step.
What it runs, for when a single stage has to be driven by hand:

```sh
scripts/prepare-qemu.sh      # overlay qemu-3dfx devices + embed/, patches, sign
scripts/configure-qemu.sh    # configure (uv-managed python — needs uv, ninja, glib, pixman)
ninja -C build/qemu qemu-system-i386 qemu-img qemu-io libqemu-embed-i386.so   # .dylib on macOS
cargo build --release        # player links libqemu-embed (rpath into build/qemu)
cargo check --release --workspace          # launcher-capi, the one non-default member
(cd launcher-qt && cargo build --release)  # the Qt launcher; its own workspace
# Direct3D pass-through (doc 14):
scripts/prepare-dxvk.sh && scripts/configure-dxvk.sh && ninja -C build/dxvk && scripts/build-d3dpt-exec.sh
# Glide pass-through (doc 12 §5):
scripts/prepare-openglide.sh && scripts/build-glide.sh
# the guest-tools ISO (SETUP.EXE, the guest DLLs, both display drivers):
guest-tools/build-wrappers.sh
```

`configure-qemu.sh` also builds `libdisc` (the CD-ROM model) and
`libsynth` (the music engines) and links both into QEMU (patches 50 and
60). After every `git pull`, run `scripts/build.sh`: it hashes each
prepare step's inputs into `build/.stamp-*` and redoes only what changed.
Why the stamps matter, and what a stale stage looks like, is in
`docs/00-status.md`'s cheat sheet and in `CLAUDE.md`.

The player with no launcher at all, the way M1 first booted something:

```sh
target/release/player        # no arguments: the M0 test pattern (integer-scaled 4:3)
target/release/player -- -L $PWD/qemu/pc-bios -machine pc -m 32 \
  -drive file=path/to/floppy.img,format=raw,if=floppy -boot a -vga std -net none
```

Everything after `--` is a `qemu-system-i386` command line. The player
adds `-display none` and `-audiodev embed,id=embed0` itself; attach the
audio with e.g. `-machine pc,pcspk-audiodev=embed0` or
`-device sb16,audiodev=embed0`.

## The player: command line and environment

```
player [--shader <preset.slangp>] [--shader-params <k=v,...>] [--pad usb|keys]
       [--mode-sweep <dir>] [--calib <bmp|dir>] [--companions] [--pads]
       [--] <qemu args...>
```

### Shaders

- `--shader <preset.slangp>` (or `PLAYER_SHADER=`) runs a libretro slang
  preset, e.g. `--shader third_party/slang-shaders/crt/crt-lottes.slangp`.
- `--shader-params <name=value,...>` (or `PLAYER_SHADER_PARAMS=`)
  overrides the preset's own parameter defaults by name, e.g.
  `--shader-params BRIGHTBOOST=1.4,GAMMA_INPUT=2.4`. This is what a
  launcher shader profile (`launcher-core/src/shader_profile.rs`)
  resolves to. `shaders/README.md` has the curated presets.
- `--calib <bmp|dir>` shades doc 09's CRT calibration patterns
  (`tools/crtcal-render` writes them; `TESTS\CRTCAL.EXE` puts the same
  ones on a real tube) and exits.
- `--mode-sweep <dir>` runs doc 03's mode sweep instead of a guest: every
  mode in the table, the geometry stage and the preset checked, a PNG of
  each dumped there. `PLAYER_MODE_PARAMS=0` is the A/B control for mode
  analysis — the preset is left to guess the scanline count from the
  framebuffer height, as it did before M2.

### Frames and dumps

- `PLAYER_DUMP=frame.png PLAYER_DUMP_SEQ=150` dumps guest frame #150 and
  exits (headless check).
- `PLAYER_DUMP_OUT=out.png` dumps the *shaded* frame (GPU readback) at
  `PLAYER_DUMP_SEQ` and exits; works while the window is occluded.
- `Ctrl+Alt+S` writes the guest's own frame — its native size, no
  geometry stage and no CRT chain — as `PLAYER_SHOT_DIR/2ksbox-NNNN.png`
  (the next free number; the working directory when `PLAYER_SHOT_DIR` is
  unset).
- `PLAYER_SHOT_EVERY=300` takes that same shot on its own every 300
  presented guest frames (a scripted run's window is behind a terminal
  and gets no redraws, so it is driven from the wake path): the only way
  a headless run sees a 3D frame, since a QMP screendump shows the VGA
  surface, frozen while the 3D device presents.
- `PLAYER_REFRESH_MS=16` (default) is the guest frame pull interval
  (QEMU's own default is 30).
- `PLAYER_REFRESH_LOG=1` prints a guest frame counter every 100 frames —
  whether the guest is drawing at all. Off by default: a machine left
  running printed it for as long as it was up.
- `PLAYER_LATENCY=1` prints publish→present latency percentiles every
  240 guest frames.
- `PLAYER_ZERO_COPY=0` refuses every dma-buf the backend offers, so 3D
  frames come back through the readback path instead of the ring (doc 12
  §4). The A/B that puts a wrong 3D picture on one side or the other of
  the hand-off: the readback path copies under a lock, so a fault that
  survives it is not the ring's.
- `PLAYER_PUBLISH_LOG=1` prints a line per published frame naming what
  made it — a ring slot and which one, the readback path, the VGA
  surface, or the cursor's republish — and a line per frame *presented*,
  on the render thread. Which source a frame came from is invisible in
  the picture, and two of them taking turns is a flicker: pairing the
  presented slot with `PLAYER_SHOT_EVERY=1`'s shot is how the frozen ring
  slot below was found.

### Keys, pointer and window

- `PLAYER_KEYS="120:enter,360:ctrl+g"` presses keys/chords at guest
  frames (headless input test); each press is held `PLAYER_KEYS_HOLD`
  frames (default 6 ≈ 100 ms) — a down+up in one flush is a zero-length
  press that a game polling the keyboard state never sees.
- `Ctrl+Alt+G` releases the grab. `Ctrl+Alt+Shift+D` is Ctrl+Alt+Del in
  the guest (the real one stays the host's). `Ctrl+Alt+Shift+F` toggles
  windowed full screen (borderless, the window's monitor).
- A close with Alt held (Alt+F4 while the host has its shortcuts) asks
  first, in the window: Enter, Close or a second Alt+F4 stops the
  machine, Esc or Back returns to it. The title bar's close button does
  not ask.
- While the window has focus the host's own shortcuts go to the guest —
  the Windows key opens the guest's Start menu (Wayland's shortcut
  inhibitor, an X11 keyboard grab, a low-level hook on Windows; nothing on
  macOS). `Ctrl+Alt+K` hands them back to the host and, pressed again, to
  the guest (the title says when they are the host's).
  `PLAYER_KEYBOARD_CAPTURE=0` starts a run with them the host's;
  `scripts/test.sh` sets it, so a test window sway focuses does not take
  the desktop's keys away.
- `qemu-embed: input:` lines on stderr report the embed input queue's
  drain latency, key down/up pairs delivered in one drain (zero-length
  presses) and drops — printed only when something is off.

### Audio and music

- `PLAYER_AUDIO_MS=40` (default) is the audio cushion QEMU keeps in the
  ring under the host device's own pull: the latency on top of the
  device's period, and how late QEMU's main loop may run (TCG, the D3D
  executor) before a gap is heard. `qemu-embed: audio:` and
  `[audio] … underruns` lines on stderr count gaps when they happen and
  `[audio] device asks for N frames` says how chunky the device is; raise
  it if gaps are counted, lower it under KVM.
- `QEMU_EMBED_AUDIO_TRACE=1` prints the embed audiodev's pacing, a line
  per call.
- `[audio] the guest's mix went past full scale` says the machine's
  voices (card, FM, MIDI, CD audio) summed past what fits — QEMU applies
  no mixer volumes — and how far the player's limiter turned it down.
- `PLAYER_AUDIO_NULL=<frames>` drains the audio ring with no device: a
  thread taking that many frames a period at 48 kHz, like a DAC (`1` =
  1024, PipeWire's default).
- `PLAYER_AUDIO_TAP=out.wav` records exactly what the player handed the
  audio device, padded silence included (`tools/audio-glitch-test.py`
  counts clicks in it).
- `LIBSYNTH_SF2=<file.sf2>` is the General MIDI bank the machine's MIDI
  port plays through (doc 20). The player sets it itself — the packaged
  bank, or `soundfonts/` in a checkout — so this is only for trying
  another bank; a machine that names its own wins over both.
- `LIBSYNTH_MT32_ROMS=<dir>` the same for the Roland CM-32L's ROMs, which
  are the user's own.
- `LIBSYNTH_MIDI_LOG=<file>` / `LIBSYNTH_OPL_LOG=<file>` capture what a
  guest wrote to a music device, for `synthx midilog` / `opllog` / `play`
  (doc 20 §7.2).

### Gamepads (M13, `docs/tracks/m13-gamepads.md`)

- `player --pads` says what this host can read, which is the one place a
  build without the `gilrs` feature or a sandbox with no `/dev/input`
  reports itself.
- `--pad usb` (or `PLAYER_PAD=usb`) sends the pad to the machine's
  `usb-gamepad` (patch 26): two analog sticks, an 8-way hat and twelve
  buttons, which XP, Windows 98 SE and Me all see through their own HID
  driver with nothing installed — DirectInput and joy.cpl find it on the
  first start after the device is added. The launcher adds `-usb -device
  usb-gamepad` for a machine whose `pad = "usb"`. Not offered on DOS,
  which has no USB stack.
- `--pad keys` (or `PLAYER_PAD=keys`) maps the pad onto the keys the
  player already sends: d-pad and left stick are the arrows, the four
  face buttons are Ctrl, Alt, Space and Enter, Start is Esc. The launcher
  writes it from the machine's own setting (`pad` in the bundle), the way
  it writes `--shader`. Works on every guest, because there is no device
  for the guest to support; a game that asks DirectInput for a joystick
  still finds none — that needs the USB gamepad.
  `launcherx --print-player-args <machine.toml>` shows what a bundle
  resolves to.
- `PLAYER_PAD_SCRIPT="30:lx=1.0,45:south=1,51:south=0"` is a synthetic
  pad: set a control to a value at a guest frame number. Frames, not
  milliseconds, so a run lands in the same place in the guest's execution
  every time (as `PLAYER_KEYS` does). Controls: `lx`/`ly`/`rx`/`ry`
  (axes, -1.0..1.0; negative is left/up), `south`/`east`/`west`/`north`,
  `dpad_up`/`down`/`left`/`right`, `l1`/`r1`/`l2`/`r2`, `l3`/`r3`,
  `select`/`start` (0 or 1). It wins over real hardware, so a test is not
  perturbed by what is plugged in.
- `PLAYER_PAD_LOG=1` prints every shaped reading with its press/release
  transitions.
- `PLAYER_PAD_SHAPING="0.30,0.55,0.40"` overrides deadzone, press,
  release — the press and release thresholds differ on purpose, and
  release must be the lower of the two: with one number a stick held at
  it chatters at the poll rate.
- `player --pad-sweep <frames>` replays `PLAYER_PAD_SCRIPT` with no
  window, no QEMU and no guest, and prints what came out — with `--pad
  keys`, the key presses too. The `pad` check in `scripts/test.sh`.

### QMP

The player always attaches a control monitor over a socketpair (no
socket file).

- `PLAYER_QMP=1` logs every QMP event (SHUTDOWN/RESET/STOP/... are logged
  regardless).
- `PLAYER_QMP_EXEC='{"execute":"query-status"}'` (or a JSON array of
  requests) runs commands once the guest has drawn its first frame and
  prints the replies.

### Direct3D pass-through (doc 14)

The `d3dpt` device is always present; it loads
`build/d3dpt/libd3dpt_exec.so` (`D3DPT_EXEC_LIB`) and DXVK
(`D3DPT_DXVK_LIB`) on the guest's first use. Guest side: `D3DPT\` on the
guest-tools ISO.

- `D3DPT_DUMP_DIR=dir D3DPT_DUMP_EVERY=60` makes the executor write every
  60th presented frame as `dir/frame-NNNNNN.ppm` (works with bare
  `qemu-system-i386` too).
- Guest side: `D3DPT_TRACE=1` or a file `d3dpt_trace.on` next to the DLL
  writes the creation/lock/upload/present calls to `d3d8_trace.log` /
  `d3d9_trace.log`; a DLL that cannot open the device forwards
  `Direct3DCreateN` to the system DLL.
- While the device is active the player shows the VGA surface again
  after 1 s without a presented frame if the guest drew on it (a game's
  error dialog, a DirectShow movie, a crashed process):
  `[display] no 3D frame for …`.
- `player --companions` prints what `player/src/companions.rs` resolved
  for the Glide wrapper, the D3D executor and DXVK — the packagers' check.

### Glide pass-through (doc 12 §5)

The guest's `GLIDE2X.DLL` reaches a host-side wrapper QEMU dlopens at
`grGlideInit` — qemu-3dfx ships none, so ours is OpenGLide
(`scripts/prepare-openglide.sh && scripts/build-glide.sh`). It is found
at `QEMU_GLIDE_LIB`, else `build/glide/libglide2x.so`, else the loader's
path; the line `glidept: wrapper <path>` says which. It renders into the
same window-less context as the GL pass-through, so Glide frames go
through the shader chain like any other. `GLIDE_HOST_LOG=<path|->` turns
on its own log. Guest side: `GLIDE\` on the ISO (`SETUP.EXE` installs it).

### OpenGL pass-through (doc 12)

The guest's `OPENGL32.DLL` (`OPENGL\` on the ISO, `SETUP /GAME 3`) is
qemu-3dfx's wrapper; it reaches the device through the mapper the Glide
component installs. It reads **`WRAPGL32.EXT` from the game's own
folder**, which ships beside it and holds `ExtensionsYear,1997`: a modern
host reports several thousand characters of extension names and a title
of the 1990s reads that into a fixed buffer. GLQuake's is 4096 bytes and
it dies in an unknown module, having returned into the text of the list
(measured on `base98-br`). Raise the year for a later game, or delete the
file. `SETUP /GAME` never overwrites one that is already there.

Host side, the frames go through the embed backend's dma-buf ring:

- `EMBED_ZC_PROBE=<n>` sets how often a buffer is checked for still being
  the memory it was made over: a known colour written into it through GL,
  read back with `gbm_bo_map` (doc 12 §4). A slot that fails is freed and
  made again — the repair, not a diagnostic. The default schedule is dense
  while the ring is young and one present in 512 after that; `=0` turns it
  off and `EMBED_ZC_HEAL=0` leaves a bad slot alone to study it.
- `EMBED_ZC_SLOTS=<n>` uses the first n of the ring's buffers (default:
  all of them).
- `EMBED_ZC_CHECK=<n>` reads four pixels out of the buffer just blitted
  into, every n-th present, twice: through GL and straight out of the
  buffer's memory. Weaker than the probe and easy to misread — while the
  guest's picture does not change, the two readings agree whether or not
  the buffer is being written. `EMBED_ZC_MARK=1` adds a line per present,
  for cutting a `FuncTrace,2` log to the window a slot went bad in.
- `tools/zc-vulkan-test.c` drives the ring with the frontend's Vulkan
  import and nothing else — no guest, no player, no wgpu — and checks each
  buffer's memory with the CPU after every frame. `--stage=` bisects the
  import, `--use=copy|shader` also reads it back through Vulkan,
  `--threaded` moves every Vulkan call to a thread of its own and
  `--draw=scene` renders instead of clearing. Every combination is clean,
  which is what rules the frontend out.
- `PLAYER_ZC_IMPORT=0` takes every dma-buf the backend offers and imports
  none of it. Declining one turns the ring off, so this is the only way to
  run the ring with no Vulkan behind it — the run that showed the frozen
  slot has nothing to do with the frontend. The picture is wrong while it
  is set (every 3D frame falls back to the VGA surface); it is for reading
  the backend's `EMBED_ZC_CHECK` lines.
- `EMBED_ZC_SETTLE=<ms>` waits that long after offering a buffer to the
  frontend before using it. Accepting an offer only queues it — the
  import happens later, on the frontend's own thread — so the first blits
  race it. This says whether that race is the cause. It is not.

## The launcher's front ends

Everything the launcher *decides* — the `machine.toml` format, the
machine library, the disc shelf, snapshots, shader profiles, the
preview's render path, and every window's own state machine and the
sentences it shows — lives in one crate, **`launcher-core`**. The front
end over it is a view: it draws and forwards events, and nothing else
(doc 07). **`launcher-qt` is the launcher every package installs** as
`2ksbox` (ADR-015). (An egui front end over the same core was retired on
2026-09-13, ADR-017.)

```sh
cd launcher-qt && cargo build --release # Qt 6 / QML through cxx-qt; what ships
```

The toolkit-free debug verbs — `--print-args`, `--new`, `--discs`,
`--host-check`, `--wizard-edit`, everything in `launcher_core::cli` —
are a binary of their own, so a scripted check needs no toolkit:

```sh
cargo build --release -p launcher-core --bin launcherx
target/release/launcherx --print-args ~/.local/share/2ksbox/machines/xp/machine.toml
```

`launcherx` is what `scripts/test.sh` and `tools/dos-guest-test.py` drive
the launcher through. What it cannot do is what *is* a toolkit: the
headless frame grabs of real windows, which are `launcher-qt`'s
(`QT_QPA_PLATFORM=offscreen`, doc 07).

`launcher-qt` declares its own workspace, so a plain `cargo build` at the
root never needs Qt 6 development files — `scripts/build.sh` has a `qt`
stage for it instead, and skips that stage (and with it any package) on a
host with no Qt 6. Building it is the whole build command — no CMake;
`cxx-qt-build` finds Qt through `qmake6`. In a checkout the binary is
`launcher-qt/target/release/launcher-qt`, and it finds the player in the
root workspace's `target/release` (`LAUNCHER_PLAYER_BIN` overrides).

Because the core is a real library, a front end in another language is a
view over it too. **`launcher-capi`** is a C ABI over the same models —
opaque handles, index-addressed rows, caller-owned strings — for a native
macOS app in Swift, or anything that speaks C:

```sh
cargo build -p launcher-capi            # liblauncher_capi.{a,so}; not a default member
cc -Ilauncher-capi/include my_frontend.c target/debug/liblauncher_capi.a -lstdc++ -lm -ldl -lpthread
```

`launcher-capi/include/launcher_core.h` is the header;
`launcher-capi/examples/smoke.c` is a working miniature front end, and is
the `capi` check in `scripts/test.sh`.

## Testing

**No unit tests. Integration and end-to-end tests only** (`CLAUDE.md`
has the policy and the full table of tools). `scripts/test.sh` runs the
suite: `host` (default, ~30 s: everything without a guest) or `all` (adds
the guest stage, ~2 min: XP headless on the D3D device from a snapshot of
`~/vms/winxp.qcow2`, plus the DOS x87 battery). Outputs land in
`build/test/`. Run `scripts/test.sh all` before every commit that touches
QEMU, the embed library, the D3D device or the guest DLLs. The suite is
local only by decision: CI will never run it (it needs the guest images
and a GPU).

CI (`.github/workflows/ci.yml`) is currently manual-only — trigger it
from the Actions tab (`workflow_dispatch`).

## Packaging

Everything is named **2ksbox** (ADR-011): the repository, the installed
commands, the user's data directory. The install layout every package
shares is in doc 07 ("The install layout").

### Linux tarball

```sh
scripts/package-linux.sh              # build/package/2ksbox-<version>-linux-<arch>.tar.zst
scripts/package-linux.sh --with-shaders   # + the ~80 MB preset collection
```

It stages the launcher, the player, the embed library, our `qemu-img`,
QEMU's firmware and the guest-tools ISO into one relocatable prefix
(doc 07's install layout), checks that the staged launcher resolves all of
them *inside* the package with a scrubbed environment — and that it opens
a real window offscreen, which is the only way to find out whether Qt's
plugins and QML modules are there — and rolls a tarball. **Qt 6 is not in
the tarball**: it needs the distribution's `qt6-base` and
`qt6-declarative` (Debian/Ubuntu: `libqt6quick6` plus the
`qml6-module-qtquick-*` packages), and `install.sh` names them if the
loader cannot find them. The extracted tree runs where it lands —
`bin/2ksbox` — and the `install.sh` inside it copies the tree into a
prefix (`~/.local` by default) and adds a desktop entry
(`com._2ksbox.Launcher.desktop`, the application ID the launcher's window
also reports as its `app_id`).

The tarball ships no system libraries, so it wants a host much like the
one that built it. The Flatpak is the portable answer.

### Flatpak

```sh
scripts/package-flatpak.sh          # build, install --user, smoke check
flatpak run com._2ksbox.Launcher
```

It builds everything from source against `org.kde.Sdk` (KDE's runtime,
because that is where Qt 6 comes from; it is `org.freedesktop.Platform`
25.08 underneath), so the ~191 libraries and Qt itself come from the
runtime. Set `FLATPAK_BUILD_DIR` (and flatpak's own `FLATPAK_USER_DIR`)
if the build tree — a whole QEMU plus a release Rust workspace, ~12 GB —
should not land on your root filesystem.

The Flatpak builds with no network, as Flathub requires: every crate is a
declared source with a checksum in `packaging/flatpak/cargo-sources.json`.
Run `scripts/gen-flatpak-cargo-sources.sh` and commit the result whenever
a dependency changes.

### macOS (`2ksbox.app` / `.dmg`)

Built natively on Apple Silicon (`scripts/package-macos.sh`). The bundle
includes the full non-system dylib closure (with unused Qt modules pruned
and dyld `@rpath` resolution verified), the OpenGLide wrapper, the
Direct3D executor, and the LunarG Vulkan loader + KosmicKrisp ICD. Signed
for Developer ID with the hardened runtime and the
`com.apple.security.cs.allow-jit` entitlement, notarized and stapled.
Every stage targets Homebrew's floor (the oldest macOS Homebrew supports,
`scripts/macos-floor.sh`). Details in [build-macos.md](build-macos.md).

### Windows (`.zip`)

Cross-built from Linux via a Fedora mingw-w64 container
(`scripts/win-cross.sh --build`, `scripts/build-windows.sh`,
`scripts/package-windows.sh`). Packages `2ksbox.exe` (Qt launcher),
`2ksbox-player.exe`, `libqemu-embed-i386.dll`, `d3dpt_exec.dll`,
`qemu-img.exe`, WHPX acceleration, firmware, and guest tools into a
portable zip. Details in [build-windows.md](build-windows.md).

## Diagnostics and logs

`launcher --paths` prints where a given build looks for each
companion — the first thing to ask when something says a file is missing;
`launcher --diagnose` prints the same plus this host's 3D and *files* it
in the launcher's own log (`launcher.log`, beside the machine library),
which is what to send when the launcher itself did not come up. On
Windows, where the launcher is a windowed program with no stdout at all,
`2ksbox-debug.bat` in the package does that for you.

The Qt front end draws in **light colours whatever the desktop is set
to** — its Quick Controls style paints controls light and takes only the
surfaces around them from the palette, so a dark system palette gets you
half a theme. `LAUNCHER_QT_SCHEME=system` hands the desktop's own palette
back and `=dark` forces the other one; `launcher.log` records the style
and the colours a run actually got.

## Licensing, for packagers

GPL-2.0, non-negotiable in practice for everything that links QEMU
(GPL-2.0) in-process: the `player`, `qemu-embed`, and `libdisc`, which is
compiled into QEMU itself.

The **launcher** — `launcher-core` and the front ends over it
(`launcher-qt`, `launcher-capi`) — and the `shader-chain` crate it shares
with the player are **GPL-2.0-or-later** (ADR-009). None of them links
QEMU code, since the launcher spawns the player as a separate process,
and they do link Apache-2.0 crates (`ring` under `ureq`'s rustls, among
others) that GPLv2 cannot take and GPLv3 can. `launcher-qt` links Qt 6
under the LGPLv3, which is the same reason.

Original code is Rust wherever possible (ADR-004 in
[decision records](10-decisions.md)); C only inside QEMU/qemu-3dfx and in
guest-side era code. The GPLv2 text is in `COPYING`, and every
third-party component is listed in `THIRD-PARTY-NOTICES.md`.

**If you package or redistribute the player, read this.** Its dependency
tree contains **Apache-2.0-only** crates — `winit`, `cpal`, `ab_glyph`,
`codespan-reporting` and `rspirv` among them — and Apache-2.0 is
incompatible with GPLv2, which the player is pinned to because it links
QEMU. This is a property of the modern Rust GUI stack rather than a
dependency we chose carelessly (`winit` alone settles it), and removing the
crates individually would change nothing: being clean means dropping wgpu
and librashader, i.e. the CRT shader chain the project exists for. **We
ship player binaries anyway**, with complete source and build scripts, and
the reasoning — including the alternatives measured and rejected — is
ADR-010. If your distribution's policy can't accept that, please open an
issue rather than patching around it; the clean fix (QEMU in its own
process) is designed and costed, not hypothetical.
