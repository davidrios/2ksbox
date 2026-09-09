# 8. Roadmap and milestones

Ordered so the riskiest bets are validated first (in-process embedding,
3D-output interop into wgpu, Apple Silicon TCG performance). Each milestone
ends in something runnable.

Reference-rig capture sessions (doc 09) slot in as inputs: rig benchmarks
before M1 (baseline for the Apple Silicon XP verdict), CRT photo set before
M2, ATAPI traces and disc dumps before M5, real-GPU screenshots during M3/M4.

## M0 — Foundation  ✅ (2026-09-02)

- Repo scaffold per doc 02; QEMU submodule pinned v9.2.4 (qemu-3dfx cadence);
  qemu-3dfx submodule; `prepare-qemu.sh` (overlay + patch + sign) and
  `configure-qemu.sh` (uv-managed Python); patched QEMU builds and runs with
  glidept/glidelfb/mesapt regions live — verified on Linux x86_64 (Arch) and
  **macOS Apple Silicon (M1 Air)**. Windows cross-built since 2026-09-06 (M11).
- Rust workspace: `player` (winit + wgpu 30 window, XRGB8888 test pattern
  with integer 4:3 viewport, mailbox present), `libdisc` (MSF/LBA + types),
  `launcher` stub. CI (manual trigger) for Linux/Windows/macOS-arm64.
- Detour: a libretro core was built and validated in RetroArch, then dropped
  (ADR-005).

## M1 — Architecture validation  ✅ (2026-09-02; Windows closed by M11)

Done, all through the in-process embed path:
- `10-embed-api` builds `libqemu-embed-<target>` from QEMU's meson; shim
  `embed/libqemu_embed.{h,c}` (lifecycle on one thread, display listener,
  input via bottom-half, VM control, audio ring, refresh interval; API v3);
  `qemu-embed` crate (hand-written FFI, rpath via `links` metadata).
- Display: FreeDOS and **Windows 98** render in the player — Linux and the
  M1 Air (dylib). sRGB-correct on macOS swapchains.
- Input: keyboard (scripted `PLAYER_KEYS` verified in FreeDOS; typing in
  Win98 on the Air), USB-tablet absolute mouse (never grabs), PS/2 relative
  grab with Ctrl+Alt+G release.
- Audio: `embed` audiodev → SPSC ring → cpal; verified (ring fills on
  FreeDOS `echo ^G`; Win98 plays sounds on the Air).
- librashader chain: `--shader <preset.slangp>` (submodule
  `third_party/slang-shaders`); verified with crt-lottes via GPU readback.
- Latency instrumentation (`PLAYER_LATENCY=1`), 16 ms refresh pull.
  **Measured on the M1 Air (Win98, crt-lottes, 2026-09-02):** publish→present
  p50 6–10 ms, p95 15–17 ms, max 18 ms — the vsync-phase floor at 60 Hz;
  Linux/Wayland reads the same. Meets the ≤ 1 host frame budget (doc 03).
- Spike A step 1: qemu-3dfx GL pass-through at 500+ fps on the Air
  (standalone `-display sdl`, SDL/native-OpenGL backend, no XQuartz at
  runtime; superseded by the window-less embed provider and zero-copy in M3).
- XP boot + TCG benchmark on the Air against the rig baseline (doc 09):
  XP boots in the player (sound, tablet, clean power-off), ~30 s to desktop;
  vs. the rig's P4 1.7: integer 1.3–2× faster (7-Zip), x87 FP 21 % (Super PI
  1M 9:49 vs 2:02), 31 % after patch 05 (6:33), and 104 % with patch 06
  (1:57) — `reference/benchmarks/README.md`.
- QMP over socketpair: `player/src/qmp.rs` — socketpair, id-matched
  synchronous `execute`, event queue drained on the UI thread;
  `PLAYER_QMP=1` logs every event, `PLAYER_QMP_EXEC='<json>'` runs
  commands after the first guest frame (verified: query-version/status/
  block, error classes, RTC_CHANGE events on FreeDOS).
- Windows host: closed 2026-09-06 by M11 (`docs/tracks/m11-windows-host.md`,
  `docs/build-windows.md`): cross-builds from Linux with mingw-w64, packages as
  portable zip, WHPX included.

## M2 — Pixel accuracy + input polish  ✅ (2026-09-05/07)

- ~~Mode analysis table (doc 03)~~ ✅ 2026-09-05 (`player/src/mode.rs`): geometry
  stage takes the display aspect from the table (320×200 is 4:3, not 1.6:1;
  so are 640×350, 640×400, 720×400 text, and mode X sizes), and the scanline count
  reaches the preset through `vga_mode` / `inter` — 320×200 draws 400 scanlines,
  640×480 draws 480. `player --mode-sweep` is the check.
- ~~Whole-pixel geometry~~ ✅ 2026-09-06: viewport size and origin rounded to
  integers to prevent fractional grid crawling during window resize; minimum inner
  window size clamped to 1x picture in physical pixels.
- ~~Native guest screenshots~~ ✅ 2026-09-06: Ctrl+Alt+S writes the guest's
  unscaled, unshaded native frame to `PLAYER_SHOT_DIR/2ksbox-NNNN.png`.
- ~~Event-driven geometry updates~~ ✅ 2026-09-07: mode analysis, minimum window
  size, preset scanline parameters, and fitted rect are computed on actual
  surface changes (`Gpu::guest_surface_changed` / `Gpu::resize`) rather than
  recalculated multiple times per rendered frame.
- Remaining: overscan crop options and final calibration against rig CRT photos.

## M3 — 3D for Win98 + Glide  ✅ (2026-09-02/06; design: doc 12)

- GL pass-through renders inside the player on Linux **and macOS** (patches 30–32,
  `embed/mglcntx_embed.c`, embed API v4): EGL surfaceless pbuffer on Linux,
  CGL + FBO stand-in on macOS. Win98 wglgears in the player: 420–450 fps.
- **Linux zero-copy** (GBM dma-buf ring → Vulkan import, API v5): 575–600 fps.
- **macOS zero-copy** (IOSurface ring → Metal, API v6) verified on the Air.
- **Glide pass-through with OpenGLide** ✅ 2026-09-06 (patch 33, `glidept/`):
  qemu-3dfx ships no host Glide library, so we build OpenGLide (LGPL, 121
  exports). Reversed window handshake (`GlideHostOps`) renders into our
  window-less context through the CRT chain.
- Verified in guest with `TESTS\GLIDETEST.EXE` (4 cases, 0 failed) through
  `tools/glide-guest-test.sh` in the player.

## M4 — Paravirtual Direct3D device (XP & Win98)  ✅ (2026-09-04; doc 14, ADR-006/007)

- **P0 spike:** DXVK d3d9 native on macOS over KosmicKrisp (ADR-007, macOS 26)
  and on Linux (RADV) — decided DXVK as host executor. Reference scene
  (`guest-tools/src/d3dgame9.c`, `d3dgame8.c`) golden on rig (P4 + GeForce 6200).
- **P1 transport + device:** SysBus `hw/d3dpt` (patch 40), protocol
  `d3dpt/d3dpt_proto.h`, decoder + executor `libd3dpt_exec`, guest `d3d9.dll`.
  XP D3D9TEST 640×480: 2840 fps on device vs 1100 on WineD3D.
- **P2 resources + fixed function:** vertex/index buffers, textures (RGB, DXT),
  transforms, lights, depth/stencil. D3DGAME9 byte-identical to native DXVK.
- **P3 shaders + queries:** SM1–3 vertex/pixel shaders, constants, occlusion
  and event queries, StretchRect, cube textures. `D3DFEAT9` byte-identical.
  Occlusion query polling yield fixed 2026-09-07 (`guest-tools/src/d3dfeat9.c`).
- **P4 D3D8 over d3d9:** `d3d8.dll` wrapper; D3DGAME8 byte-identical. Max Payne
  and GTA Vice City running on device.

## M5 — CD-ROM backend & DirDisc  ✅ (2026-09-04/06; docs 05, 17, `docs/tracks/m5-cdrom-backend.md`)

- **M5a libdisc (Rust):** cue/bin + CCD + MDS + ISO models, EDC/ECC,
  Q-subchannel synthesis, C API, `discx` tool; `cdimage` QEMU block driver
  (patch 50); ATAPI patch 51 (raw sector reads, READ CD, subchannel, raw TOC).
- **M5b CD-DA playback:** `audiodev` on `ide-cd`, mode pages, sample-accurate
  seeking; MCI stop command maps to `START STOP UNIT` (2026-09-07 fix).
- **M5c–e Protection checks & dumps:** SafeDisc 2.x, mixed-mode + CD-DA,
  and VOB ProtectCD verified with real game dumps (Age of Empires Gold, Moto
  Racer, The Settlers 3, FIFA 2002); negative control testing with `discx repair`.
- **M5g DirDisc (`isodir:`)** ✅ 2026-09-06 (`docs/tracks/m5-dirdisc.md`):
  host directory mounted as on-the-fly generated ISO 9660 + Joliet volume;
  XP and Win98 copy all files identical without burning disc images.
- In-guest disc shelf integration (`patch 52`, `cdshelf/cdshelf_proto.h`,
  `CDSHELF.EXE` / `CDSHELF.COM`).

## M6 — Companion launcher & packaging  ✅ (2026-09-06/07; doc 07, `docs/tracks/m6-launcher.md`)

- **Architecture (ADR-014):** `launcher-core/` library owns all logic, models,
  state machines, bundle formats, and debug CLI verbs.
- **Shipped front end (ADR-015):** `launcher-qt/` on Qt 6 / QML via cxx-qt
  installs as `2ksbox`. `launcher/` (egui) kept as maintained reference view.
  `launcher-capi/` exposes C ABI for external embedding.
- **Machine library & wizard:** 4 families (Win98, XP, DOS, Other), RAM bounds,
  CPU throttling for DOS (`cpu_speed`), display adapter choice (`d3dpt`, `cirrus`, `std`),
  **networking defaults to off on new machines** (avoids guest DHCP delays and popups; easily toggled on),
  seamless mouse toggle, emulation optimization toggles.
- **Disc shelf & snapshots:** natural-sorted disc shelf with live insertion/ejection;
  internal snapshot manager over QMP.
- **Shader profile manager:** animated preset detection, live preview with
  wgpu rendering, controlled `PathField` component, and clean model refresh in Qt launcher.
- **Packaging:**
  - Linux tarball (`scripts/package-linux.sh`) with relocatable layout and desktop entry.
  - Flatpak (`packaging/flatpak/`, `scripts/package-flatpak.sh`) on `org.kde.Platform` 6.10, fully offline build.
  - macOS `.app` / `.dmg` (`scripts/package-macos.sh`) with complete dylib closure, pruned unused Qt frameworks (saving 6 MB), dyld `@rpath` resolution checks, hardened runtime, Vulkan loader, notarized.
  - Windows portable `.zip` (`scripts/build-windows.sh`, `scripts/package-windows.sh`) cross-built via container with WHPX.

## M7 — XP guest display driver  ✅ (2026-09-04/05; doc 15, `docs/tracks/m7-display-driver.md`)

- **M7a framebuffer driver:** `d3dpt-vga` PCI adapter (stdvga core + register BAR),
  `d3dptvid.sys` miniport + `d3dptdisp.dll` display driver; zero-copy inside QEMU,
  host mode table.
- **M7b DirectDraw DDI:** VRAM surfaces, paced page flips against hardware frame
  counter, vertical blank waiting, hardware cursor (v4 register set).
- **M7c Direct3D DDI:** DX8 DDI landed (`D3DCAPS8`, hardware T&L, DX8 token
  stream rewrite, state sets, render-to-texture); vs 1.1 / ps 1.4 shaders (protocol v7);
  palettized P8 textures & color keying (protocol v8); DX3 execute buffers for
  1997 titles (`IDirect3DDevice::Execute`, Moto Racer); untracked GDI write
  shadowing; video-memory vertex/index buffers (protocol v9).

## M8 — CPU fast paths in TCG  ✅ (2026-09-04; docs 13, 16, `docs/tracks/m8-tcg-fastpaths.md`)

- **x87 shadow doubles (patches 05/06, doc 13):** host FPU at 53/24-bit precision;
  translator keeps x87 stack as doubles across instructions in TCG. XP Super PI
  1M on Air: 9:49 (softfloat) → 1:57 (faster than reference rig's P4 1.7 at 2:02).
- **SSE inline in TCG (patch 11, doc 16):** packed ops on vector unit, scalar in
  GPRs; native x86-64 `fmin_vec`/`fmax_vec`/`fcmp_vec` opcodes mapping to VMINPS/VMAXPS/VCMPPS;
  SSEBENCH clamp+cmp 10.0× over helpers.
- **SIMD/MMX inline in TCG (patch 12, doc 16):** integer/permutation ops inline;
  `tbl_vec` opcode; `mulsh_vec`/`muluh_vec` and `ssnarrow_vec`/`usnarrow_vec` native ops.
  Merged to `main`.

## M9 — TCG on Apple Silicon  ✅ (2026-09-05/06; `docs/tracks/m9-tcg-aarch64.md`)

- Profile-first tuning on aarch64 with `-perfmap` (patch 13).
- **Patch 14:** JIT write-protect toggle per thread (Super PI 1M: 1:36.2 → 1:25.3).
- **Patch 15:** vAPIC TB invalidation storm fix (ROM page invalidations clamped).
- **Patch 16:** softmmu TLB 4096-entry floor (eliminates context-switch thrashing;
  Moto Racer slow path: 48 % → 1.3 % of vCPU).
- **Patch 17:** REP MOVS/STOS fast path via host `memcpy`/`memset` (MOVSD 2.15 → 0.07 ns/elem).
- **Patch 18:** same-value SMC store filter (Moto Racer race: 7.3 → 21.7 fps at start, 39.2 fps at 60 dumps/s).
- **Patch 19:** TLS/RCU lock overhead reduction in code-page store path (standing start 40.4 fps).
- **Patch 20:** inline jump-cache lookup for `ret`/`call *`/`jmp *` (7-Zip +12 % compress / +7 % decompress).
- **Patch 21:** pinned guest registers x20–x28 across chained TBs (doc 18, opt-in).
- Merged to `main`.

## M10 — Native Win98 display driver  (Active; doc 19, `docs/tracks/m10-win98-driver.md`)

- Architecture (ADR-012): split XP display driver into OS-independent core
  plus thin OS layer.
- 9x driver model: 16-bit `d3dpt9x.drv` (DIB engine), ring-0 mini-VDD `d3dpt9v.vxd`,
  and ring-3 DirectDraw/D3D HAL. Built with Open Watcom v2.
- **Milestones achieved (2026-09-06/07):**
  - Toolchain and builds established (`guest-tools/build-driver9x.sh`).
  - Mini-VDD maps VRAM and register BAR; BARs survive entire boot.
  - GDI loads driver; mode switches to linear mode (e.g. 800×600×16).
  - VGA text-mode exception decoding from top 32 KB of VRAM.
  - `ExtTextOut` thunk argument fix.
  - PnP INF-only clean installation (`pnpdrvr.drv`, `DelReg`, `DRIVER9X\`).
  - `SETUP /ALL` clean 9x restart via detached process (`SETUP /REBOOTNOW`).
  - Win98 machine family defaults to `d3dpt-vga`.
  - Desktop shell painting resolved (boots straight to desktop at 800×600×16 or chosen mode).
- Next: core split and 9x DirectDraw/Direct3D HAL.

## M11 — Windows host support  ✅ (2026-09-06; `docs/tracks/m11-windows-host.md`, `docs/build-windows.md`)

- Cross-compilation container based on Fedora mingw-w64 (`scripts/win-cross.sh`).
- Built artefacts: `2ksbox.exe` (Qt launcher), `2ksbox-player.exe`, `libqemu-embed-i386.dll`,
  `d3dpt_exec.dll`, `qemu-img.exe`, firmware, guest tools.
- WHPX acceleration support detected and built in.
- Portable zip packaging (`scripts/package-windows.sh`).
- Windows-specific fixes: C-runtime descriptor translation (`qemu_embed_socket_to_fd`, embed API v7),
  debug logging (`launcher.log`, panic hook, `2ksbox-debug.bat`), cxx-qt emutls proxy fix (`once_proxy.cpp`),
  light-mode default appearance, WGL backend for OpenGL.

## M12 — Music  (Active; doc 20, `docs/tracks/m12-music.md`)

The half of a period machine's audio QEMU never had: an **OPL3** (Nuked)
at 0x388 and at the Sound Blaster's own base, an **MPU-401** at 0x330,
and behind it either a **SoundFont General MIDI** synthesizer or a
**Roland CM-32L**; QEMU's own **Gravis Ultrasound** offered as a card.
All three engines are pure Rust in `libsynth/`, linked into QEMU the way
`libdisc` is, so music mixes on the guest's own clock and a headless run
can capture it to a wav.

- **M12a the engines** ✅ 2026-09-09: `libsynth` + `synthx selftest`
  (OPL detection, an FM note, the shipped GPL-2 bank, running status).
- **M12b the devices:** `hw/audio/opl3.c`, `hw/audio/mpu401.c`, patch 25.
- **M12c the pickers:** `bundle::Sound` / `bundle::Music`, per-family
  defaults (doc 20 §6), both front ends, the `music` check.
- **M12d packaging:** the bank into `share/2ksbox/soundfonts/`.
- **M12e the guest end-to-end:** `tools/midi-guest-test.py`.

## M13 — Gamepads  (Planned; `docs/tracks/m13-gamepads.md`)

Opened 2026-09-09 out of doc 08's post-v1 list. QEMU has **nothing** to
build on — no gameport, no gamepad HID (`hw/input/hid.h` knows mouse,
tablet and keyboard only), and no joystick class in the input core — so
each guest-facing path is new code in the patch queue. Three of them,
because they reach different guests:

- **Step 0 the host end** ✅ 2026-09-09: `gilrs` in the player (evdev /
  XInput / GameController), the abstract pad and its shaping in the new
  `gamepad/` crate — shared the way `shader-chain` is, because the player
  must not depend on the launcher — `bundle::Pad` (`none` / `keys`) and
  its wizard row in `launcher-core`, `player --pads` and
  `--pad-sweep`, and `PLAYER_PAD_SCRIPT`, a synthetic pad, without which
  no headless check can drive a controller and the whole track is
  hand-testing only. The `pad` check guards it. Embed API v8 moved to
  path A, where it has a consumer.
- **Path C the key mapping** ✅ 2026-09-09: pad → the key events the
  player already sends, `--pad keys` written from `bundle::Pad`. Every
  guest, no QEMU patch, no analog. The mapping recomputes the wanted set
  of keys each poll and diffs it, so shared keys (d-pad *and* stick on
  the arrows) release only when the last holder does, and a stick crossing
  centre releases before it presses. Not yet seen arriving in a real
  guest: `tools/pad-guest-test.sh` is still to write.
- **Path A `usb-gamepad`** ✅ 2026-09-09 (patch 26): a whole device —
  `gamepad/qemu/dev-gamepad.c`, two analog sticks, an 8-way hat with a
  null state and twelve buttons in a six-byte report, built under
  `CONFIG_USB_HID`. Driven by absolute state through embed API v8
  (`qemu_embed_pad_state`), so a dropped update is corrected rather than
  leaving a button held; a second one is refused at realize. The
  joystick event class the plan wanted was dropped: QEMU's input core is
  built around consoles and a gamepad has no console affinity, so the
  shim calls the device directly — which is not upstreamable as it
  stands, and the track doc says so. XP, Win98 SE and Me should see it on
  their in-box HID stack with nothing to install; DOS cannot, and is not
  offered it. **No guest has enumerated it yet** —
  `tools/hid-descriptor-check.py` checks the descriptor bytes and the
  `pad` check watches a real `qemu-system-i386` attach it to the bus,
  but `tools/pad-guest-test.sh` is still to write.
- **Path B the gameport** (patch 27): four RC one-shots at 0x201,
  computed against `QEMU_CLOCK_VIRTUAL` on read. The only path that
  reaches DOS, and the 9x analog stack (`VJOYD` / `MSANALOG`). The
  busy-wait timing risk is bounded by the DOS family's existing
  `-icount shift=N,align=on`.

## Post-v1 candidates

Recording/streaming, CRT bezel packs, VRR pacing,
suspend/resume, a host MIDI port for a real module (doc 20 §8),
upstreaming campaign (libdisc, embed API).

## Standing risks

| Risk | Status & Mitigation |
|---|---|
| Spike A: GL→wgpu interop on macOS & Linux | **Resolved:** Zero-copy dma-buf ring on Linux and IOSurface ring on macOS (embed API v5/v6) flowing into wgpu presentation. |
| Host-side Glide support | **Resolved:** Custom OpenGLide build (`third_party/openglide`, patch 33, `glidept/`) integrated with window-less provider. |
| Apple GL deprecation | Long-term watch. ANGLE/Zink escape hatch remains designed if Apple drops OpenGL.framework. |
| XP-on-TCG performance on Apple Silicon | **Addressed:** Tested and benchmarked against rig P4 1.7. Patches 05, 06, 11, 12, 14–20 bring integer performance beyond P4 and x87 FP to 104 % of P4 1.7. |
| Fork drift from upstream QEMU | Pinned to v9.2.4 cadence; strict patch-queue discipline in `patches/qemu/` with automated `scripts/prepare-qemu.sh`. |
| QEMU dependency bloat | **Addressed:** Disabled unused UI, audio, network, and block backends; reduced shared library dependencies from 175 to 93. |
| Protection checks needing dump features | Clear UI guidance and error messages; `discx` provides inspection, verification, and negative-control repair tools. |
