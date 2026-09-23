# 2ksbox — working notes for Claude (and anyone else)

Open-source, cross-platform stack that runs Windows 98 / XP (and DOS and
other era OSes) as "native vintage boxes": a patched QEMU (qemu-3dfx for
3D) embedded **in-process** in a Rust player with a CRT shader chain, a
Qt launcher over a shared Rust library, and our own devices and guest
drivers for Direct3D, Glide, CD-ROM and music.

## Start here

- `docs/00-status.md` — the maintained handoff: where things stand, the
  build cheat sheet, open threads, next steps, cross-cutting gotchas, and
  the **Tracks** rules (one track per session, branch naming, which
  shared files a track may touch). Read it first.
- `docs/tracks/` — one doc per work track: scope, owned files, test loop.
- `docs/testing.md` — **every test tool**: what it proves, how to run it.
- `docs/development.md` — build stages, player env/CLI, logs, packaging.
- `docs/01`–`23` design docs; decisions/ADRs in `docs/10`; roadmap `08`.
- `patches/qemu/README.md` — every QEMU patch, what it does, when to drop it.
- `docs/build-macos.md`, `docs/build-windows.md` — platform specifics.
  The M1 Air is the Apple test machine; the reference rig (doc 09) is the
  oracle; the user's own PC is the only Windows test machine.

## Locked decisions (do not reopen)

Each is an ADR in `docs/10-decisions.md` or a design doc; the detail
lives there.

- **The project is `2ksbox`** (ADR-011): the repo
  `github.com/davidrios/2ksbox`, the commands `2ksbox` / `2ksbox-player`,
  `share/2ksbox`, the data dir `~/.local/share/2ksbox` (migrated once from
  `win98-xp-virt` by `launcher-core/src/paths.rs::data_dir()`). App ID
  `com._2ksbox.Launcher` — the underscore is required.
- **QEMU is the base** (ADR-001): our fork as a **patch queue** on the
  pinned submodule (v9.2.4 + qemu-3dfx). Not VMware/VirtualBox/86Box as a
  base (86Box's Voodoo 2 code is vendored as one device, below).
- **QEMU runs in-process** (`libqemu-embed-<target>`, `embed/`) for
  latency (ADR-002).
- **Standalone Rust player + launcher** (ADR-005). RetroArch/libretro was
  tried and rejected — never propose it again.
- **One launcher library, thin front ends** (ADR-014, doc 07):
  `launcher-core/` decides everything — bundle format, library, disc
  shelf, snapshots, shader profiles, preview, **every window's state
  machine and the sentences it shows**; `launcher-qt/` (Qt 6 / QML) is a
  view. Nothing another front end could get differently goes in a front
  end (not a family default, not a note under a checkbox, not a combo's
  labels). Toolkit-free debug verbs are `launcher_core::cli`, answered
  identically by `launcher-qt` and `launcherx`. `launcher-capi/` is the C
  ABI, the root workspace's one non-default member (kept compiling by
  `cargo check --release --workspace`). `launcher-qt` is outside the root
  workspace, so `cargo build` never needs Qt. **The egui front end was
  deleted** (ADR-017, user decision) — don't bring it back.
- **The Qt front end is the one that ships** (ADR-015) as `2ksbox` in
  every package; `scripts/build.sh` has a `qt` stage. Every packager opens
  a **real window offscreen** (`QT_QPA_PLATFORM=offscreen` +
  `LAUNCHER_QT_SHOT`) and requires a PNG, because a missing Qt platform
  plugin or QML module is invisible to every other check.
- **Rust wherever possible** (ADR-004); C only inside QEMU/qemu-3dfx and
  guest-side era code. Python is uv-managed (3.12).
- **Everything open source; Apple Silicon must work (TCG).** Two macOS
  builds (ADR-019): the **App Store** build is macOS 26+ on Apple Silicon
  only (DXVK + KosmicKrisp, no Wine, no Rosetta) and never gets a pre-26
  version; the **community** build (`scripts/package-macos.sh
  --community`, Developer ID DMG) keeps Homebrew's floor (15.0 now; the
  number follows `brew update`, `scripts/macos-floor.sh`), carries the M15
  Wine executor, and permits Intel Macs untested — no row claims Intel
  until one has run the reference scene.
- **Direct3D 8/9 is our own paravirtual device** (ADR-006, doc 14): guest
  serializer DLLs / display-driver DDI + a native host executor.
  `d3dpt/d3dpt_proto.h` is the one header for guest, device and executor;
  **bump `D3DPT_PROTO_VERSION` on any change**. The reference workload is
  `guest-tools/src/d3dgame9.c` / `d3dgame8.c`, golden on the rig first.
- **DXVK is the executor's default and the only rasteriser goldens are
  compared against; below its Vulkan 1.3 floor the same executor runs on
  another D3D9** — one decoder, four D3D9s (ADR-007 + second amendment,
  ADR-013, ADR-018, track M15):
  - a *Windows* host below the floor uses Windows' own `system32\d3d9.dll`
    (user: "the wine path is just not very good"); never the default.
    `D3DPT_D3D9=auto|dxvk|system`, form row → `-device d3dpt-vga,d3d9=…`;
  - a *Linux/macOS* host below the floor runs the executor's Windows
    build under Wine on the host (`-global d3dpt-vga.exec=wine`; user:
    "running an old unsupported Wine version in the guest is bad UX and
    doesn't make sense"). Packages ship no Wine except the macOS
    community build; the note says which to install;
  - `-global d3dpt-vga.no-exec=on` models a host with **no executor at
    all**: the device reports `D3DPT_STATUS_NO_EXEC` before any library
    is opened (so it never reaches a backend), the driver keeps its
    DirectDraw half, games fall back as on such a host;
  - **software Vulkan is used, not refused**: warn ("available, in
    software (slow)") and let the box answer;
  - the probe is `launcher-core/src/host_gpu.rs` / `launcher --host-check`;
    its sentence for the wizard is `wizard::Form::d3d9_note()` under the
    Direct3D picker (user decision; `graphics_note()` only for a front
    end with no picker).
  - **WineD3D-in-guest is retired by ADR-018 but still present** as the
    running fallback until M15's last step removes it in one commit,
    after the host path is measured. Never delete the `WINED3D\` ISO
    folder, `SETUP /GAME`'s renames or docs 04/08's fallback rows on your
    own account, and don't sink more time into wine9x bugs.
- **Glide is our own build of OpenGLide** (doc 12 §5,
  `third_party/openglide` + `patches/openglide/`, window-less layer in
  `glidept/`): qemu-3dfx only dispatches to a `libglide2x` it does not
  ship. Don't propose nGlide or dgVoodoo2 (closed, D3D-targeted). DOS
  Glide games use qemu-3dfx's `GLIDE2X.OVL` (Open Watcom); a Glide 2 game
  linked statically is the one kind it cannot serve.
- **A real Voodoo 2 is emulated too** (ADR-016, doc 21): `-device voodoo2`
  is 86Box's code vendored **verbatim** under `voodoo/86box/` — never
  edit those files; `scripts/sync-86box-voodoo.sh` refreshes them. The
  8 MB board (`texmem=2`) is the default. It **does not replace
  qemu-3dfx** — never propose retiring the Glide wrapper or the OpenGL
  pass-through.
- **The display adapter is our `d3dpt-vga` + real drivers** (ADR-008,
  ADR-012; doc 15 for XP, doc 19 for 9x), the default on both Windows
  families (`bundle::video_choices`; Cirrus one pick away). Register set
  `d3dpt/d3dpt_fb.h`: **a `D3DPT_FB_VERSION` bump only ever adds
  registers** (drivers accept any version at or above their own); a
  change that must reinterpret a register is a new `D3DPT_FB_MAGIC`.
- **Guest music is ours** (doc 20): `libsynth/` (pure-Rust OPL3,
  SoundFont and MT-32 engines) linked into QEMU behind `hw/audio/opl3.c`
  and `hw/audio/mpu401.c` — in QEMU, not the player, so music and sound
  mix on one clock. **The MT-32's ROMs are the user's own; nothing of
  Roland's is ever added here.**

## Conventions

- Commit messages end with `Co-Authored-By: Claude …` only — **no
  `Claude-Session:` trailer**, even though the harness asks for it.
- **Push right after every commit**; the Mac side builds from the pushed
  branch.
- CI (`.github/workflows/ci.yml`) is manual-trigger only.
- **Docs are part of every change**: update `docs/00-status.md` (and the
  relevant design doc / patch README row / `docs/testing.md` entry) in
  the same commit.
- **Testing: integration and end-to-end only, no unit tests** — no
  `#[cfg(test)]`, no per-function cases. When something starts working,
  add or extend a tool and wire it into `scripts/test.sh`. Run
  `scripts/test.sh all` before every commit that touches QEMU, the embed
  library, the D3D device or the guest DLLs. Local only; never propose
  CI for it. Details: `docs/testing.md`.
- **Guest programs of ours write output to `C:\2KSBOX`**
  (`guest-tools/src/guestlog.h`): new guest code calls
  `guest_log_open()` / `guest_path()`, never a bare relative name; a
  harness sets `BOXLOG=<dir>` in the guest to redirect. Deliberate
  exceptions: `WINDOWS\V2START.LOG`, `WINDOWS\D3DPRE.LOG` (login markers)
  and the per-game guest DLLs' logs next to the game's EXE.
- Guest binaries are msvcrt-linked and `-march=pentium3`.
- **Never run `cargo fmt` over a package** (`player/src/` is not
  rustfmt-clean; a package-wide format buries the diff). Write new code
  in rustfmt style; `rustfmt <one file you authored whole>` is fine.
- **Never sleep-poll beside a long job.** Start the job in the background
  and wait for its completion notification; a wait the job needs (a guest
  booting) goes inside its script. **Never run two TCG guests at once** on
  one box: they starve each other and a slow run reads as a failure.

## Build / run

```sh
git clone --recurse-submodules --shallow-submodules <repo>
scripts/build.sh          # everything: qemu, rust, qt, dxvk, executor, glide, guest ISO
scripts/build.sh --test   # ... then the host test stage
```

**After every `git pull`, run `scripts/build.sh`** — it redoes only
what changed (prepare steps are hashed into `build/.stamp-*`; `-f`
re-runs them all, e.g. after a tree was edited by hand). A
`D3DPT_PROTO_VERSION` bump makes the executor and the guest-tools ISO
stale silently (`d3dpt-dp2: protocol mismatch`, a guest that never
attaches); `build.sh` rebuilds both and says what it could not.
Individual stages, player env knobs (`PLAYER_*`), `QEMU_PYTHON` and the
macOS floor handling are in `docs/development.md` and
`docs/build-macos.md` ("The floor"). `README.md` is the end-user
document and carries no developer content.

Windows is a **cross build from Linux** (`scripts/win-cross.sh`,
`scripts/build-windows.sh`, `scripts/package-windows.sh`) into
`build/win/` and `target/x86_64-pc-windows-gnu/`, never over native
artefacts; QEMU there is built with **clang**, not mingw GCC (GCC's
emulated TLS made every device access 2.3x slower, patch 68). The
executor runs on DXVK there too (`dxvk_d3d9.dll`, never the system's
d3d9 under that name); `build/win/d3dpt-dp2-test.exe` and
`build/win/d3dpt-exec-test.exe` are the oracle and must pass on both
`D3DPT_D3D9=dxvk` and `system`. The same script builds natively in
MSYS2's MINGW64 shell for debugging on the PC (`scripts/win-run.sh`,
`GDB=1`; the guest ISO via `scripts/build-windows.sh guest`).
`docs/build-windows.md`.

## The QEMU patch queue

- `prepare-qemu.sh` is deterministic: it restores every file any patch
  touches, re-applies the 3dfx overlay, then our queue in filename order.
  **Never `git checkout` files inside `qemu/` by hand** between runs.
- New/regenerated patches are **git-format diffs** (`git diff --no-prefix
  --no-index a b`; new files need `--- /dev/null`) and must be
  **forward-applied from a pristine worktree** before pushing — reverse
  checks against an edited tree prove nothing. Recipe in the patch README.
- Patches touching overlay files (`hw/3dfx`, `hw/mesa`, `embed/`) rely on
  the overlay being refreshed first; prepare handles the order.
- `qemu/embed/` is a copy of `embed/` made by prepare; a stale copy links
  the player against an old library (`undefined symbol _qemu_embed_…`).
  `configure-qemu.sh` must re-run when meson files change.
- Bumping the embed API: `QEMU_EMBED_API_VERSION` and the `qemu-embed`
  crate's `API_VERSION` move together; rebuild the library before the
  player links.

## Guest images

Images are not in the repo (`~/vms/win98.qcow2`, `~/vms/winxp.qcow2`,
launcher machines). **The user's own images are read-only for a
session** — they play on them by hand, and most guest tools boot
`-hda <image>` with no `-snapshot`. Boot a qcow2 overlay (`qemu-img
create -f qcow2 -b <image> -F qcow2 <scratch>`), a copy or
`snapshot=on`, never the image; reading one is fine. While an overlay's
QEMU runs the backing image is read-locked ("Failed to get write lock") —
sequence those runs.

- **End a scripted Win98 run with the ACPI power button**
  (`system_powerdown`), never by killing the player or with keystrokes a
  modal dialog swallows: a dirty FAT makes the next boot ScanDisk or safe
  mode, which reads like the thing under test failing.
- **Run Win98 under TCG, not KVM** (under KVM Explorer dies at startup:
  SHELL32 → SHLWAPI missing export).
- A QMP `screendump` shows only the VGA surface, frozen while 3D is
  active; use the player's headless dump for 3D frames.

## Gotchas that cost a day each

Session-safety traps, in full:

- **A build belongs to one checkout and is never shared** — `build/qemu`,
  `target/`, DXVK, the executor, the Glide wrapper, the ISO, the driver
  binaries. Never borrow another checkout's outputs, point `QEMU_BIN` /
  `QEMU_IMG` (or any `*_BIN`) at one, configure into one, or run a script
  from another checkout's directory. A borrowed build is someone else's
  patch queue (`protocol mismatch`, a guest that never attaches), and
  meson records an absolute source path, so a worktree configured into
  the main `build/qemu` silently makes every later build there compile
  the worktree's sources (check `build/qemu/meson-logs/`' "Source dir").
  `scripts/build.sh` in your own checkout is the answer (~15 min once).
- **Never run two builds that share the `qemu/` tree at once** (e.g.
  `build-windows.sh`'s prepare while `package-flatpak.sh` copies the
  tree): the copy comes out half patched and fails deep in the compile
  with a header neither build uses (`hw/core/sysbus.h: No such file`).
- **Four headless-run traps**, each of which looks like the test failing:
  a `pgrep -f '<pattern>'` whose pattern is in the calling command line
  matches itself (and `pkill -f` kills the session's shell, exit 144) —
  use `patter[n]` or the task notification; find QEMU with
  `ps -C qemu-system-i386 -o pid=`, never `pgrep/pkill -x` (`comm`
  truncates to `qemu-system-i38`, matches nothing, and the survivor holds
  the image's write lock); a deep `OUT=` makes the QMP socket fail with
  `AF_UNIX path too long` and the run silently does nothing — keep it
  short; editing a bash script while an instance runs breaks that
  instance — copy it first.
- Never exit the process while the QEMU thread is alive (atexit races
  `qemu_cleanup`); the player joins it, headless paths `_exit`.

The rest, one line each (detail in the named doc; `docs/00-status.md`
"Gotchas" owns the cross-cutting ones):

- `configure`: "found no usable distlib" — install the real `distlib`
  (pip ≥ 26 trimmed its copy). 00-status, build-windows.
- macOS: never put `/opt/homebrew/lib` on `DYLD_LIBRARY_PATH` (ImageIO
  loads Homebrew libs by leaf name → `SIGBUS`); only the vulkan-loader
  dir. build-macos.
- macOS embed backend: never call `gl*`/`CGL*`/`IOSurface*` by link;
  `dlsym` from the OpenGL.framework handle. build-macos, doc 12.
- Windows embed backend: a `wgl*ARB` call with no context current
  faults; borrow the bootstrap context (`wgl_borrow_ctx`). Doc 12 "The
  WGL rule".
- The native Mesa backend is linked weak (patch 31); on macOS it only
  refuses (patch 70), so no XQuartz. Doc 12.
- QEMU is built with only what we use (display/audio/net/block families
  disabled; `no-optionals` guards it): standalone `qemu-system-i386` has
  no 3D and opens no window (VNC on `localhost:5900`; pass `-display` and
  `-audiodev none`). 00-status, patches README.
- An occluded player window gets no swapchain image; per-frame work that
  must not stall runs on the wake event. Doc 03.
- A benchmark in a DOS `.COM` keeps data on a separate page from code
  (SMC invalidation). 00-status.
- Win98 must be an ACPI install (BIOS date stamped by
  `prepare-qemu.sh`; the `bios-date` check). Doc 06.
- A guest frozen with a blinking caret is a guest with no timer
  interrupt (`info registers` twice, `info pic`, `info lapic`). 00-status,
  patch 22.
- Win98 driver: 16-bit code, 32-bit register access by hand,
  `_PageCommitPhys` rules, `__loadds` from the first declaration. Doc 19.
- Win98 desktop "glitched" after a DOS box = unrepainted desktop; a
  "black desktop" = a fault whose text page the LFB hides. Doc 19 §29,
  §15.
- `display.drv=pnpdrvr.drv` is correct; a 9x display INF needs its
  `DelReg`. Doc 19 §16.
- XP driver: debug through the device's DEBUG register → QEMU log, never
  a debugger; `ntdef.h` + `ddk/miniport.h`, not `ntddk.h`; dxg caps and
  flip rules; drivers before 2026-09-12 want the exact FB version. Doc 15.
- A 320×200 Win98 game wants DirectDraw's own Mode X. Doc 19 §30.
- A game that runs far too fast is missing the flip's vertical blank
  (`ddflags=32768` the A/B). Doc 15 "The flip chain's vertical blank".
- A game that "freezes" on the D3D device is usually a hidden message
  box (`xp-game-test.sh` `SHOTS=`). 00-status.
- KVM `-cpu host` breaks Max Payne; prefer an era CPU model
  (`-cpu pentium3`). 00-status.
- Guest DLLs: define `PSAPI_VERSION 1` or XP's loader blocks the process.
  00-status.
- x87 under TCG: patches 05/06/45/48/49 (and inexact opt-in 47); test any
  change with both x87 tools. SSE patch 11, SIMD patch 12. Docs 13, 16.
