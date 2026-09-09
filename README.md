# 2ksbox

An open-source, cross-platform stack for running Windows 98 and Windows XP as
"native vintage boxes": hardware-accelerated period 3D (Direct3D / Glide /
OpenGL), pixel-accurate CRT-shaded video output, and faithful CD-ROM drive
emulation that works with raw disc dumps — including era copy protection
(SafeDisc, SecuROM, etc.) — of discs you own.

Built on QEMU. Runs on Linux, Windows, and macOS, with Apple Silicon as a
first-class target.

## What exists vs. what we build

| Piece | Status |
|---|---|
| x86 emulation/virtualization | Exists — QEMU fork (slimmer: 93 shared libraries; custom TCG fast paths for x87, SSE, SIMD, REP string, same-value SMC, and inline TB lookup; KVM/WHPX on x86) |
| Guest 3D acceleration | **We build & integrate** — Paravirtual Direct3D device (`d3dpt`, DXVK native host executor) + qemu-3dfx GL pass-through + OpenGLide host Glide wrapper |
| Guest display drivers | **We build** — Native `d3dpt-vga` drivers: XP miniport + display driver with DirectDraw/Direct3D DX8 DDI; Win98 mini-VDD + 16-bit DIB engine driver; SoftGPU/WineD3D as fallback |
| CRT shader ecosystem | Exists — libretro slang shaders via librashader (library, not RetroArch) |
| **Player: in-process QEMU + pixel-accurate CRT-shaded display** | **We build** (Rust, wgpu + librashader, mode analysis, event-driven geometry, low-latency audio) |
| **Companion launcher (library, creation wizard, disc shelf)** | **We build** (Rust: `launcher-core` library; shipped `launcher-qt` in Qt 6 / QML via cxx-qt; maintained `launcher` in egui; `launcher-capi` for C/Swift) |
| **Raw CD-ROM backend (cue/bin, subchannel, C2, CD-DA, dir-as-CD)** | **We build** (Rust "libdisc"; ATAPI patches; live disc shelf; `isodir:` directory mounting) |
| **Guest machine families** | **We build** — Win98, XP, DOS (with cycle-throttled CPU rates), and Other (BeOS, period Linux, OS/2) |

Authentic-hardware Win98 emulation (real Voodoo, real S3) is 86Box's territory
and explicitly **out of scope** — we don't duplicate that work.

## Design docs

0. [**Status and how to resume**](docs/00-status.md) — read first
1. [Goals and non-goals](docs/01-goals.md)
2. [Architecture: in-process QEMU, process model, threading](docs/02-architecture.md)
3. [Display pipeline: pixel accuracy, CRT shaders, latency](docs/03-display-pipeline.md)
4. [3D acceleration: qemu-3dfx, paravirtual D3D, and guest drivers](docs/04-3d-acceleration.md)
5. [CD-ROM backend: raw images, copy protection, and directory discs](docs/05-cdrom-backend.md)
6. [Guest machines: Win98, XP, DOS, and Other reference configs](docs/06-guest-machines.md)
7. [Frontend: machine library, UX, input, audio, and packaging](docs/07-frontend.md)
8. [Roadmap and milestones](docs/08-roadmap.md)
9. [Reference hardware rig](docs/09-reference-hardware.md)
10. [Decision records (ADRs 001–015)](docs/10-decisions.md)
11. [M1 embed API design](docs/11-m1-embed-api.md)
12. [M3 window-less GL and Glide context provider design](docs/12-m3-context-provider.md)
13. [x87 shadow doubles: the FPU stack as host doubles in TCG](docs/13-x87-inline-tcg.md)
14. [Paravirtual Direct3D device for XP and Win98](docs/14-d3d-paravirt.md)
15. [A real XP display driver: d3dpt-vga, miniport + Direct3D DDI](docs/15-guest-display-driver.md)
16. [SSE on the host FPU: scalar and packed ops inline in TCG](docs/16-sse-inline-tcg.md)
17. [CD-ROM backend: implementation specification](docs/17-cdrom-implementation.md)
18. [Pinned guest registers: the x86 register file in TCG](docs/18-pinned-guest-registers.md)
19. [A native Win98 display driver: d3dpt9x](docs/19-win9x-display-driver.md)

### Platform build guides and tracks
- [Building and packaging on macOS (Apple Silicon)](docs/build-macos.md)
- [Building and packaging for Windows (cross-build from Linux)](docs/build-windows.md)
- [Parallel development tracks](docs/tracks/) (M4, M5, M5g, M6, M7, M8, M9, M10, M11)

## Building

```sh
git clone --recurse-submodules https://github.com/davidrios/2ksbox
cd 2ksbox                    # submodules: qemu (gitlab.com, pinned v9.2.4),
                             #             third_party/qemu-3dfx (github.com)
# already cloned without submodules? → git submodule update --init --depth 1

scripts/build.sh             # everything, in order: qemu, rust, dxvk, the D3D
                             # executor, the guest-tools ISO. This is the command
                             # after every pull; `--test` chains the host test
                             # stage, `--help` lists the individual stages.
target/release/player        # M0: native window with test pattern (integer-scaled 4:3)

# What build.sh runs, for driving a single stage by hand:
scripts/prepare-qemu.sh      # overlay qemu-3dfx devices + embed/, patches, sign
scripts/configure-qemu.sh    # configure (uv-managed python — needs uv, ninja, glib, pixman)
ninja -C build/qemu qemu-system-i386 qemu-img qemu-io libqemu-embed-i386.so   # .dylib on macOS
cargo build --release        # player links libqemu-embed (rpath into build/qemu)

# M1: boot something in-process (firmware path needed until machine bundles land)
target/release/player -- -L $PWD/qemu/pc-bios -machine pc -m 32 \
  -drive file=path/to/floppy.img,format=raw,if=floppy -boot a -vga std -net none
# PLAYER_DUMP=frame.png PLAYER_DUMP_SEQ=150 dumps guest frame #150 and exits (headless check)
# --shader <preset.slangp> (or PLAYER_SHADER=) runs a libretro slang preset, e.g.
#   target/release/player --shader third_party/slang-shaders/crt/crt-lottes.slangp -- ...
# --shader-params <name=value,...> (or PLAYER_SHADER_PARAMS=) overrides the preset's own
#   parameter defaults by name, e.g. --shader-params BRIGHTBOOST=1.4,GAMMA_INPUT=2.4 — this is
#   what a launcher shader profile (launcher-core/src/shader_profile.rs) resolves to
# PLAYER_DUMP_OUT=out.png dumps the shaded frame (GPU readback) at PLAYER_DUMP_SEQ and exits
# PLAYER_KEYS="120:enter,360:ctrl+g" presses keys/chords at guest frames (headless input test);
#   each press is held PLAYER_KEYS_HOLD frames (default 6 ≈ 100 ms) — a down+up in one flush is a
#   zero-length press that a game polling the keyboard state never sees
# LIBSYNTH_SF2=<file.sf2> is the General MIDI bank the machine's MIDI port plays through
#   (doc 20). The player sets it itself — the packaged bank, or `soundfonts/` in a checkout —
#   so this is only for trying another bank; a machine that names its own wins over both.
# LIBSYNTH_MT32_ROMS=<dir> the same for the Roland CM-32L's ROMs, which are the user's own
# PLAYER_AUDIO_NULL=1 keeps the audio ring without a device and logs QEMU's writes
# PLAYER_AUDIO_MS=60 (default) is the audio cushion QEMU keeps ahead of the host audio
#   thread: the output latency, and how late QEMU's main loop may run (TCG, the D3D
#   executor) before a gap is heard. `qemu-embed: audio:` lines on stderr count gaps
#   and dropped audio when they happen; raise it if they do, lower it under KVM.
# --calib <bmp|dir> shades doc 09's CRT calibration patterns (tools/crtcal-render
#   writes them; TESTS\CRTCAL.EXE puts the same ones on a real tube) and exits
# --mode-sweep <dir> runs doc 03's mode sweep instead of a guest: every mode in the
#   table, the geometry stage and the preset checked, a PNG of each dumped there
# PLAYER_MODE_PARAMS=0 is the A/B control for mode analysis — the preset is left to
#   guess the scanline count from the framebuffer height, as it did before M2
# Ctrl+Alt+S writes the guest's own frame — its native size, no geometry stage and
#   no CRT chain — as PLAYER_SHOT_DIR/2ksbox-NNNN.png (the next free number; the
#   working directory when PLAYER_SHOT_DIR is unset). Ctrl+Alt+G releases the grab.
# Gamepad (M13, docs/tracks/m13-gamepads.md). `player --pads` says what this host
#   can read, which is the one place a build without the `gilrs` feature or a
#   sandbox with no /dev/input reports itself.
# --pad usb (or PLAYER_PAD=usb) sends the pad to the machine's `usb-gamepad`
#   (patch 26): two analog sticks, an 8-way hat and twelve buttons, which XP,
#   Windows 98 SE and Me all see through their own HID driver with nothing
#   installed — DirectInput and joy.cpl find it on the first start after the
#   device is added. The launcher adds `-usb -device usb-gamepad` for a machine
#   whose `pad = "usb"`. Not offered on DOS, which has no USB stack.
# --pad keys (or PLAYER_PAD=keys) maps the pad onto the keys the player already
#   sends: d-pad and left stick are the arrows, the four face buttons are Ctrl,
#   Alt, Space and Enter, Start is Esc. The launcher writes it from the machine's
#   own setting (`pad` in the bundle), the way it writes --shader. Works on every
#   guest, because there is no device for the guest to support; a game that asks
#   DirectInput for a joystick still finds none — that needs the USB gamepad.
#   `launcherx --print-player-args <machine.toml>` shows what a bundle resolves to.
# PLAYER_PAD_SCRIPT="30:lx=1.0,45:south=1,51:south=0" is a synthetic pad: set a
#   control to a value at a guest frame number. Frames, not milliseconds, so a run
#   lands in the same place in the guest's execution every time (as PLAYER_KEYS does).
#   Controls: lx/ly/rx/ry (axes, -1.0..1.0; negative is left/up), south/east/west/
#   north, dpad_up/down/left/right, l1/r1/l2/r2, l3/r3, select/start (0 or 1).
#   It wins over real hardware, so a test is not perturbed by what is plugged in.
# PLAYER_PAD_LOG=1 prints every shaped reading with its press/release transitions
# PLAYER_PAD_SHAPING="0.30,0.55,0.40" overrides deadzone,press,release — the press
#   and release thresholds differ on purpose, and release must be the lower of the
#   two: with one number a stick held at it chatters at the poll rate
# player --pad-sweep <frames> replays PLAYER_PAD_SCRIPT with no window, no QEMU and
#   no guest, and prints what came out — with --pad keys, the key presses too.
#   The `pad` check in scripts/test.sh
# PLAYER_LATENCY=1 prints publish→present latency percentiles every 240 guest frames
# PLAYER_REFRESH_LOG=1 prints a guest frame counter every 100 frames — whether the
#   guest is drawing at all. Off by default: a machine left running printed it for
#   as long as it was up, which buries the lines that mean something
# PLAYER_REFRESH_MS=16 (default) is the guest frame pull interval (QEMU's own default is 30)
# QMP: the player always attaches a control monitor over a socketpair (no socket file).
#   PLAYER_QMP=1 logs every QMP event (SHUTDOWN/RESET/STOP/... are logged regardless)
#   PLAYER_QMP_EXEC='{"execute":"query-status"}' (or a JSON array of requests) runs
#   commands once the guest has drawn its first frame and prints the replies
# Direct3D pass-through (doc 14): the d3dpt device is always present; it loads
#   build/d3dpt/libd3dpt_exec.so (D3DPT_EXEC_LIB) and DXVK (D3DPT_DXVK_LIB) on the
#   guest's first use. Build: scripts/prepare-dxvk.sh && scripts/configure-dxvk.sh &&
#   ninja -C build/dxvk && scripts/build-d3dpt-exec.sh; guest side: D3DPT\ on the ISO.
#   D3DPT_DUMP_DIR=dir D3DPT_DUMP_EVERY=60 makes the executor write every 60th
#   presented frame as dir/frame-NNNNNN.ppm (works with bare qemu-system-i386 too).
#   Guest side: D3DPT_TRACE=1 or a file d3dpt_trace.on next to the DLL writes the
#   creation/lock/upload/present calls to d3d8_trace.log / d3d9_trace.log; a DLL
#   that cannot open the device forwards Direct3DCreateN to the system DLL.
#   While the device is active the player shows the VGA surface again after 1 s
#   without a presented frame if the guest drew on it (a game's error dialog,
#   a DirectShow movie, a crashed process): "[display] no 3D frame for …".
# Glide pass-through (doc 12 §5): the guest's GLIDE2X.DLL reaches a host-side
#   wrapper QEMU dlopens at grGlideInit -- qemu-3dfx ships none, so ours is
#   OpenGLide: scripts/prepare-openglide.sh && scripts/build-glide.sh. It is found
#   at QEMU_GLIDE_LIB, else build/glide/libglide2x.so, else the loader's path;
#   the line "glidept: wrapper <path>" says which. It renders into the same
#   window-less context as the GL pass-through, so Glide frames go through the
#   shader chain like any other. GLIDE_HOST_LOG=<path|-> turns on its own log.
#   Guest side: GLIDE\ on the ISO (SETUP.EXE installs it).
# audio: the player adds -audiodev embed,id=embed0 automatically; attach e.g.
#   -machine pc,pcspk-audiodev=embed0   or   -device sb16,audiodev=embed0
```

macOS / Apple Silicon specifics: [docs/build-macos.md](docs/build-macos.md).

## The launcher's front ends

Everything the launcher *decides* — the `machine.toml` format, the
machine library, the disc shelf, snapshots, shader profiles, the
preview's render path, and every window's own state machine and the
sentences it shows — lives in one crate, **`launcher-core`**. There are
two maintained front ends over it, and both are views: they draw and
forward events, and nothing else (doc 07). **`launcher-qt` is the one
every package installs** as `2ksbox` (ADR-015); `launcher` is kept, and
installed by nothing.

```sh
cd launcher-qt && cargo build --release # Qt 6 / QML through cxx-qt; what ships
cargo build --release -p launcher       # egui/eframe; the second view, not a default member
```

Neither is a *default* member of the root workspace: `launcher` left
`default-members` on 2026-09-07, because it costs 70
crates nobody else needs (eframe, accesskit, harfrust, icu) for a binary
no packager installs. `scripts/build.sh`'s `rust` stage builds the
default members and then `cargo check --release --workspace`, so the
front end still cannot rot unnoticed.

The toolkit-free debug verbs — `--print-args`, `--new`, `--discs`,
`--host-check`, `--wizard-edit`, everything in `launcher_core::cli` —
are a binary of their own, so a scripted check needs neither toolkit:

```sh
cargo build --release -p launcher-core --bin launcherx
target/release/launcherx --print-args ~/.local/share/2ksbox/machines/xp/machine.toml
```

`launcherx` is what `scripts/test.sh` and `tools/dos-guest-test.py` drive
the launcher through. The verbs it cannot answer are the two that *are* a
toolkit: `--pick-file` / `--pick-folder` (a real OS dialog) and the
`--diag-*` frame grabs.

`launcher-qt` declares its own workspace, so a plain `cargo build` at the
root never needs Qt 6 development files — `scripts/build.sh` has a `qt`
stage for it instead, and skips that stage (and with it any package) on a
host with no Qt 6. Building it is the whole build command — no CMake;
`cxx-qt-build` finds Qt through `qmake6`.

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

## Packaging (Linux)

Everything is named **2ksbox** (ADR-011): the repository, the installed
commands, the user's data directory.

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
loader cannot find them. The extracted tree runs where it lands — `bin/2ksbox` — and the
`install.sh` inside it copies the tree into a prefix (`~/.local` by
default) and adds a desktop entry (`com._2ksbox.Launcher.desktop`, the
application ID the launcher's window also reports as its `app_id`):

```sh
tar xf 2ksbox-*.tar.zst && cd 2ksbox-*
./install.sh                 # or --prefix /usr/local, or --uninstall
```

The tarball ships no system libraries, so it wants a host much like the
one that built it. The **Flatpak** is the portable answer — it builds
everything from source against `org.kde.Sdk` (KDE's runtime, because that
is where Qt 6 comes from; it is `org.freedesktop.Platform` 25.08
underneath), so the ~191 libraries and Qt itself come from the runtime:

```sh
scripts/package-flatpak.sh          # build, install --user, smoke check
flatpak run com._2ksbox.Launcher
```

Set `FLATPAK_BUILD_DIR` (and flatpak's own `FLATPAK_USER_DIR`) if the
build tree — a whole QEMU plus a release Rust workspace, ~12 GB — should
not land on your root filesystem.

The Flatpak builds with no network, as Flathub requires: every crate is a
declared source with a checksum in `packaging/flatpak/cargo-sources.json`.
Run `scripts/gen-flatpak-cargo-sources.sh` and commit the result whenever
a dependency changes.

## Packaging (macOS and Windows)

- **macOS (`2ksbox.app` / `.dmg`):** Built natively on Apple Silicon (`scripts/package-macos.sh`). The bundle includes the full non-system dylib closure (with unused Qt modules pruned to save 6 MB and dyld `@rpath` resolution verified), the OpenGLide wrapper, the Direct3D executor, and the LunarG Vulkan loader + KosmicKrisp ICD. Signed for Developer ID with the hardened runtime and `com.apple.security.cs.allow-jit` entitlement, notarized and stapled. Details in [docs/build-macos.md](docs/build-macos.md).
- **Windows (`.zip`):** Cross-built from Linux via a Fedora mingw-w64 container (`scripts/win-cross.sh --build`, `scripts/build-windows.sh`, `scripts/package-windows.sh`). Packages `2ksbox.exe` (Qt launcher), `2ksbox-player.exe`, `libqemu-embed-i386.dll`, `d3dpt_exec.dll`, `qemu-img.exe`, WHPX acceleration, firmware, and guest tools into a portable zip. Details in [docs/build-windows.md](docs/build-windows.md).

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

CI (`.github/workflows/ci.yml`) is currently manual-only — trigger it from the
Actions tab (`workflow_dispatch`).

## License

GPL-2.0, non-negotiable in practice for everything that links QEMU
(GPL-2.0) in-process: the `player`, `qemu-embed`, and `libdisc`, which is
compiled into QEMU itself.

The **launcher** — `launcher-core` and the front ends over it
(`launcher`, `launcher-qt`, `launcher-capi`) — and the `shader-chain`
crate it shares with the player are **GPL-2.0-or-later** (ADR-009). None
of them links QEMU code, since the launcher spawns the player as a
separate process, and they do link Apache-2.0 crates (egui's `ab_glyph`,
winit's `dpi`, `ring` under `ureq`'s rustls) that GPLv2 cannot take and
GPLv3 can. `launcher-qt` links Qt 6 under the LGPLv3, which is the same
reason.

Original code is Rust wherever possible (see ADR-004 in
[decision records](docs/10-decisions.md)); C only inside QEMU/qemu-3dfx and
in guest-side era code. The GPLv2 text is in [COPYING](COPYING), and every
third-party component is listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

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
