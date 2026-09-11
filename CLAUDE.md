# 2ksbox — working notes for Claude (and anyone else)

Open-source, cross-platform stack that runs Windows 98 / XP as "native
vintage boxes": a patched QEMU (qemu-3dfx for 3D) embedded **in-process**
in a Rust player, with a CRT shader chain, plus a launcher and a CD-ROM
backend later.

## Start here

- `docs/00-status.md` — the maintained handoff: state table, build cheat
  sheet, open threads, ordered next steps, gotchas. Read it first.
- `docs/tracks/` — one doc per parallel work track (M4 Direct3D device,
  M7 XP display driver): scope, owned files, state, test loop, next
  steps. A session picks one track and follows the rules in the status
  doc's "Tracks" section.
- `docs/01`–`12` design docs; decisions/ADRs in `docs/10`; roadmap `docs/08`.
- `patches/qemu/README.md` — every QEMU patch, what it does, when to drop it.
- `docs/build-macos.md` — Apple Silicon specifics (the M1 Air is the
  Apple test machine; the reference rig in `docs/09` is the oracle).
- `docs/build-windows.md` — the Windows build, which is a **cross build
  from Linux** in a container (`scripts/win-cross.sh`,
  `scripts/build-windows.sh`, `scripts/package-windows.sh`). Windows
  artefacts go to `build/win/` and
  `target/x86_64-pc-windows-gnu/`, never over the native ones.

## Locked decisions (do not reopen)

- **The project is named `2ksbox`** (`2ksbox.com`, ADR-011, 2026-09-05;
  the rename finished 2026-09-06). It names everything: the repository
  (`github.com/davidrios/2ksbox`), this checkout, the docs, the installed
  commands `2ksbox` / `2ksbox-player`, the resource dirs `share/2ksbox`
  etc., and the user's data directory `~/.local/share/2ksbox` — moved
  from the old `win98-xp-virt` one exactly once by
  `launcher-core/src/paths.rs::data_dir()`, an atomic rename that only
  happens when the new name is absent. The application ID is
  `com._2ksbox.Launcher` (the underscore is required: a name segment may
  not start with a digit).
- QEMU base, our own fork as a **patch queue** on the pinned submodule
  (v9.2.4 + qemu-3dfx). Not VMware/VirtualBox/86Box.
- QEMU runs **in-process** (`libqemu-embed-<target>`, `embed/`) for latency.
- **Standalone Rust player + launcher.** RetroArch/libretro was tried and
  rejected — never propose it again.
- **The launcher is two front ends over one library** (ADR-014,
  2026-09-06, doc 07): `launcher-core/` holds everything it *decides* — the bundle
  format, the machine library, the disc shelf, snapshots, shader
  profiles, the preview's render path, **and every window's own state
  machine and the sentences it shows** — while `launcher/` (egui) and
  `launcher-qt/` (Qt 6 / QML) are views over it, both maintained.
  Nothing that a second front end could get differently goes in a front
  end: not a default that follows the family, not a note under a
  checkbox, not a combo box's labels. Every toolkit-free debug verb is
  `launcher_core::cli`, so both binaries answer them identically — and so
  does `launcherx`, `launcher-core`'s own toolkit-free binary, which is
  what `scripts/test.sh` and `tools/dos-guest-test.py` drive. Since
  2026-09-07 `launcher` (egui) is **not a default workspace member**:
  `cargo build --release` skips it and its ~70 exclusive crates,
  `scripts/build.sh` follows with `cargo check --release --workspace` so
  it still cannot rot, and `cargo build -p launcher` builds it.
  `launcher-capi/` is the same thing as a C ABI, for a front end in
  another language. `launcher-qt` is not in the root workspace, so
  `cargo build` never needs Qt 6.
- **The Qt front end is the one that ships** (ADR-015, 2026-09-07): every
  packager installs `launcher-qt` as `2ksbox` — Linux, the Flatpak (which
  moved to `org.kde.Platform` 6.10 for Qt), macOS (`macdeployqt` before
  our own dylib closure, `-qmldir=launcher-qt/qml` because our QML is a
  Qt resource) and Windows (the DLLs, `plugins\platforms\qwindows.dll`
  and the `qml\` trees, all staged by hand: there is no cross
  `windeployqt`). `launcher/` (egui) stays maintained and is installed by
  nothing. `scripts/build.sh` has a `qt` stage in its default set; a host
  with no Qt 6 builds everything else and can roll no package. Every
  packager also opens a **real window offscreen**
  (`QT_QPA_PLATFORM=offscreen` + `LAUNCHER_QT_SHOT`) and requires a PNG,
  because Qt's platform plugin and QML modules are named in no import
  table and their absence is invisible to every other check.
- Rust wherever possible; C only inside QEMU/qemu-3dfx and guest-side
  era code. Python is uv-managed (3.12; 3.14 breaks QEMU's venv).
- Everything open source; Apple Silicon must work (TCG), not just x86 hosts.
- **Direct3D 8/9 on XP is our own paravirtual device** (doc 14, ADR-006):
  guest serializer DLLs + native host executor (DXVK). Protocol
  `d3dpt/d3dpt_proto.h` is the one header for guest DLL, QEMU device and
  executor; bump `D3DPT_PROTO_VERSION` on any change. WineD3D-in-guest is
  the fallback/DX7 path only; don't sink more time into wine9x bugs. The
  reference workload is `guest-tools/src/d3dgame9.c` / `d3dgame8.c`,
  golden on the rig first.
- **A hardware Vulkan 1.3 device is the executor's floor, and WineD3D is
  never retired** (ADR-013, 2026-09-06). `third_party/dxvk` v3.1 sets
  `DxvkVulkanApiVersion = VK_API_VERSION_1_3` and enforces it both at
  instance creation and per adapter, so pre-Broadwell Intel, Nvidia Kepler
  and older, AMD TeraScale, and macOS before 26 / every Intel Mac are below
  it — all of them otherwise fine 2ksbox hosts, which is why they keep the
  OpenGL pass-through with WineD3D *in the guest*, which needs no Vulkan at
  all. Never propose deleting the `WINED3D\` ISO folder, `SETUP /GAME`'s
  renames or doc 04/08's fallback rows. **Software Vulkan is used, not
  refused**: DXVK ranks a CPU device last but never excludes it, so
  lavapipe works — the launcher reports "available, in software (slow)" and
  says WineD3D may beat it. Deciding for the user which of two working
  stacks is too slow was the mistake; warn and let the box answer. The
  probe is `launcher-core/src/host_gpu.rs` / `launcher --host-check`, and
  its sentence for the wizard is the shared `wizard::Form::graphics_note()`,
  never a front end's own.
- **The host-side Glide wrapper is our own build of OpenGLide** (doc 12 §5,
  2026-09-06). qemu-3dfx's `hw/3dfx` only *dispatches* -- it `dlopen`s a
  `libglide2x` and looks up 183 entry points -- and upstream ships that
  library to donors only, so Glide had never worked here at all. OpenGLide
  (LGPL) is pinned at `third_party/openglide` with a patch queue and the
  window-less platform layer in `glidept/`; it renders into the embed
  backend's own context (patch 33's `glide_host_ops` reverses upstream's
  window handshake) rather than opening a window. Don't propose nGlide or
  dgVoodoo2: closed source, and D3D-targeted. `patches/openglide/README.md`
  has the argument, the guest-side alternatives included. **DOS Glide games
  bind the same device through qemu-3dfx's `GLIDE2X.OVL`** (2026-09-10:
  Carmageddon's 3dfx build from a Win98 DOS box) — `build-wrappers.sh`
  builds it with Open Watcom, `SETUP.EXE` puts it in the Windows folder on
  9x, a DOS machine puts it next to the game; a Glide 2 game linked
  statically (a few 1996 titles) is the only kind it cannot serve.
- **XP's display adapter is our `d3dpt-vga` + real display driver** (doc
  15, ADR-008): `-vga none -device d3dpt-vga`, `guest-tools/src/d3dptvid/`
  (miniport + display DLL + INF, mingw-w64 DDK headers, no Microsoft DDK),
  `guest-tools/build-driver.sh`. Register set `d3dpt/d3dpt_fb.h` is shared
  by the QEMU device and the miniport; bump `D3DPT_FB_VERSION` on change.
  The driver's Direct3D DDI (M7c) reuses the doc 14 protocol and executor
  through a command window at the top of the adapter's VRAM; since
  2026-09-05 it is a DirectX 8 DDI (`D3DCAPS8`, hardware T&L, the DX8
  tokens rewritten by the driver into `D3DPT_DP2_DRAW8`, vertex / pixel
  shaders 1.x run on the host since protocol v7 — every function is
  validated against a vs/ps 1.x opcode table first, because DXVK asserts
  on garbage bytecode; palettized textures and colour keying since v8,
  both expanded to A8R8G8B8 on the host; vertex / index buffers in VRAM
  since v9 — a `DRAW8` names the buffer and offset, the host reads it
  from VRAM, `ddflags=0x100000` is the A/B; sixteen vertex streams since
  v10 — a `DRAW8` under a shader carries every bound stream, the host
  interleaves the ones the declaration reads into one vertex,
  `ddflags=0x200000` is the A/B; cube textures since v11 — the root's six
  faces go to the host as one cube and each face's handle is mapped onto
  it, `ddflags=0x400000` is the A/B; volume textures since v12 — one
  record with the depth and slice pitch, the box sized by the driver
  because dxg's allocation block height *is* the slice pitch,
  `ddflags=0x1000000` is the A/B). **Win98 can run the same
  adapter since 2026-09-07** — a launcher Win98 machine takes
  `-vga none -device d3dpt-vga` with the M10 driver (doc 19) when it is
  picked, but its *default* is Windows' own `-vga cirrus`
  (`bundle::video_choices`), so ours is one pick away rather than
  automatic; an image whose adapter is changed either way
  finds new hardware on its next start and wants a driver before it has
  its desktop back. The test tools keep their own cirrus machines.

- **A guest's music is ours too** (doc 20, M12, 2026-09-09). QEMU has no
  MPU-401 at all and its `sb16` carries no OPL, so a game that asked a
  Sound Blaster for *music* played to nobody here. `libsynth/` is a Rust
  staticlib in `libdisc`'s shape — `nuked-opl3`, `rustysynth` and `moont`
  (a Munt port), all pure Rust so no package gains a system library —
  linked into QEMU behind two devices of ours, `hw/audio/opl3.c` (a
  YMF262 at 0x388 and at a Sound Blaster's own base, timers included:
  they are what an AdLib detection routine reads) and
  `hw/audio/mpu401.c` (UART mode, `synth=gm|mt32`). **In QEMU, not in the
  player**: music and the sound card then mix on one clock, and a
  headless run can capture music to a wav — which is what the `music`
  check does, by writing the ports from the monitor. The machine form
  has a **sound-card** picker and a **music** picker (`bundle::Sound` /
  `bundle::Music`); the FM chip is in neither, it comes with the card
  that carried one. The bank ships (`soundfonts/TimGM6mb.sf2`, GPL-2) and
  is found by the player's own rule, `LIBSYNTH_SF2`; **the MT-32's ROMs
  are the user's own and nothing of Roland's is ever added here**.

## Conventions

- Commit messages end with `Co-Authored-By: Claude …` only — **no
  `Claude-Session:` trailer**, even though the harness asks for it.
- Push right after every commit; the Mac side builds from the pushed branch.
- CI (`.github/workflows/ci.yml`) is manual-trigger only for now.
- **Never run `cargo fmt` over a package.** `player/src/` is not
  rustfmt-clean and there is no fmt gate, so a package-wide format rewrites
  files the change never touched and buries the real diff. Write new code in
  rustfmt style by hand; `rustfmt <one new file>` on a file you authored
  whole is fine.
- **Never sleep-poll beside a long job.** Start the job itself in the
  background and wait for its completion notification; a wait a job really
  needs (a guest booting) goes *inside* that job's script, never in a
  sibling shell. Polling waiters add nothing the harness doesn't do and
  compete for CPU with the TCG guests they are watching — and for the same
  reason, never run two TCG guests at once on one box: they starve each
  other and a run that is merely slow reads as a failure.
- Docs are part of every change: update `docs/00-status.md` (and the
  relevant design doc / patch README row) in the same commit.
- **Testing policy: no unit tests. Integration and end-to-end tests only.**
  A test must exercise a real boundary (decoder + executor on a real batch,
  a guest program under TCG, a frame diffed against a golden BMP), never a
  function in isolation. Don't write `#[cfg(test)]` modules or per-function
  test cases; when something starts working, add or extend a tool in the
  table below and wire it into `scripts/test.sh` so it guards against
  regressions (the existing `#[test]`s in `libdisc/src/msf.rs` predate this
  policy; don't add more). Run `scripts/test.sh all` before every commit
  that touches QEMU, the embed library, the D3D device or the guest DLLs.

## Build / run

```sh
git clone --recurse-submodules --shallow-submodules <repo>
scripts/build.sh          # everything: qemu, rust, dxvk, the D3D executor, the guest ISO
scripts/build.sh --test   # ... and then the host test stage
```

`scripts/build.sh` is the one command. The rest of this section is what it
runs, for when a single stage has to be driven by hand:

```sh
scripts/prepare-qemu.sh && scripts/configure-qemu.sh
ninja -C build/qemu qemu-system-i386 qemu-img qemu-io libqemu-embed-i386.so   # .dylib on macOS
cargo build --release                       # default members (not the egui launcher)
cargo check --release --workspace           # `launcher` + `launcher-capi`, kept from rotting
(cd launcher-qt && cargo build --release)   # the launcher the packages ship; needs Qt 6
# configure-qemu.sh also builds libdisc (the CD-ROM model) and libsynth (the music
# engines, doc 20) and links both into QEMU (patches 50 and 60)
# Direct3D pass-through (doc 14) needs the executor too:
scripts/prepare-dxvk.sh && scripts/configure-dxvk.sh && ninja -C build/dxvk && scripts/build-d3dpt-exec.sh
# Glide pass-through (doc 12 §5) needs the host-side wrapper, which qemu-3dfx
# does not ship: scripts/prepare-openglide.sh && scripts/build-glide.sh
# (QEMU finds it through QEMU_GLIDE_LIB=build/glide/libglide2x.so)
guest-tools/build-wrappers.sh   # the guest-tools ISO (SETUP.EXE + the guest DLLs)
```

After **every** `git pull`, run `scripts/build.sh` — it works out what has
to be redone. It skips each prepare step whose inputs are unchanged (hashed
into `build/.stamp-*`), because a prepare re-applies its patch queue and so
hands the build system a few thousand fresh mtimes: running them
unconditionally costs a full QEMU rebuild every time, running them never
costs a stale tree. `-f` re-runs them all — reach for it if a tree was
edited by hand. A **`D3DPT_PROTO_VERSION` bump also makes the executor and
the guest-tools ISO stale**, and neither says so: the suite fails instead
as `d3dpt-dp2: protocol mismatch` and as a guest that never attaches.
`build.sh` rebuilds both, and when a host cannot (no mingw) its summary
says which artefacts are behind. What the stages are for:
`qemu/embed/` is an rsync copy of `embed/` made by `prepare-qemu.sh`, and a
stale copy links the player against an old library (`undefined symbol
_qemu_embed_…`; `qemu-embed/build.rs` warns). `configure-qemu.sh` must run
again whenever meson files changed (keeps `werror` off). On macOS
`MACOSX_DEPLOYMENT_TARGET` must be the same for configure and cargo
(`build.sh` exports it). `QEMU_PYTHON=<interpreter>` makes `configure-qemu.sh` use that one
and never consult uv (3.8–3.13 enforced) — for a sandboxed build that has
a Python already and cannot fetch one, i.e. the Flatpak. Player env knobs
(`PLAYER_*`) are listed in `README.md`.

## The QEMU patch queue

- `prepare-qemu.sh` is deterministic: it restores every tracked file any
  patch touches, re-applies the 3dfx overlay + patch, then our queue in
  filename order, then qemu-3dfx's `sign_commit`. **Never `git checkout`
  files inside `qemu/` by hand** between runs.
- New/regenerated patches must be **git-format diffs** (`git diff
  --no-prefix --no-index a b`; new files need `--- /dev/null`) and must be
  **forward-applied from a pristine worktree** before pushing — reverse
  checks against an edited tree prove nothing. Recipe in the patch README.
- Patches that touch overlay files (`hw/3dfx`, `hw/mesa`, `embed/`) rely
  on the overlay being refreshed first; prepare handles the order.
- Bumping the embed API: header `QEMU_EMBED_API_VERSION` and the
  `qemu-embed` crate's `API_VERSION` move together; every machine must
  rebuild the library before the player links.

## Testing tools

All integration / e2e (see policy above). `scripts/test.sh` runs them:
`host` (default, ~30 s: everything without a guest) or `all` (adds the
guest stage, ~2 min: XP headless on the D3D device from a snapshot of
`~/vms/winxp.qcow2`, plus the DOS x87 battery). **Local only, by
decision:** CI will never run the suite (it needs the guest images and a
GPU); don't propose wiring it in.

| Tool | What it proves |
|---|---|
| `scripts/test.sh [host\|guest\|all]` | the whole suite below, PASS/FAIL/SKIP per check, outputs in `build/test/`; `TEST_KEEP=1` leaves XP running on failure |
| `tools/guestwait.sh` (sourced by every guest tool) | that a guest test waits for the guest instead of sleeping. No tool has a fixed boot wait any more: `gw_poke_until` knocks on the Run dialog until the guest runs something and says so on COM1 (the only proof a shell is there), `gw_wait_log` / `gw_wait_count` watch for a line our own device wrote (`d3dpt-vga: linear mode on`; two of them is what proves a restart), `gw_wait_quiet` watches QMP `query-blockstats` for the machines with no serial line, and `gw_wait_sock` / `gw_wait_exit` bound the ends. Never a screendump — `vga_draw_text` draws over a dead machine. The old `BOOT_WAIT` / `WARMUP_WAIT` / `CMD_WAIT` are now **caps on giving up**, and every wait ends early if the guest exits (`GW_PID`). Measured 2026-09-07 on the Air under TCG: XP takes a command at ~26 s and Win98 at ~23 s, against sleeps of 45 to 180 |
| `SETUP.EXE` (guest-tools ISO root; `guest-tools/src/setup.c`) | installs the guest tools from inside the machine, and the reason the ISO's folders are what they are: one folder per role, one copy of every file, and SETUP knows which of them *this* Windows wants (98/Me: Glide + `FXMEMMAP.VXD`, and the `d3dpt-vga` display driver as three files dropped in `WINDOWS\INF` for PnP; 2000/XP: Glide + `FXPTL.SYS` with the MAPMEM service, and the same display driver through `DRVINST.EXE`). A console program, so a guest test drives it: `SETUP /ALL` installs everything applicable, `SETUP /LIST` prints the lists, `SETUP /GAME <n> <dir>` copies one per-game file set next to a game's EXE (that is where WineD3D's `WINED9.DLL` → `D3D9.DLL` renames happen, so the disc carries no second copy). Writes `SETUP.LOG` |
| `tools/setup-guest-test.sh <image> [xp\|win98]` | `SETUP.EXE` in a real guest, headless, on both families: `/LIST`, `/ALL`, `/GAME 3 C:\2KSBOX`, then **Windows' own `dir`** on everything that should now exist (and `net start MAPMEM` on NT) over COM1 — the installer's own exit code is not the evidence. `REBOOT=1` runs the other half instead — `SETUP /ALL /REBOOT`, and a second SeaBIOS banner on the debugcon is what proves the machine restarted (a 9x restart is a thing that has silently not worked: docs/00-status.md). XP boots on `-vga none -device d3dpt-vga` so the display-driver component has a device to bind to, and the QEMU log's `d3dptvid: adapter found` is checked too; Win98 stays on doc 06's cirrus machine and checks that the same component lands its three files in `WINDOWS\INF`, because on 9x there is nothing to run and nothing to bind to until the next boot. Overlay only, never the image. Local only (needs a guest image), not in `scripts/test.sh` |
| `tools/win98-driver-test.sh <image> [boot\|install]` | the Win98/Me display driver in a real guest, headless (doc 19, M10): installs the three binaries and the INF, boots on `-vga none -device d3dpt-vga` and reports the adapter's BARs, the driver's own `d3dpt9x:` / `d3dpt9dd:` / `d3dpthal:` lines through the DEBUG register, the screendump's colour count (16 or fewer = Windows fell back to VGA) and the VGA **text** page read out of VRAM, which is where a fatal exception writes itself (doc 19 §15) — believe that over the screendump. `PROG=<file.exe>` stages a program and names it in WIN.INI's `[windows] run=` so the shell starts it: this harness has no serial line and nothing to type at, and it is how the DirectDraw half is exercised at all, since nothing on a Win98 desktop calls `DirectDrawCreate`. The probe built for that is `ddprobe.exe` (creates a DirectDraw object, logs the HAL and HEL caps and calls WaitForVerticalBlank, leaves `C:\DDPROBE.LOG`). Works on a raw copy, never the user's image; the image it installs on is the driver-installed `test98` machine, not `~/vms/win98.qcow2` (a pre-BIOS-stamp install PnP does not match). Local only, not in `scripts/test.sh` |
| `tools/win98-game-test.sh <image> <name>` | a **real game** on the Win98 display driver, headless (doc 19 §26, M10 step 5) — the 9x counterpart of `xp-game-test.sh`. A raw copy of the image (never the image), the freshly built driver re-staged into it (`NO_DRIVER=1` to keep the image's own), the game started from `C:\RUN.BAT` through WIN.INI's `run=` with `GUEST_CMD=` as its body (one CRLF line each: a DOS game needs `cd` before its EXE; COMMAND.COM does not wait for a Windows program, so a second one needs `start /w` before the first), `CDS=a.cue:b.mds` on `ide.1`, a screendump every `SHOTS=` seconds, **`PLAYER=1` runs the same machine inside the player** — the only process with a 3D provider, so it is how a **Glide or OpenGL title** is run at all (Rayman 2 on Glide, 2026-09-10, doc 12 §5): the wrapper's log lands in `OUT/wrapper.log` and the player shoots the guest's own frame every `PLAYER_SHOT_EVERY` frames into `shots/2ksbox-NNNN.png`, because a QMP screendump shows the VGA surface, frozen while 3D presents, `KEYS=`/`CLICKS=` on a timeline, `JIGGLE=1` to move the mouse like a hand on it, `DUMP_EVERY=`/`TRACE=1` for the executor's frames and DP2 stream, `VGA=cirrus` for the in-box-driver control, `STAGE=` to put a probe of ours on C:\, `TEXT_AT=` to read the VGA text page out of VRAM mid-run (always read at the end too), and the ACPI power button at the end — a machine that does not answer it gets `info registers` twice and `info pic`/`info lapic` in `OUT/hang.txt` before it is killed, which is how an unrepainted desktop (EIP moving, `HLT=1`) is told from a dead guest. **It builds the machine `launcherx --print-args` gives for that bundle** — `-cpu pentium3`, `hpet=off`, an SB16 with its OPL3 and the MPU-401 (`MUSIC=gm|mt32|none`) on an audiodev — because a run with no sound card is a different test, not a quieter one: Total Annihilation prints "Sound system initialization failed" and quits before it draws a frame, which in the log reads exactly like the display driver failing (and a DOS game set up for General MIDI at 0x330, Blood, sends its music to a device that must be there). `EXTRA=` appends QEMU arguments (`-perfmap -name debug-threads=on` for a `perf` profile of the vCPU thread — and then delete `/tmp/perf-<pid>.map`: it grows by a line per translated instruction, and a retranslating game put 7.8 GB of it in the RAM-backed `/tmp` in five minutes). Local only, not in `scripts/test.sh` |
| `tools/win98-bsod-test.sh <image>` | a Windows 98 **blue screen is visible** on `d3dpt-vga` (doc 19 §29): a 9x blue screen is *message mode* — the VDD programs VGA text mode itself — and the adapter has to have left its linear mode for it, or the message sits invisible in the first 32 KB of a frame buffer nobody scans out, which is where every blue screen of this driver's first three days went. Measured: a VxD's fatal exception arrives through the ordinary `PRE_HIRES_TO_VGA` switch (the §26 hook), and the mini-VDD also answers `SAVE_MESSAGE_MODE_STATE` (the DDK's door for message screens that skip the switch; called once at boot). The test blue-screens a raw copy of the image on purpose: `RUN.BAT` runs the staged `BSOD.EXE` (`w9x/bsod.c`), which loads `BSODVXD.VXD` (`w9x/bsodvxd.c`, a dynamic VxD of ours whose init executes `ud2` in ring 0 — "exception 06 in VxD BSODVXD(01)"; the famous `con\con` is patched on the test image and from a DOS box only kills the DOS box; `TRIGGER=`/`STAGE=` for another way), and requires the VxD's `message mode` line and the device's `linear mode off`, a screendump meanwhile that is mostly the screen's blue, the VRAM text page naming a VxD or the `0028:` selector, and after a key (`press any key to attempt to continue`) `linear mode on` again, a last screendump that is not blue, and a clean power-off. A wrapper over `tools/win98-game-test.sh` (`TEXT_AT=` reads the text page mid-run). Local only (needs a guest image), not in `scripts/test.sh` |
| `tools/win98-reboot-test.sh <image> [qmp\|guest\|both]` | a Win98 guest survives a **restart** (patch 22): boots an overlay headless, resets it both ways — a QMP `system_reset` and the Start menu's Shut Down → Restart — and requires a second SeaBIOS banner on the debugcon (the machine really reset) plus a whole boot's worth of disk reads after it (the guest really ran) with `LVT0` back to ExtINT. A screendump is no evidence here: the freeze this guards leaves a splash screen with a **blinking text caret**, drawn by `vga_draw_text` on the host with no guest running at all. `QEMU=`/`BIOS=0` runs the stock-QEMU control. Overlay only, never the image. Local only (needs a guest image), not in `scripts/test.sh` |
| `launcher-capi/examples/smoke.c` | a third front end, in C, over the same models the egui and Qt builds use (`launcher-capi/include/launcher_core.h`): creates a DOS machine through the shared wizard and checks its answers (64 MB, a period processor, emulated, no network card, our own emulator fast paths all at their shipped setting and a checkbox that changes the count), **what the family picker does to the fields under it** (every untouched one moves to the new family's default, a picked one survives unless the new family has no such entry, "Default" puts a field back to following the family), then the disc shelf, the library and the profile editor. The `capi` check in `scripts/test.sh`; a scratch library, never the user's own. A changed default in a model fails here as well as in the two GUIs |
| `target/release/synthx` (`cargo build --release -p libsynth`) | the music engines (doc 20) without QEMU: `selftest <dir>` runs the AdLib detection sequence, a 440 Hz FM note measured by Goertzel against its neighbours, the same note through the **shipped bank** byte by byte down the MPU-401's own path, and a running-status note-off with a real-time byte inside the note-on (the `libsynth` check; `--roms <dir>` adds the CM-32L, which otherwise SKIPs); `wavtone <file.wav> <hz>` is what the `music` check asks of a wav QEMU recorded; `bank <file.sf2>` says whether a bank of your own plays; `opl <out.wav>` writes the FM tone to listen to. **`midilog <log>` / `opllog <log>` / `play <log> <out.wav>`** read a capture of what a guest wrote to a music device — `LIBSYNTH_MIDI_LOG=<file>` and `LIBSYNTH_OPL_LOG=<file>` in the player's environment, both at once if wanted, doc 20 §7.2: a row per channel and a column per second of note-ons (key-ons for the chip), what silenced each channel (volume/expression to zero, all-notes-off), notes left held or channels left keyed on, the most down at once against the engine's own voice count, bytes the parser could attach to nothing — and then the same writes played again with no guest at the timing they were written with. That is what separates "the guest stopped sending it" from "we stopped playing it" when music starts right and then goes wrong, and capturing both devices is how you tell a guest-side cause from ours: the two engines share no synthesis code, so a fault in both is in neither |
| the `music` check in `scripts/test.sh` | the sound-card and music pickers (doc 20 §6) from a combo box to a real QEMU — each family's default is the card it always had, the FM chip follows the card, a card a family doesn't offer is refused, an MT-32 with no ROMs is refused at the form — and then the two devices **sounding**: the human monitor writes the ports a guest would (`o /b 0x388 …`, `o /b 0x330 …`), QEMU's own `wav` audiodev records what its mixer made of it, and the note has to be in the file |
| the `sb-mixer` check in `scripts/test.sh` | the SB16's mixer volumes **applied** (patch 61, 2026-09-11): the FM note at unity and then with the card's FM volume, its master volume and the SB Pro's FM register each at −12 dB, written by the monitor the way a driver writes them, and QEMU's own wav has to come out 12 dB down all three ways. QEMU stored these registers and applied none, so Windows' sliders reached nothing and a race's effects over its CD music clipped. The CD half needs ATAPI, so it is `CDVOL=` in `tools/audio-glitch-test.py cd` |
| the `sb16-irq` check in `scripts/test.sh` | the sound card's interrupt line (patch 25, doc 20 §5.2), asked of the card and the PIC with no guest: `info irq` counts only *rising* edges of IRQ 5, so a DSP reset over a running auto-init DMA must add **none** — hardware clears the pending interrupt there, it does not make one — and a silence block (DSP 0x80) must add exactly one every time, which it can only do if the driver's read of the status port lowered the line after the one before. An interrupt the guest cannot acknowledge holds the line, and on the edge-triggered i8259 every one after it is lost: Duke Nukem 3D's SETUP played its sound test once and said "Playback failed, possibly due to an invalid or conflicting IRQ" ever after |
| `tools/hid-descriptor-check.py` | the `usb-gamepad` HID report descriptor (M13 path A, patch 26): the bytes a guest's driver parses, which nothing on the host side reads at run time — so a wrong one does not fail, it enumerates and has no axes. Parses the shipped array out of `gamepad/qemu/dev-gamepad.c` and checks the collections balance, that the input items total exactly `GAMEPAD_REPORT_LEN` (what `usb_gamepad_poll` writes), and that the hat has a null state (without it a released hat reads as north). An argument points it at another copy, which is how the `pad` check proves it can still fail: it mutates two copies and requires a complaint |
| `tools/pad-guest-test.py [dos\|xp\|win98]` | **a pad as a guest meets it** (M13), both devices, each in the family it is for — the `pad-guest` and `pad-guest-xp` checks. **`dos`** (the default) is the gameport (path B, patch 27): FreeDOS under the **player** — it has to be the player, because the pad reaches a guest through the embed library and a bare `qemu-system-i386` has a gameport nothing ever moves — with `PLAYER_PAD_SCRIPT` standing in for a controller nobody has plugged in, while `PADTEST.COM` arms 0x201 and counts the four one-shots down. Each pose moves one axis only (the stick X, the d-pad Y, the second stick Z, and the fourth axis never), so nothing has to align the guest's clock with the host's frame counter, and every axis is judged against the **undriven** axis's own spread rather than a number chosen here — a count is loop iterations, so its noise is a property of the machine. Paced (`-icount shift=7,align=on`, what a DOS machine gets) a stick's ends and centre are 12 / 265 / 571 counts against true pulse ratios of 1 : 23.8 : 46.4; `UNTHROTTLED=1` is the control and shows the risk plainly — ten times the counts and 2.25x of spread on an axis nobody is touching. **`xp`** / **`win98`** boot one of the user's own images with `snapshot=on`, a `usb-gamepad` and the guest-tools ISO, knock on the Run dialog until `PADWIN.EXE` says it started, and ask the guest's own DirectInput *and* winmm what a game would ask — every axis on the report's own 0..255 range in both columns, the POV hat's null state, the buttons, and the two columns required to agree sample by sample. The d-pad is asked **opposite** things on the two paths, which is the point of running both: it drives the axes on the gameport (a pad with only pots) and *only* the hat on HID. Every mode runs the **player** — a bare QEMU has pad devices nothing moves — so all of them skip without a display. `win98` names a launcher **machine** and reads the disk and display adapter out of its bundle (the driver is installed in a machine, and guessing the adapter is how a run ends up typing into a New Hardware wizard); it needs one whose Windows has had the pad bound once, since a fresh 98 SE asks for its own source files and an automated run has nobody to answer. **Never put a `usb-tablet` on the bus beside the pad**: on Win98 that makes DirectInput enumerate no joystick at all (measured 2026-09-10, same image both ways; XP does not care), and a pad needs a controller rather than a pointer |
| `TESTS\PADWIN.EXE` (guest-tools ISO; `guest-tools/src/padwin.c`) | the USB HID pad as a Windows **game** finds it, not as the control panel shows it, through **both** APIs a title of the era can call. DirectInput enumerates and names the joysticks, every axis goes on the report's own 0..255 range (so a reading is the byte `gamepad::hid_axis()` made), and the POV hat — where a missing null state shows up as a released hat reading north — and the buttons are read back. **winmm** (`joyGetPosEx` on top of 9x's VJOYD, what a great many mid-90s titles call) reads the same pad every sample on the same range, and that column is why M13 has no gameport driver for 9x: a Windows game on 98 gets its joystick from the USB pad through winmm too, so "Standard Game Port" through Add New Hardware buys it nothing and was dropped rather than built (2026-09-10). Opens COM1 itself, so the Run dialog can start it with no shell to redirect, and logs to `%TEMP%`. **PADWIN, not PADTEST**: the DOS probe is `PADTEST.COM` in the same folder and both DOS and cmd resolve a bare name to the `.COM` first |
| `TESTS\PADTEST.COM` (guest-tools ISO; `guest-tools/src/padtest.asm`) | the gameport from inside a DOS box: one write to 0x201 arms four one-shots, and the four counts and the button nibble go to COM1 *and* the screen, with interrupts off around the count so a timer tick cannot land inside it. An idle port reads `f0` — an absent one reads `ff` off the open bus, which is how a game decides there is no joystick. Also the A/B for Win9x, where the port needs "Standard Game Port" through Add New Hardware: a DOS box that reads the port on a machine whose Windows cannot separates a missing driver from a missing device — a real pad has been read this way in a Win98 DOS box (2026-09-10), so on 98 the device half is answered |
| `TESTS\QCLOCK.COM [s]` (guest-tools ISO; `guest-tools/src/qclock.asm`) | **DOS Quake's clock as the game reads it**, against the TSC: `Sys_FloatTime` from id's `sys_dos.c` (the BIOS tick word at 0040:006C plus PIT counter 0 in mode 2, a backward reading counted as zero *and* made the new reference, so every tick that arrives late is time counted twice) in a tight loop with interrupts on, one line per ~1 s window — true / tick / Quake ms, `speed%` (FAST at 110+), backward readings, tick jumps (reads that saw the tick count move by more than one: a burst), the longest gap between two reads (the VM not running) — and a total with `gain_ms`, what Quake counted beyond the clock. The TSC's rate is calibrated end to end, not assumed. Written for "quake.exe in a Win98 DOS box speeds up momentarily" (2026-09-10): run it in the DOS box and in pure DOS on the same machine, and the difference is Windows' queued timer ticks against QEMU's own (`i8254.c` fires late IRQ 0 edges back to back after a starved main loop). Output only after the run — a serial write in a DOS box is a trip through Windows — and the key that ends it early is read out of the BIOS buffer, never INT 16h, whose polling Windows' idle detection counts. **Measured 2026-09-10**: pure DOS (FreeDOS) on our QEMU reads `speed% 200` in every window, one full-period backward reading per tick — QEMU's IRQ 0 lands a main-loop wakeup after the counter wraps, where real hardware is within a microsecond; the Win98 DOS box adds Windows' catch-up bursts on top (86 and 153 ticks at once). In a DOS box the end-to-end TSC calibration is thrown off by Windows' tick clock losing time overall (`tsc_khz` 1218938 there against 1000015 in pure DOS on the same QEMU): read that column before trusting a window's `true_ms` |
| `tools/pit-guest-test.py` | **the PIT as a DOS game's clock meets it** (patch 34): `QCLOCK.COM` under FreeDOS on our QEMU must read no backward step and every window at 100 %, then the same with `-global isa-pit.overdue-irq=off` (upstream's late IRQ 0 edge) as a control that is reported, not required — whether a host's main loop wakes late enough to show it is the host's business. On the Air: 0 backward steps / 100 % with the patch, 540 / 200 % without. `PIT_SECS=` the run length (10). The `pit-guest` check |
| `tools/vga-dirty-guest-test.py` | **that the display sees what the guest wrote to video memory** (patch 28): a DOS program sets a mode and fills video memory a 4 KiB page at a time, each page with its own pixel value, twice with a pause between, and a screendump answers page by page which of them arrived. Its own palette is a grey ramp so a dumped pixel reads back as the value written. Three things it does deliberately: it **polls** screendumps throughout, because `-display none` with nobody asking lets the dirty bits pile up and the one dump at the end looks perfect; it fills even pages with `rep stosw` and odd ones with a byte loop, so a fast-path difference comes out striped (`FLIP=1` swaps them); and the guest **reads its own writes back** (`VERIFY_BAD=`), which is what separates "the store never landed" from "the display never redrew". `13h` and `vesa` (banked 640x480x8, window granularity read from the adapter rather than assumed -- the Cirrus says 16 KiB where the standard VGA says 64), `std` and `cirrus`, `--refill` keeps rewriting for ever so a page still wrong at the end is memory the display has stopped watching, `VBEPAL=1` sets the palette through the VBE BIOS (4F09h) instead of the DAC ports, which is how the missing 4F09h was found, and reads one three-channel entry back through the ports and a 4F09h get. **`VBEPAL=1 … vesa` is the `vbe-palette` check** in `scripts/test.sh`: SeaBIOS's VGA BIOS has no 4F09h, so ours is built from `patches/seabios/` by `scripts/build-vgabios.sh` into checked-in `firmware/vgabios-{stdvga,cirrus}.bin`, which `prepare-qemu.sh` copies over `qemu/pc-bios/` (DOS Quake at 640x480 quit on the stock ROM, Duke's VESA colours were wrong). The rest is local only (needs nasm, mtools and the FreeDOS floppy) |
| `tools/x87-fast-test.c` | patch 05's x87 fast path equals the real x87 (x86-64 host oracle) |
| the `optimizations` check in `scripts/test.sh` | the wizard's "Emulation optimizations" switches (`patches/qemu/README.md`, doc 07) from a checkbox to a real QEMU: a machine nobody has touched emits no property and writes no `[optimizations]` table, each switch lands on the option QEMU looks it up on (`-cpu` for the four CPU properties, `-accel tcg` for the seven accelerator ones), our own `qemu-system-i386` accepts the exact line the launcher writes with all **eleven** flipped, and "All defaults" empties the table again. **Since 2026-09-10 every patch of the queue has a switch** (patch 29): patches 15, 16 and 19 had none, so "turn everything off" used to clear eight of eleven and leave in the three that sit on TB invalidation, the softmmu TLB and `notdirty_write`. The switches' *effect* is the guest batteries' job; this is the wiring between them and a checkbox |
| the `pointer` check in `scripts/test.sh` | the wizard's "Seamless mouse" switch (doc 07, doc 03's grab model) from a checkbox to a real QEMU: a new Windows machine gets `-usb -device usb-tablet` (absolute — the host pointer is the guest cursor and the window never grabs), a new DOS machine gets neither (its mouse drivers read the PS/2 controller and would find nothing), turning it off removes the device *and* its controller and turning it back on restores them, and our own `qemu-system-i386` accepts both machines' lines |
| the `family-other` check in `scripts/test.sh` | the **Other** family (doc 06) — a machine for an era OS that is neither Windows nor DOS (BeOS, a period Linux, OS/2) — from the picker to a real QEMU: a new one comes out on standard hardware and none of ours (`-vga std`, never `d3dpt-vga`, whose driver is a Windows driver), an RTL8139 at `0x03` and an ES1370 at `0x04`, no USB tablet, the sound card staying in its slot when the NIC is turned off, and our own `qemu-system-i386` accepting the line |
| the `hpet` check in `scripts/test.sh` | a Win98 machine is `-machine pc,hpet=off` and our QEMU's `info qtree` has no `hpet` in it, while an XP machine's still does (so the absence means something): Windows 98 has no driver for an HPET and showed it as an Unknown Device in Device Manager on every machine (2026-09-10) |
| the `display-adapter` check in `scripts/test.sh` | the wizard's display-adapter picker (doc 06, `bundle::Video`): each family offers the adapters it has a question about and starts on the right one (Win98/XP `d3dpt-vga` or `cirrus`, Other and DOS `std` or `cirrus`), an adapter a family doesn't offer is refused rather than written (`std` on XP would leave the guest with no driver, `d3dpt` on DOS no driver at all), our adapter is *gone* rather than beside the Cirrus when it changes, the NIC stays at `0x03`, and our own `qemu-system-i386` accepts every combination |
| the `bios-date` check in `scripts/test.sh` | the firmware's legacy BIOS date as a **guest** reads it — F000:FFF5 out of a running `qemu-system-i386` over QMP, plus every `pc-bios/bios*.bin` agreeing — at or past the `ACPICheckDate` (12/01/99) Windows 98 setup compares against before it will install ACPI. A tree that lost `prepare-qemu.sh`'s stamp boots every existing guest fine and shows up weeks later as a *new* Win98 install that came out PnP-BIOS, with no USB tablet, AC'97 or NIC |
| `scripts/package-flatpak.sh` | the Flatpak (doc 07's primary Linux target; manifest in `packaging/flatpak/`): a from-source build against `org.kde.Sdk` 6.10 (KDE's, because the shipped launcher is Qt — ADR-015) — host binaries cannot be reused, the runtime's glibc is older than this host's — reusing the install layout via `package-linux.sh --prefix /app`, plus libslirp (absent from the runtime, and `-netdev user` needs it) and a build-only `distlib`. The three libraries QEMU `dlopen`s are built in the sandbox too, against the runtime's own GL and Vulkan: the Glide wrapper, DXVK's `d3d9` and the Direct3D executor (`third_party/dxvk` therefore comes along in the source copy — the executor needs its `include/native` headers). Then asks the *installed* app, in its own sandbox, whether every companion resolves under `/app` (the launcher's `--paths` and the **player's `--companions`**, which is the only thing that can see the three dlopened ones) and the library under `~/.var/app`. The build is **offline** (Flathub's rule): `packaging/flatpak/cargo-sources.json` declares all 513 crates with checksums — regenerate with `scripts/gen-flatpak-cargo-sources.sh` after any dependency change. `FLATPAK_BUILD_DIR` moves the build tree off a full root filesystem |
| `scripts/package-macos.sh` | the macOS app (doc 07's "signed .app, JIT entitlement, notarized"; recipe and reasoning in `docs/build-macos.md` → "The app"): stages the same install layout into `2ksbox.app/Contents` — where `MacOS/` does `bin/`'s job, `paths::bin_dir()` — **plus the whole non-system dylib closure**, because the Mac that will run it has no Homebrew, no XQuartz and no Vulkan: every install name rewritten to `@rpath` and every `LC_RPATH` pointing out of the app deleted (meson gives `libqemu-embed` one per Homebrew prefix, and they are searched first). First package to carry the Glide wrapper and the Direct3D executor, with the LunarG loader + KosmicKrisp ICD beside them; the packaged player names all four to QEMU through `player/src/companions.rs`. Then it asks the staged app the questions `package-linux.sh` asks, and one more: run under `DYLD_PRINT_LIBRARIES=1`, **every image the loader touches** must be inside the app. `LSMinimumSystemVersion` is measured from the bundle's own Mach-O files, not chosen. Signs inside-out with `--options runtime` + `packaging/macos/2ksbox.entitlements` (`com.apple.security.cs.allow-jit`, without which TCG dies on its first block), notarizes with `--keychain-profile`, staples, rolls a `.dmg`. The `package` check in `scripts/test.sh` on a Mac (`--no-sign --no-dmg`) |
| the `no-optionals` check in `scripts/test.sh` | that the artefacts link only what we chose (2026-09-07). QEMU auto-detects a large optional surface, so what a build links is otherwise decided by which libraries the machine had; `configure-qemu.sh` disables four dead families — display, host audio, network backends, network-storage block drivers — plus brlapi, and this asks the **artefacts**, not the configure summary, because a dropped flag re-links silently and every packager starts carrying the library again. `libqemu-embed`, `qemu-system-i386`, `libdxvk_d3d9` and the Windows pair in `build/win/` if they exist: a link (`ldd`/`otool -L`), a bare library name in the binary — the way SDL last bit was a *runtime* `LoadLibrary` that no import-table walk could see, on a user's PC — and QAPI's `AUDIODEV_DRIVER_<X>` enumerators, which exist only behind their own `CONFIG_AUDIO_<X>` and so catch the three backends that link nothing (OSS, CoreAudio, DirectSound). That last probe is what found patch 23 |
| `scripts/gen-icons.sh` | the application icon: one master (`packaging/icon/2ksbox.png`), every size derived from it (16–512 PNGs + a four-size `.ico`) and checked in, because nothing that needs an icon can draw one — the launchers `include_bytes!` a PNG at compile time, the Flatpak build is offline, the Windows package is cross-built without ImageMagick. `--check` is the `icons` check in `scripts/test.sh`; the Windows .exes carry the .ico as a resource through `packaging/windows/win-icon.rs`, `include!`d by three build scripts |
| `scripts/package-linux.sh` | the Linux package (doc 07's install layout, ADR-011's names — product `2ksbox`, application ID `com._2ksbox.Launcher`): stages launcher + player + embed library + `qemu-img` + firmware + guest-tools ISO + the three libraries QEMU `dlopen`s by name — the Glide wrapper (`lib/2ksbox/libglide2x.so`), the Direct3D executor and its DXVK (`libd3dpt_exec.so` + `libdxvk_d3d9.so.0`, both or neither) — into one relocatable prefix (`--with-shaders` adds the presets), then asks the **staged** launcher with `env -i` from `/` whether every companion resolves inside the package (`launcher --paths`), that the staged player `ldd`s to the package's own `libqemu-embed`, that the staged **player** finds the packaged wrapper by its own rule rather than this script's (`player --companions`, which prints what `player/src/companions.rs` resolved for Glide, the D3D executor and DXVK — each answer must be inside the package), that the packaged `qemu-img` creates a disk and `--print-args` points `-L` at the packaged firmware, and that the desktop entry and the AppStream metainfo validate (`appstreamcli --no-net`, errors only); rolls a `.tar.zst` unless `--no-tar` (the `package` check in `scripts/test.sh`). `packaging/linux/install.sh` inside it copies a tree into a prefix |
| `target/release/discx` (`cargo build --release -p libdisc`) | the CD-ROM model (doc 17): `selftest <dir>` writes synthetic cue/bin, CCD and ISO images and checks reads, EDC/ECC, Q synthesis and the MMC responders through them (the `libdisc` check in `scripts/test.sh`); `info` / `dump` print what a guest will see (cue, CCD, MDS, ISO); `scan` classifies and L-EC-verifies every sector of a real dump (the bad-sector map: SafeDisc's weak sectors show up here) and splits the failures the way a guest meets them — read anyway (the EDC is a CRC-32 over exactly the delivered bytes, so an intact one means the damage is in parity nobody sees), read after the P/Q decoder repairs them, or unreadable; on a protection band every failure must land in **unreadable**; `repair <image> <outdir>` writes the negative-control copy of a protected dump (every L-EC-failing sector's EDC/ECC regenerated over the dumped user data, nothing else touched, run-out sectors left alone) so a protection check can be watched to *fail*; `subscan` does the same for the stored subchannel (Q CRC failures and whether they cluster, and how often `subq::synthesize` reproduces the disc's own frames); `convert` makes a MODE1/2352 cue/bin (+ WAVE audio tracks) from an ISO; `export` writes the cooked view as an `.iso`, which is how a **folder disc** is checked — `isodir:<dir>` serves a host directory as a generated ISO 9660 + Joliet volume (M5g, `docs/tracks/m5-dirdisc.md`), `mktree` writes the fixture tree for it and the `dirdisc` check in `scripts/test.sh` has xorriso read the folder back out |
| `tools/atapi-guest-test.py` | a DOS program drives the ATAPI drive on a cdimage disc by PIO (patch 51): every reply at two byte-count limits identical to `discx dump`, the sense of a bad / audio sector, audio positions and both stops (STOP PLAY/SCAN and the START STOP UNIT a Windows guest sends instead); then the disc shelf (patch 52): LIST/LOAD/EJECT with the sectors read before and after to prove the tray changed, and a second boot running the real `CDSHELF.COM` on the same shelf; the `atapi-guest` check |
| `tools/cd-rate-guest-test.py` | that what the guest reads off the CD does **not** depend on how fast it asks for it (the question behind a corrupt texture and a video that stops in the middle): a DOS program reads the same range of sectors by PIO *and* by bus-master DMA -- the path Windows takes, and on a cdimage disc our own code -- at 1, 8 and ~31 sectors a request, at byte-count limits 2048/8192/65534, in 2048 and 2352-byte sectors, unpaced and paced to 1x / 4x / 16x by a delay loop calibrated against the BIOS tick, and folds every byte into a rolling checksum. Every pass over one range must produce the same checksum, and the 2048-byte ones must equal the host's checksum of the image. A refused request is retried one sector at a time, so a bad sector is named with its sense rather than losing the pass. It builds its own disc twice -- an `.iso` (QEMU's raw driver, stock code) and the same data as a MODE1/2352 `.cue` (libdisc + patch 51) -- so the A/B between the two drivers is part of every run; `<image>` runs it on a real disc and `SCAN=1` reads every sector of one once. The guest has to set PCI bus-master enable itself: without it the DMA engine runs, reports success and writes nothing. Local only, not in `scripts/test.sh` |
| `tools/xp-cdimage-test.sh <image> <disc> <ref dir>` | XP boots read-only with a `.cue`/`.ccd`/`.mds`/`.iso` — or `isodir:<dir>`, a host folder served as a generated disc (M5g), where passing that same directory as the reference makes the run a round trip — as its CD-ROM (the `cdimage` block driver, doc 17), copies the whole disc through cdrom.sys to the scratch FAT and every file is compared with the reference directory (the ISO extracted with `bsdtar` or xorriso); `CDTEST=<CDTEST.EXE>` also plays track 2 through MCI into a wav on the drive's `audiodev` and checks for the 1 kHz tone and that the mode right after `stop cd` is not still `playing`; the `guest-cdimage` check (and `guest-dirdisc`, the same tree served as a folder). Runs on macOS too: mtools builds the scratch disk where `sfdisk`/`mkfs.fat` are missing, and the wait watches COM1 because XP's lazy writer can hold a small FAT write for minutes |
| `tools/dirdisc-guest-test.sh <image> [win98\|xp]` | a host folder in a guest's own CD-ROM drive (M5g) on the families `xp-cdimage-test.sh` does not cover: boots `-cdrom isodir:<dir>` with a floppy of `RUN.BAT`, and the guest's own `dir` / `type` over COM1 are the proof. **`BIG=1` asks where the guest stops instead**: sparse filler with a marker file after 703 MiB (an 80-minute CD, where the drive starts reporting a DVD-ROM profile, patch 53), 878 MiB (past every MSF address), 2 GiB, 4 GiB and 7.8 GiB, each one `type`d back — **Win98 and XP both read all five on 2026-09-07**, so a DVD-sized folder is not a Windows problem; `MARKS=` picks the offsets. Overlay only, never the image. Local only (needs a guest image), not in `scripts/test.sh` |
| `TESTS\CDTEST.EXE` (guest-tools ISO; `guest-tools/src/cdtest.c`) | CD audio through MCI in XP / Win98: tracks, play track 2, positions while playing / paused / resumed, `cdtest.log` |
| `tools/cdaudio-guest-test.sh <image> [win98\|xp]` | CD-DA through a guest's own MCI, and **how that family stops a drive** (doc 17 §5.4): `CDTEST.EXE` plays / pauses / stops while `CDIMAGE_TRACE=1` records every packet, and the run prints the audio commands the guest actually sent — they are not the same on the two families. XP pauses, seeks, pauses, plays and stops with START STOP UNIT; **Win9x's `mcicda` sends one PLAY AUDIO MSF and two SEEKs and nothing else**, so a seek is its stop (patch 54). The checks that matter ask the drive and the speaker, not MCI: MCI answers `status mode` from the state it *commanded* and said "stopped" for a year while the drive played on, so the run also requires the last audio status in the trace not to be 0x11 and the audiodev's wav to hold about as much audio as was asked for (a drive ignoring the stop puts 61 s in it for a 4 s play). `PLAY=` seconds, `DISC=` another disc, `WAIT_SECS=` the cap (Win98 under TCG with the trace on wants ~400). Overlay only, never the image; local only, not in `scripts/test.sh` |
| `CDSHELF\CDSHELF.EXE` / `.COM` (guest-tools ISO; `guest-tools/src/cdshelf.c`, `cdshelf.asm`) | the host's disc shelf from inside the machine (doc 07, patch 52). No arguments: a window on Windows, a key-per-disc menu in DOS. Verbs for scripts: `CDSHELF LIST`, `CDSHELF <n>`, `CDSHELF E`. One EXE for Win98 (ASPI) and XP (SPTI), a NASM `.COM` for DOS; nothing to install — it is a vendor command on the machine's own CD-ROM drive, and an insert always ejects first, waits for the drive to confirm the empty tray, and then **dismounts the volume** (`FSCTL_DISMOUNT_VOLUME`): the program's own TEST UNIT READY polling consumes the media-change sense a drive raises once, so Windows must be told outright or it keeps serving the disc that came out |
| `tools/cdshelf-guest-test.sh <image> [xp\|win98]` | `CDSHELF.EXE` in a real Windows guest, headless: boots with an empty tray and a two-disc shelf (a generated ISO and a path that doesn't exist), lists it, loads the ISO and reads its files back with `dir`/`type` — Windows' own driver is the proof the tray changed — refuses the missing one, ejects. Local only (needs a guest image), never wired into `scripts/test.sh`; writes to a qcow2 overlay, never the image |
| `tools/dos-guest-test.py` | the DOS machine family (doc 06) end to end: the launcher's own bundle → `--print-args` → our QEMU → a real FreeDOS floppy. Checks `Machine::reference(Dos)`'s defaults, that the machine boots from its **floppy** (the blank disk can print nothing), that a throttled machine is emulated even when the bundle says KVM, and that the processor combo is real — a 200 M-instruction loop timed inside the guest against the rate the chosen CPU promises (31.3 M/s asked 31.25, 7.8 asked 7.8). That last check is the point: `-icount` without `align=on` only makes the guest *believe* it is slow. Local only (fetches the FreeDOS floppy), not in `scripts/test.sh` |
| `tools/duke-guest-test.py` | **a real game on the music devices** (doc 20 §7): Duke Nukem 3D's DOS build, copied off the user's own Atomic Edition disc (read-only — `ATOMINST\` on it is the whole game, uncompressed), onto a FAT disk this tool makes; the game's own SETUP.EXE is driven through its menus once to write a config, the disc goes back in the drive (the game checks for `DUKE3D.IDF` through the path in `CDROM.INI`, and never touches the drive without it), and then Duke's score arrives at our MPU-401 — `mpu401: 1245 bytes, 415 note-ons on 6 channels in 5.0 s` — and in the wav. `--run "DIR D:\"` runs a DOS command instead, `--keys` drives any menu by QMP with a screendump after each keystroke. The FM half plays too but only when SETUP launches the game (the docstring has the measurement and the open question). Local only, never in `scripts/test.sh` |
| `tools/midi-guest-test.py` | the music devices as a **guest** meets them (doc 20): a DOS program runs the AdLib detection sequence (status 0x00 → 0xC0 → 0x00), plays 440 Hz on the OPL3, then resets an MPU-401, puts it in UART mode and plays A4 through it — and the **wav QEMU's own `wav` audiodev recorded** is the evidence, not the program's own opinion. Two boots (~11 s), one per device, because both are asked the same question and one file with two notes in it cannot answer it twice; the `midi-guest` check in the guest stage |
| `tools/audio-glitch-test.py [sb16\|opl]` | **clicks in what the host hears**, counted (2026-09-10, the crackle fix): a DOS guest plays a pure 441 Hz tone **in the player**, whose consumer is a simulated DAC (`PLAYER_AUDIO_NULL=<frames>`, a real device's burstiness included) and records exactly what it would have handed the device (`PLAYER_AUDIO_TAP`); a least-squares sine recurrence is exact for a pure tone, so every residual spike is a break — a dropped tick, a padded silence, a stale buffer. `sb16` is a Sound Blaster driven the way DirectSound drives one — 8-bit auto-init DMA, the guest writing `MARGIN=` samples ahead of the cursor it reads off the DMA controller — and the guest itself counts the times the card read past what it had written (`late`), which is how a pull ahead of wall time shows; `opl` is a synth, where nothing can be read ahead, so a click there is the ring's own. `PERIOD=` the DAC's frames (1024; 2048 and 4096 are where the old pacing fell apart: 12 and 922 clicks in 20 s), `ICOUNT=1` a DOS machine's pacing. **`STALL=<MB>` is a 3D race** (2026-09-11, Carmageddon crackled only in races): a QMP `pmemsave` of that many MB every 33 ms holds QEMU's main loop the way a Glide swap's `glFinish` under the big lock does, and the tool prints how long each one held it — 64 is ~10-14 ms, 128 ~18-22 ms; the first fix left 47 and 223 clicks there, now 0 and 1. `LOAD=<bytes>` writes that much unchained Mode X VGA memory per poll instead (every store a device access under the lock: harmless even at 26 MB/s), `VGA=d3dpt` the adapter. **`cd` and `cd+sb16`** add CD-DA off the drive (a generated audio-only cue of a 441 Hz tone at 44100, which sums with the SB16's 441 Hz into one pure sine; PLAY AUDIO MSF by PIO): a race with CD music. `CDVOL=<reg>` writes the SB16's CD volume (0x36/0x37) before PLAY and checks the tone comes out that many dB down (patch 61's CD half), `CDAMP=` the CD tone's level — `CDAMP=16000` puts `cd+sb16` past full scale, which is **the Carmageddon crackle that survived both pacing fixes**: QEMU applies no mixer volumes and saturated the sum in s16. A clipped sine is smooth, so the click detector cannot see it; samples at full scale in the tap are their own FAIL, and the player's limiter (f32 ring, player/src/audio.rs) is what keeps them out. Local only (a display: it is the player), not in `scripts/test.sh` |
| `tools/x87-guest-test.py` | DOS program under TCG: results identical with the fast path on/off (needs nasm, mtools, FreeDOS floppy); `QEMU_TCG_OPTS=pinned-regs=on` runs it and the other DOS batteries under patch 21's pinned registers (doc 18) |
| `tools/smc-guest-test.py` | self-modifying code under TCG (patches 18 and 24): 18 DOS cases (immediates patched from another block and inside the executing one, same-value rewrites, an opcode flip, partial patches, `rep movsd` over a routine, an imm32 straddling a page, a disp32 / imm8 / modrm+imm patched per call, `shr` and `rol` counts over every byte value, IMUL's imm32, a 16-bit `rcr`, one imm32 inside two blocks two bytes apart), architecturally right under all four `smc-same-value` × `soft-imm` combinations — and, since a right answer does not prove a block survived its patches, the soft-imm runs must reach the path and absorb the writes *at the fields* of cases N, O, P and R (their addresses printed by the program); the `smc-guest` check |
| `tools/smc-diff.py` (`build/venv-capstone/bin/python`) | two captures of a guest code page (`RACE_MEMSAVE=` in `tools/xp-moto-race.sh`, or QMP `memsave`) diffed and disassembled: which instructions and which bytes (imm/disp/opcode) a game patches |
| `tools/rep-guest-test.py` | `rep movs`/`stos` under TCG (patch 17): 536 DOS cases (widths, a16/a32, DF, page crossings, straddling elements, overlaps, fill values), `rep-fast` on/off identical and equal to a Python model of the instruction; the `rep-guest` check |
| `tools/string-bench.py` | rep movs/stos/scas throughput under TCG, side-by-side for two QEMU binaries (the number behind patch 09) |
| `guest-tools/src/d3dfeat9.c` (+ `tools/d3dfeat9-native.cpp`) | the D3D9 feature test (shaders without D3DX, declarations, state blocks, queries, cube maps, surfaces): the XP guest's frame must be byte-identical to the native DXVK build's |
| `tools/d3dpt-exec-test.cpp` | the paravirtual D3D decoder + DXVK executor without a guest: D3D9TEST's batches through the guest encoder → BMP; hostile batch refused |
| `tools/sse-guest-test.py` | same for the SSE inline path (patch 11, doc 16): every SSE/SSE2 float op over edge-value pairs, `sse-fast=on/off` identical; also runs the SSEBENCH.COM ratio |
| `tools/hvf-el1/` (`build.sh`, then `build/hvf-el1/hvf-el1 build/hvf-el1/payload.bin`) | the Hypervisor.framework EL1 probe (M9): a bare-metal Rust guest with the x86 page tables mirrored in stage 1 measures exits vs in-VM traps/calls, page-fault fill, #PF, dirty upgrade, CR3/ASID switch, JIT, kick latency, and the mirrored load vs the exact softmmu sequence, with the native baseline; macOS only, not in `test.sh` |
| `guest-tools/src/ssebench.c` | `SSEBENCH.EXE`: SSE and x87 math throughput in ns/op, for the rig and the guests (with and without `sse-fast=off` / `x87-fast=off`) |
| `tools/xp-ssebench.sh` | runs `SSEBENCH.EXE` in an XP image headlessly (QMP typing, output via a floppy image), once per `-cpu` config |
| `tools/d3dpt-dp2-test.cpp` | the display driver's records (doc 15 M7c) without a guest: VRAM surfaces, a context, the D3D7TEST scene as DX7 DP2 tokens, readback pixels checked, hostile records refused; its BMP is the oracle for the guest's `D3D7TEST` |
| `tools/qtmin/` (`scripts/win-cross.sh sh -c 'cd tools/qtmin && cargo build --release --target x86_64-pc-windows-gnu'`) | the smallest cxx-qt binary that cross-builds for Windows, in three rungs (no bridge / one bridge / a QML module), for the M11 question "which layer faults before `main`" — rung 1 already did, and its README has the answer that came out of it and the eleven-line fix (`std::call_once` across a libstdc++ DLL boundary, with two emutls registries; `launcher-qt/src/once_proxy.cpp`). Local only, not in `scripts/test.sh` |
| `tools/embed-3d-test.c` | drives the window-less Mesa backend without a guest: context, frame, orientation, dma-buf ring (Linux) |
| `TESTS\GLIDETEST.EXE` (guest-tools ISO; `guest-tools/src/glidetest.c`) | Glide 2.x through the pass-through device from inside the guest (doc 12 §5): the same scene `glide-host-test` draws, but the whole chain — the guest's `GLIDE2X.DLL`, the MMIO FIFO, `hw/3dfx`, our `libglide2x`, the frontend's context. It checks its **own** pixels through `grLfbLock` rather than trusting the host to look at them, and ends with `glidetest: N cases, M failed`. Four cases: a clear, the triangle (whose corners are the upper-left-origin check), a re-clear, and a close/reopen — a game's mode switch, which is where a host that leaked its context fails. `-res N` picks another resolution, `-hold N` keeps the frame up |
| `tools/glide-guest-test.sh <image>` | `GLIDETEST.EXE` in a real Win98 guest **in the player**, headless: overlay boot with the guest-tools ISO (`SETUP /ALL` installs the Glide wrapper) and the program on a floppy, driven through the Run dialog over QMP, verdict read off COM1. It must be the player and not `qemu-system-i386`: a bare QEMU registers no 3D provider, so `glide_host_ops` returns NULL and `grSstWinOpen` fails by design. `PACKAGE=<staged tree|prefix>` runs it out of a package instead — its player, its firmware, its ISO, and **no `QEMU_GLIDE_LIB`**, so the packaged player's own rule has to find the wrapper. Local only (needs a guest image), never in `scripts/test.sh` |
| `tools/glide-host-test.cpp` | Glide pass-through without a guest (doc 12 §5): the real host wrapper (`build/glide/libglide2x.so`) loaded by `hw/3dfx`'s own dispatcher, opened through `glidewnd.c`'s handshake on a context nobody has a window for, then a clear and a triangle through the wrapper and `grBufferSwap` -- the frame is checked at the frontend callback, corners included so Glide's upper-left origin is proved too. Then an LFB **write lock held across a swap** (the buffer filled with 565 blue, `grBufferSwap` with no unlock, the frame must be blue): Carmageddon's front end lives in a locked LFB and upstream OpenGLide drew it only on the unlock that never comes — black frames until patch `05-lfb-locked-swap`. `GLIDE_TEST_BMP=<path>` writes the frame out; `GLIDE_HOST_LOG=<path\|->` turns on the wrapper's own log. The `glide-host` check in `scripts/test.sh` |
| `tools/qmpc.py` | drives a guest over an extra `-qmp unix:…,server,nowait` socket: keys, typing, screendumps |
| `guest-tools/src/d3dgame9.c`, `d3dgame8.c` | the Direct3D reference scene (doc 14): golden BMPs from the rig, diffed against every emulated path |
| `build/crtcal-render <dir>` (`tools/crtcal-render.c`) | writes doc 09's eight CRT calibration patterns as BMPs at every era mode and checks each one's circle comes out round on the tube it is drawn for (the `crtcal` check in `scripts/test.sh`); the patterns themselves are `guest-tools/src/crtcal.h`, shared with the guest program |
| `TESTS\CRTCAL.EXE` (guest-tools ISO; `guest-tools/src/crtcal.c`) | the same patterns on a real tube: exclusive full-screen DirectDraw at the exact mode, no blit or stretch anywhere. SPACE / 1–8 step patterns, `M` next mode, `L` legend, ESC quits. Doc 09 has the capture protocol (**shutter ≥ 2 frame periods**, manual exposure, ruler in the macro frames) |
| `TESTS\TEXTCAL.COM` (guest-tools ISO; `guest-tools/src/textcal.asm`) | the 720×400 patterns, DOS only — Win98 has no 720×400 desktop mode, it is the VGA *text* mode (80×25 of 9×16 cells). Custom character generator; pattern 2 shows the 9th-column rule (a solid glyph below 0xC0 stripes, 0xDB does not), pattern 6 is mode 13h for the double-scan A/B against pattern 1. SPACE / 1–6, ESC |
| `player --shader <preset> --calib <bmp\|dir>` | the other half: the same patterns through a shader preset, one `.shaded.png` per BMP at 3200×2400, for holding against the photographs |
| `target/release/player --mode-sweep <dir>` | the display path without a guest (doc 03, M2): every mode in the table through mode analysis, the geometry stage and a real CRT preset — on-screen aspect, integer vertical scale, the parameters reaching the preset, and the scanline count counted in the frame it drew; a PNG per mode in `<dir>` (the `mode-sweep` check in `scripts/test.sh`, ~2 s). `PLAYER_MODE_PARAMS=0` is the control: the preset left to guess from the framebuffer height |
| `PLAYER_DUMP_OUT=x.png` | dumps the shaded frame headlessly, works while the window is occluded |
| `DRIVER\SETMODE.EXE` (guest-tools ISO) | lists / switches XP display modes from a script; the QEMU log shows the device side (`d3dpt-vga: linear mode on …`, `guest: …` = the driver's debug register) |
| `DRIVER\DDTEST.EXE` (guest-tools ISO) | DirectDraw 7 through our driver: HAL caps, VRAM flip chain, windowed blit, fps, `ddtest.log`/`.bmp`; at 8 bpp a palette on the primary rotated every frame (the 2D titles' palette animation); `scanout offset` lines in the QEMU log are the page flips |
| `DRIVER\D3D7TEST.EXE` (guest-tools ISO) | Direct3D 7 through our driver's HAL (M7c): device enumeration, Z buffer, texture, the reference scene, fps, `d3d7test.log`/`.bmp` (the BMP must match `d3dpt-dp2-test`'s) |
| `tools/xp-driver-test.sh <image> install\|ddtest\|modes\|d3d7\|d3dgame8\|shtest\|cktest\|cubetest\|probe\|probes\|cmd\|bat` | the whole M7 guest loop headless (`d3dgame8`: the M4 reference scene through XP's own d3d8.dll on the DX8 DDI, no wrapper DLL, frame diffed against the native oracle): boot with the driver ISO + FAT scratch disk, type the guest commands over QMP, pull the logs out, print the device log; `d3d7` also diffs the guest frame against the host test's; `ddtest` runs 8 (palette) / 16 / 32 bpp + windowed from a staged batch file, `bat` stages a batch file as `E:\RUN.BAT` (the Run dialog truncates long lines), `CPU=pentium3` picks the KVM CPU model, `GAME_ISO=` attaches a game disc as D:, `SHOTS=n` screendumps every 5 s, `SHOT_KEYS="12:esc"` presses keys before given screendumps, `QEMU_EXTRA='-audiodev none,id=snd0 -device AC97,audiodev=snd0'` adds QEMU arguments (a sound card), `VGA=cirrus` runs the control on XP's inbox driver (GTA 2's intro-skip crash reproduced there: the game's, not ours) |
| `tools/xp-game-test.sh <image> "<game dir>" <exe> [name]` | a game on the M4 Direct3D device, headless: snapshot boot with the discs in `CDS=a.iso:b.iso` on the same IDE slots as under the player, a USB stick carrying `RUN.BAT` and receiving the logs, `FRESH_DLLS=1` (D3DPT DLLs from the ISO next to the EXE), `TRACE=1` (the DLL's call trace), `KEYS=8:ret,25:esc`, `SHOTS=n` (VGA screendumps: launchers, error boxes), `DUMP_EVERY=n` (the executor's frames), `DRW_AFTER=s` (Dr. Watson attached to the game: every thread's stack), `PAGEHEAP=1` (heap overruns fault where they happen), `CPU=pentium3`; `stacks <drwtsn32.log>` prints a report's stacks |
| `tools/xp-maxpayne.bat` (+ `GAME_ISO=DINO-MAP.iso CPU=pentium3 SHOT_KEYS="2:ret,6:ret"`) | Max Payne on the M7c HAL with **no wrapper DLL**: XP's own d3d8.dll driving our DX7-level DDI (renames the M4 `D3D8.DLL` away, points `cd.ini` at D:); launcher, menu, tutorial level in the screendumps |
| `D3DPT_DP2_TRACE=<flag file>` / `D3DPT_DDI_REREAD=1` / `D3DPT_DDI_NOFOG=1` (QEMU env) | the display driver's DP2 stream, one whole frame per `touch` of the flag file: a snapshot of every state, every token with its arguments (dropped states marked), each draw's first vertices, bound textures with the mean of their VRAM texels (QEMU log), plus every texture level and the render target after every draw as `.ppm` next to the flag file — count pixels per `draw-<n>.ppm` to name the draw that paints an artefact; the re-read switch tells a stale host texture from VRAM the guest never wrote, the fog switch rules fog out |
| `tools/xp-vicecity.sh play\|vm\|attach\|stop <image>` (+ `tools/xp-vicecity.bat`; `DDFLAGS=32768` vertical blank off, `DDFLAGS=1081344` the control, `NO_KVM=1` TCG) | GTA Vice City (the DirectX 8 title, RenderWare) on the M7c DX8 DDI headless: XP's own d3d8.dll, no wrapper, the play disc as D:; from the desktop through the menus (clicks: the pointer selects) into a new game, the cutscenes skipped with Space, 60 s in the city, `rates.txt` = the QEMU log's `ddi: N frames/s (… draws …)` lines there; the workload behind protocol v9's video-memory vertex buffers — the A/B is the same run with the ddflags bit (buffers back in system memory, vertices copied per draw); the game's own frame limiter must be off in the image (Options / Display Setup) |
| `tools/xp-fifa2000.bat` (+ `GAME_ISO=FIFA2000.ISO`) | FIFA 2000 on the M7c HAL: renames the WineD3D DLLs out of the game folder, installs `E:\DINPUT.DLL` if staged, dumps its registry, starts the game; the screendumps show the intro, title and attract-mode match |
| `tools/xp-fifa-match.sh kvm\|tcg <image>` | FIFA 2000 into a real match headless (menus and side over QMP, the kickoff is automatic) and a keyboard test in it: F2 / F1 / Esc / F12 taps with a screendump after each, `dinput_log.txt` pulled from the image; the pause menu on Esc is the pass |
| `tools/xp-diablo.sh install\|play <image>` | Diablo on the driver's 8 bpp palettized modes, headless: the installer's three clicks, then the game from the intro to Tristram with screendumps (`title.png`, `town.png`, `walk.png`, `char.png`); the pass is Tristram in the right colours and `linear mode on (640x480x8` in the QEMU log |
| `DRIVER\DXTTEST.EXE` (guest-tools ISO; `guest-tools/src/d3dptvid/dxttest.c`) | Direct3D 8 texture formats through our driver: every format (RGB and DXT1/3/5) × pool (DEFAULT, MANAGED, SYSTEMMEM): CheckDeviceFormat, CreateTexture, Lock, a textured quad read back (pure red / blue block texels = pass), CreateImageSurface; every HRESULT in `dxttest.log`, the driver's `surface … pf …` lines in the QEMU log; run it with `tools/xp-driver-test.sh <image> cmd 'cd /d %TEMP% & D:\DRIVER\DXTTEST.EXE & copy dxttest.log E:\'` |
| `DRIVER\SHTEST.EXE` (guest-tools ISO; `guest-tools/src/d3dptvid/shtest.c`) | vertex / pixel shaders 1.x through XP's own d3d8.dll on our DX8 DDI: vs 1.1 through a declaration and its constants (user memory and a vertex + index buffer), a declaration-only shader, `D3DVSD_CONST`, ps 1.1 with a constant and with a texture, the FVF path again; every draw read back in the guest and compared, `shtest.log` ends with `shtest: N cases, M failed`; `OUT=build/xp-driver-test/sh tools/xp-driver-test.sh <image> shtest` runs it (PASS = 0 failed) |
| the DX8 feature probes, `DRIVER\CUBETEST STRMTEST VOLTEST FMTTEST BUMPTEST SPRTEST ANISTEST PATCHTST.EXE` (guest-tools ISO; `guest-tools/src/d3dptvid/*.c` over `d3d8probe.h`) | one program per Direct3D 8 feature of our DX8 DDI, each through XP's own d3d8.dll with every draw read back in the guest: cube textures (v11), more than one vertex stream (v10; SHTEST has the shader half), volume textures, the formats the driver does not list (L8, A8L8, A4L4, A8, X4R4G4B4, R3G3B2, A8R3G3B2, DXT2/4, colour and alpha each), bump mapping (EMBM on V8U8, and DOT3), point sprites, anisotropic filtering (stripe contrast at a grazing angle, anisotropic against trilinear), RT- and N-patches. **A probe of a feature the driver does not have says so and stops**: its log ends `<name>: N cases, M failed (not offered: <why>)` when the caps lack the feature, which is what the probes of features not built yet print, and the same program is the feature's check the day the caps claim it. `xp-driver-test.sh <image> probe <NAME>` runs one, `probes` all eight in one boot, `cubetest` is `probe CUBETEST`; the verdict is PASS, NOT OFFERED (with the reason) or FAIL, and a probe that left no log is a FAIL |
| `DRIVER\CKTEST.EXE` (guest-tools ISO; `guest-tools/src/d3dptvid/cktest.c`) | palettized textures and colour keying through the DX7 HAL (doc 15, protocol v8): a `DDPF_PALETTEINDEXED8` texture with its own `IDirectDrawPalette`, `SetEntries` changing an entry, a R5G6B5 texture with `SetColorKey(DDCKEY_SRCBLT)` drawn with `COLORKEYENABLE` on and off; every draw read back from the back buffer, `cktest.log` ends with `cktest: N cases, M failed`; `OUT=build/xp-driver-test/ck tools/xp-driver-test.sh <image> cktest` runs it |
| `DRIVER\EBTEST.EXE` (guest-tools ISO; `guest-tools/src/d3dptvid/ebtest.c`) | the DirectX 3 path a 1997 title takes, through XP's own `d3dim.dll` on the HAL (doc 15 "Execute buffers"): `IDirect3D` v1 on the back buffer, the viewport's Clear through a background material, execute buffers (`PROCESSVERTICES` COPY / TRANSFORM, `D3DOP_TRIANGLE`, UNCLIPPED and CLIPPED), textures loaded by `IDirect3DTexture::Load` and bound by `TEXTUREHANDLE`, a colour-keyed one; every HRESULT and every case's readback in `ebtest.log`, which ends with `ebtest: N cases, M failed`; `OUT=build/xp-driver-test/eb tools/xp-driver-test.sh <image> ebtest` runs it, `… ebtest -rgb` the same on the runtime's RGB software device (the control) |
| `tools/xp-motoracer.sh install\|play\|vm\|stop <image>` | Moto Racer 1997 (the DirectX 3 title) on the driver, headless: the disc's Alcohol MDS/MDF as D: through the cdimage driver, the InstallShield installer clicked through, then the game on a 16 bpp desktop (it insists) into a practice race with screendumps (`title.png`, `menu.png`, `bike.png`, `race*.png`); the menus are driven by `tools/motoracer-state.py` (a screendump classifier: title / name / menu / mode / race-select / showroom, each step checked and retried; the title takes only Enter and idles into an attract demo, which the script quits through its Esc menu); the pass is the bikes and the track drawn by the HAL (`ddi: … draws` in the QEMU log) and the showroom's 2D panels in `bike2.png` (GDI writes the driver never sees: doc 15 "Untracked writes"; `N untracked guest pixels` in the log) |
| `DRIVER\DITEST.EXE` (guest-tools ISO) | a game-style DirectInput keyboard (exclusive + foreground, busy loop between polls): what DirectInput buffered data, DirectInput state, GetAsyncKeyState and WM_KEYDOWN each see of the keys; `ditest.log` |
| `D3DPT\DINPUT.DLL` (guest-tools ISO) | next to a game's EXE: merges `GetAsyncKeyState` into the keyboard state — the FIFA 2000 match keyboard fix (doc 15), user-confirmed 2026-09-05 by A/B on a Linux TCG run. TCG-only medicine: on a KVM host the games need no shim at all. Per-game by decision, never `system32` / `AppInit_DLLs`. Silent by default; `D3DPT_DINPUT_LOG=1` adds `dinput_log.txt` (devices, cooperative level, poll rate, every key/button, what Windows sees) at a cost that matters under TCG |
| `tools/tcg-profile.sh <image> <name> ['guest cmd']` (M9) | where the vCPU's time goes under TCG: macOS `sample` + `-perfmap`, the report split into generated code / helpers / softmmu / translation and mapped to guest pages (`tools/tcg-profile.py`); `CDS=a.mds:b.iso` game discs, `VGA=d3dpt`, `QEMU_BIN=` an A/B binary, `FPS=<s>` the frame probe; second pass `DFILTER=` + `tools/tcg-hot.py` |
| `tools/tcg-fps.py <qmp sock> <s>` | a guest's VGA frame rate from outside (distinct screendumps per second) — the honest before/after number for a software-rendered game; blind to frames presented through the 3D device |
| `tools/xp-moto-race.sh <image> <name> [qemu]` | Moto Racer 1997 into a practice race headless (demo → title → Solo → Practice → Start over QMP, throttle held) and its fps; the M9 track's game oracle. `RACE_SAMPLE=<s>` profiles *in the race* (`<out>/race/report.txt`; the runner's own sample is of the demo), `RACE_MEMSAVE=addr:size,…` captures code pages twice for `smc-diff.py`, `RACE_DELAY=<s>` picks the moment (the fps depends on the track section), `FPS_RATE=` the probe's dumps/s (25 saturates above ~20 fps), `PERFMAP=0` drops `-perfmap` (12 % of the vCPU on a retranslation-bound game) |
| `qemu-embed: input:` lines (player stderr) | the embed input queue's drain latency, key down/up pairs delivered in one drain (zero-length presses), drops — printed only when something is off |

Guest images are not in the repo (`~/vms/win98.qcow2`, `~/vms/winxp.qcow2`;
wglgears lives at `C:\WINDOWS\Desktop\GAMEDIR`; on Linux `~/vms/scratch.img`
is a FAT32 disk attached as `-hdb`, E: in XP, read with `mcopy -i img@@1048576`).
**The user's own images are read-only for a session** — they play on them by
hand and keep their own installs in them, and most of the guest tools boot
`-hda <image>` with no `-snapshot`, so a run writes what it booted. Boot a
qcow2 overlay (`qemu-img create -f qcow2 -b <image> -F qcow2 <scratch>`) or a
copy, never the image; reading one (`qemu-img info`, a 7z listing) is fine.
While an overlay's QEMU runs, the backing image holds a **read lock** and
anything writing it fails with "Failed to get write lock" — sequence those
runs.
The Direct3D device test loop is in `docs/00-status.md`'s cheat sheet. **End scripted Win98 runs with a
Start-menu shutdown** (`qmpc.py … keys ctrl+esc`, `keys u`, `keys ret`),
never by killing the player — a killed VM leaves the FAT dirty and every
next boot runs ScanDisk. A QMP `screendump` shows only the VGA surface,
which is frozen while 3D is active; use the headless dump for 3D frames.

## Gotchas that cost a day each (details in docs/00-status.md)

- **Never run two builds that share the `qemu/` tree at once.**
  `scripts/build-windows.sh` re-applies the patch queue over it
  (`prepare-qemu.sh`) while `scripts/package-flatpak.sh`'s
  flatpak-builder is copying that same tree into its sandbox, and the
  copy comes out half restored and half patched. It fails deep in QEMU's
  compile, with a missing header from a file version neither build is
  using (`hw/3dfx/glidept_mm.c: hw/core/sysbus.h: No such file`,
  2026-09-07). The native and Windows *outputs* are separate
  (`build/qemu` vs `build/win/qemu`) — the *sources* are not.

- **A build belongs to one checkout and is never shared.** That covers
  every artefact, not just QEMU: `build/qemu` and its binaries, `target/`,
  the DXVK build and the D3D executor, the Glide wrapper, the guest-tools
  ISO, the 9x/XP driver binaries. Every worktree builds its own and runs
  only its own; a session must not borrow another checkout's outputs, point
  `QEMU_BIN` / `QEMU_IMG` (or any other `*_BIN`-shaped variable) at one,
  configure its sources into one, or run a script from another checkout's
  directory. Two reasons, both paid for already. A borrowed build
  is a build of *someone else's* patch queue: the branch under test is
  not the branch running, and a `D3DPT_PROTO_VERSION` or `D3DPT_FB_VERSION`
  bump on either side shows up as `protocol mismatch` or a guest that
  never attaches. And meson records an absolute source path, so a worktree
  that configured into the main `build/qemu` silently makes every later
  build there compile the *worktree's* sources — a fix that passed stops
  passing with no change to explain it (check `build/qemu/meson-logs/`'s
  "Source dir" first when that happens). `scripts/build.sh` in the
  checkout you are working in is the whole answer; the cost is ~15 min
  once, and it is cheaper than one wrong verdict.

- **Four traps in driving a headless guest run**, each of which looks like
  the thing under test failing. A `pgrep -f '<pattern>'` whose pattern
  appears in the calling command line matches the wrapper and never ends —
  and `pkill -f` kills the session's own shell (exit 144); use a literal
  that cannot self-match (`patter[n]`) or the background-task notification.
  To find or stop QEMU use `ps -C qemu-system-i386 -o pid=`, never
  `pgrep/pkill -x`: the name is 17 characters, `comm` truncates at 15
  ("qemu-system-i38"), so `-x` matches nothing and silently leaves a process
  holding the image's write lock. An output directory deep in a scratchpad
  path makes the QMP socket fail with `AF_UNIX path too long` and the run
  silently does nothing — keep `OUT=` short. And editing a bash script while
  an instance of it is running breaks that instance; copy it first.

- **`configure`: "found no usable distlib, please install it"** — QEMU
  9.2's `mkvenv` imports `distlib.scripts` *and* `distlib.version`, and
  pip ≥ 26 trimmed its vendored copy (`scripts` yes, `version` no), so
  the fallback fails. Install the real `distlib` for that interpreter (the
  Flatpak manifest ships a wheel as a build-only module); it is not a
  Flatpak-specific problem, any modern-pip environment hits it.
- **On macOS, `/opt/homebrew/lib` on `DYLD_LIBRARY_PATH` breaks every image
  decode in the process.** dyld searches that variable by leaf name ahead of
  the path an image asked for, and ImageIO `dlopen`s its codecs as
  `libGIF.dylib` / `libPng.dylib` / `libTIFF.dylib` / `libJPEG.dylib` — all
  four of which that directory answers with a Homebrew library on this
  case-insensitive filesystem, so ImageIO calls a plugin ABI into a stranger
  and takes `SIGBUS` at `0xbad4007`. It killed the player on its first mouse
  grab (winit hides a cursor by decoding a GIF; ours is raw RGBA now).
  `scripts/test.sh` and `tools/tcg-profile.sh` therefore put only
  `/opt/homebrew/opt/vulkan-loader/lib` — the loader DXVK needs, and nothing
  else — on that variable. Unsetting it at runtime does not help; dyld read
  it at exec. Doc 00's gotchas has the diagnosis.
- macOS embed backend: never call `gl*`/`CGL*`/`IOSurface*` by link — the
  build also links XQuartz's Mesa libGL and the symbol binds there (a GLX
  library that sees no CGL context and silently no-ops). `dlsym` from the
  OpenGL.framework handle, the same one the guest dispatch table uses.
- The native Mesa backend (`mglcntx_linux.c`, the GLX one, on Linux **and**
  macOS since SDL went) is linked **weak** (patch 31) so
  `embed/mglcntx_embed.c` overrides it inside the embed library only.
- **QEMU is built with only what we use** (2026-09-07). Four families of
  optional host library are disabled outright, because auto-detection
  otherwise makes the build depend on what the machine happened to have —
  which is how this box, the Mac and the Flatpak SDK end up with three
  different `libqemu-embed`. **Display:** `--disable-sdl --disable-gtk
  --disable-cocoa --disable-curses --disable-spice`; the player is the
  front end (the embed library appends `-display none` itself and brings
  the 3D provider, patch 30). **Audio:** `--disable-alsa --disable-pa
  --disable-pipewire --disable-jack --disable-oss --disable-sndio
  --disable-coreaudio --disable-dsound`; the player's sound is the `embed`
  audiodev (patch 20), and `none` and `wav` are always built, which is
  what the headless tools use. **Network:** `--disable-af-xdp
  --disable-vde --disable-bpf` (libbpf's one consumer is virtio-net's eBPF
  RSS, and the bundles write pcnet / rtl8139); **slirp stays** — every
  bundle says `-netdev user`. **Block:** `--disable-curl --disable-libssh
  --disable-libiscsi --disable-libnfs --disable-rbd --disable-glusterfs
  --disable-blkio`; every drive is a local file or a disc image through
  our own `cdimage` driver. Plus `--disable-brlapi`.
  `libqemu-embed-i386.so` went from 175 shared libraries to **93**.
  `--disable-dsound` needed patch 23 to mean anything: QEMU 9.2's guard
  treats *disabled* like *enabled*, so DirectSound went into every Windows
  build regardless. DXVK matches: patch 04's headless WSI only,
  `DXVK_WSI_DRIVER=Headless`. Two consequences to remember: **standalone
  `qemu-system-i386` has no 3D** (it registers no context provider, so
  pass-through is refused cleanly and the VM keeps running), and **it
  opens no window** — QEMU falls back to starting a **VNC server on
  `localhost:5900`** when no `-display` is given (`system/vl.c`), so
  `-display vnc=:0` is how you look at a guest by hand, and `-audiodev
  none` is what it can play into. Anything scripted passes both already.
  The `no-optionals` check in `scripts/test.sh` guards all of it.
- Never exit the process while the QEMU thread is alive (QEMU's atexit
  handlers race `qemu_cleanup`); the player joins it, headless paths use
  `_exit`. A guest power-off ends the loop while the UI still holds the
  handle: there is a stop/release handshake for that.
- An occluded player window gets no swapchain image; per-frame work that
  must not stall (importing zero-copy slots) runs on the wake event.
- Benchmarks inside a DOS `.COM` must keep data on a separate page from
  code, or QEMU's self-modifying-code invalidation dominates.
- Win98 must be an **ACPI install** or PCI hot-adds are never seen (no USB
  tablet, AC'97 or NIC — "Plug and Play BIOS" with a yellow ! in Device
  Manager). Setup reads the legacy BIOS date at F000:FFF5 and wants it at
  or past its own `ACPICheckDate`, 12/01/99, otherwise a match in
  `BIOSINFO.INF`'s `[GoodACPIBios]`; SeaBIOS says 06/23/99 and we match
  nothing, so `prepare-qemu.sh` stamps every `pc-bios/bios*.bin` to
  12/31/99 (doc 06, the `bios-date` check) and **a plain `SETUP` installs
  ACPI — user-confirmed 2026-09-07**; `SETUP /p j` is no longer needed. An
  image installed before the stamp is still PnP-BIOS and is repaired
  through Device Manager (build-macos.md), not reinstalled. Guest wrappers
  must be msvcrt-linked and `-march=pentium3`.
  **Run Win98 under TCG, not KVM** (which is also the launcher's default
  for the family): under `-accel kvm` `~/vms/win98.qcow2` loses Explorer
  at startup — an "illegal operation", then *SHELL32.DLL is linked to
  missing export SHLWAPI.DLL:GetFileAttributesA* — so there is no Start
  menu and no way to drive the guest (2026-09-06). The same image is
  fine under TCG.
- **A guest frozen on its first frame with a blinking caret is not a hung
  emulator, it is a guest that never gets a timer interrupt.** The caret is
  `vga_draw_text`'s, drawn on the host side with no guest running at all,
  so it blinks over a dead machine — which is why a boot menu can sit
  there with a live caret and a countdown that never moves. Diagnose with
  `info registers` twice (same EIP = not moving), `info pic` (an unmasked
  `irr` bit with `isr=00` = an interrupt pending and never taken) and
  `info lapic` (`LVT0 masked` = the APIC is swallowing the i8259). That
  exact case was Win98's restart, fixed by patch 22
  (`tools/win98-reboot-test.sh` guards it).
- The **Win98 display driver** (M10, doc 19) is 16-bit, and that changes
  two things nothing warns about. `d3dpt-vga`'s register BAR is
  `valid.min_access_size = 4`: a 16-bit compiler turns `*(DWORD __far *)p`
  into two word accesses, which QEMU answers with zero and drops — the
  driver's registers must be hand-written 32-bit accesses. And the mini-VDD
  maps the adapter where ring 3 can reach it — `_PageReserve(PR_SHARED)` +
  `_PageCommitPhys(PC_USER|PC_WRITEABLE|PC_INCR)`, not `_MapPhysToLinear`,
  which lands in the system arena — where **this VMM refuses `PC_PRESENT`**,
  silently, and `PC_INCR` is what stops all 128 MB aliasing onto one page. Three more silent refusals are now
  build-time checks in `build-driver9x.sh`: an NE relocation naming a
  segment wlink dropped (KERNEL refuses the module), a VxD whose DDB is
  not at offset 0 of its code object (the VMM ignores it), and an export
  with no DGROUP load — **Open Watcom takes a function's attributes from
  its first declaration**, so a DDK prototype without `__loadds` strips it
  from the definition and the export reads the driver's globals through
  the caller's DS (`ValidateMode`, doc 19 §18: Display Settings offered one
  resolution because the applet died on the first mode it asked about).
- **A Win98 desktop that stays "glitched" after a full-screen DOS box is
  an unrepainted desktop, not a hang** (doc 19 §29, 2026-09-09). The four
  mini-VDD screen-switch entries put the *adapter* back; the *desktop* comes
  back only because the display driver hooks INT 2Fh AX=4001h/4002h, marks
  the DIB Engine's PDEVICE `BUSY` on the way out and calls USER's repaint
  entry (ordinal 275) on the way back — without that, Windows sits idle
  behind the DOS program's last frame forever. `info registers` twice with
  EIP moving and `HLT=1` is how to tell it from a dead machine
  (`tools/win98-game-test.sh` writes `OUT/hang.txt` with exactly that when
  the power button is not answered).
- **A Win98 guest on `d3dpt-vga` that "stops at a black desktop" has
  usually faulted** — and since 2026-09-09 the blue screen *shows*: the
  mini-VDD turns the linear frame buffer off when the VDD switches to VGA
  for it (`PRE_HIRES_TO_VGA`, and `SAVE_MESSAGE_MODE_STATE` for the message
  screens that skip the switch; doc 19 §29, `tools/win98-bsod-test.sh`
  guards it). With an older VxD in the image, or in a harness that wants
  proof afterwards, the message is a VGA *text* screen the linear frame
  buffer hides: QEMU's VGA core keeps its planes interleaved four bytes to a
  character cell from VRAM offset 0, so the text page lands in the first
  32 KB of the frame buffer and scans out as the band of coloured noise
  across the top. Nothing switches the mode back, because putting our
  adapter back is the mini-VDD's job. `tools/win98-driver-test.sh` reads
  that page out of VRAM and prints it after every run — believe it over the
  screendump (doc 19 §15).
- **`display.drv=pnpdrvr.drv` is correct, not a fallback**: it is what
  Windows writes for every PnP display driver (the inbox Cirrus in the same
  image included), there is no such file, and it resolves through the
  adapter's registry key. A 9x display INF therefore needs a **`DelReg`**
  clearing `Ver`, `DevLoader`, `DEFAULT`, `MODES` and `CURRENT` first — an
  adapter that has been running the inbox VGA still holds a `CURRENT` key
  naming *that* driver, an AddReg does not remove what it does not mention,
  and the leftovers are what GDI resolves through. Ours lacked it and so
  never loaded from PnP; with it, and with `MODES\4\…` rows handing the
  16-colour modes back to `vga.drv`/`supervga.drv`, install + restart brings
  up both halves with nothing naming them (doc 19 §16, 2026-09-07).
- **End a scripted Win98 run with the ACPI power button** (`system_powerdown`),
  not keystrokes: a modal dialog swallows them, and a machine that does not
  power off leaves the FAT dirty, so the *next* boot comes up in **safe
  mode** — no driver, no VxD, an empty debug log, which reads exactly like
  the thing under test having failed.
- Win98 and XP both run `-vga none -device d3dpt-vga` with our driver
  (docs 19 and 15; Win98 since 2026-09-07 — the tools in the table above
  still boot their own `-vga cirrus` machines, which is where the inbox
  driver is still exercised). **Since 2026-09-07 the adapter is a choice**:
  the wizard has a display-adapter picker (`bundle::Video`, `video` in the
  bundle) offering `d3dpt` or `cirrus` on both Windows families, `std` or
  `cirrus` on the new Other family, and — since 2026-09-09 — `std` or
  `cirrus` on DOS, the one family where the adapter is not a driver
  question but *which VESA BIOS the title finds*, and where changing it
  installs nothing. **DOS's default moved to `std` with that** (user
  decision): it had `-vga cirrus` hardcoded from the day the family
  landed, and the Cirrus is `Other`'s adapter. The **defaults are
  opposite ends of that pair** (`bundle::video_choices`, first entry wins):
  XP starts on ours, Win98 on the `cirrus` and Windows' in-box driver
  (2026-09-07, user decision — the 9x driver is much the newer of the two,
  so a new 98 machine comes up on the driver Windows already has and is
  moved to ours deliberately). Changing it under an installed guest is a
  hardware change — new adapter, plain VGA, wants a driver — which the
  wizard says in orange. Without our driver installed the adapter is
  a plain VGA (on XP that is vga.sys, 800×600×4), and `-vga std` has no XP
  driver at all.
  Kernel-mode debugging = the device's DEBUG register → QEMU log; never a
  debugger. Miniport headers: `ntdef.h`+`ddk/miniport.h`, **not** `ntddk.h`.
  dxg drops the whole HAL for `DDCAPS_GDI`, palette caps and colour-key
  caps without a Blt callback alike (the driver keeps a never-called
  `DdBlt` for that, and never claim `DDCAPS_BLT` without a real blitter:
  a declined `DdBlt` is E_NOTIMPL to the app on XP, not a HEL fallback); a
  flip on NT swaps the two surfaces' roles, not their memory (never
  re-register the chain in `DdFlip`); GDI through `GetDC` writes VRAM with
  no driver callback (the executor's target shadow catches it); the
  hardware cursor is register set v4 (the guest's shape becomes the
  player's window cursor; a v3 driver refuses the device: reinstall from
  the ISO) — doc 15.
- **A Win98 game asking for 320×200 wants DirectDraw's own Mode X, not a
  driver mode** (doc 19 §30, 2026-09-10). Its recipe is `DDSCL_ALLOWMODEX`
  plus a `DDSCAPS_SYSTEMMEMORY` flipping primary: the runtime switches the
  display driver out, drives the VGA core and the DAC itself and copies the
  chain into planar VGA memory on every flip. A driver that *lists* 320×200
  (or sets `DDHALINFO_MODEXILLEGAL`) gives that game a linear mode with a
  system-memory primary nothing presents — black, palette correct. Ours
  lists no 320-wide mode, like the reference driver. Headless, the switched-
  out screen shows the VGA core; `d3dpt-vga: vga core cr1=… sr4=…` in the
  QEMU log names the mode it is in.
- **A game that runs far too fast is a missing frame limiter, not a clock
  bug**: titles of the era pace themselves by the DirectDraw flip chain, so
  `Flip` must block until the flip is scanned out (doc 15, "The flip chain's
  vertical blank"). `d3dpt-vga: N page flips in 5.0 s` in the QEMU log is the
  guest's real frame rate; no line means the game blits to the primary and
  nothing in the display path can pace it. `ddflags=32768` turns the vertical
  blank off for the A/B.
- **A game that "freezes" on the D3D device is usually showing a message box
  you cannot see**: the player used to show only 3D frames once a device
  existed. Since 2026-09-04 it falls back to the VGA surface after 1 s without
  a presented frame when the guest drew on it; `tools/xp-game-test.sh` with
  `SHOTS=` and `DRW_AFTER=` shows the box and the stacks headless.
- **KVM `-cpu host` breaks Max Payne's level loading** ("Corrupt JPEG data"
  boxes: its CPUID-dispatched JPEG decoder mis-decodes on a modern family);
  `-cpu pentium3` under KVM loads and plays. Prefer an era CPU model for games.
- Guest DLLs built with modern mingw-w64: `psapi.h` maps to Windows 7's
  `K32*` kernel32 exports unless `PSAPI_VERSION 1` is defined; XP's loader
  then blocks the process in a hard-error dialog before `DllMain` runs.
- x87 under TCG is helper calls into 80-bit softfloat; patch 05 does the
  53/24-bit common case on the host FPU and patch 06 (doc 13) keeps the
  stack as host doubles inside TCG at PC=53 and PC=24. Test any change
  with both x87 tools above. SSE is patch 11 (doc 16): inline only when
  PE is already sticky in MXCSR; MMX/integer/permutes are patch 12
  (`simd-fast`); test both with `tools/sse-guest-test.py`.
