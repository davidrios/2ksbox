# 2. Architecture: in-process QEMU, player + launcher, process model

## Decisions (locked — see doc 10 for rationale)

1. **QEMU runs in-process** with the display (ADR-002).
2. **Standalone Rust player + companion launcher** — no RetroArch/libretro
   (ADR-005 supersedes ADR-003).
3. **Rust where possible**; C only inside QEMU/qemu-3dfx and in guest-side
   code (ADR-004).

## Why in-process

Gaming latency: framebuffer, input, and audio must not cross a process
boundary. Linking QEMU as a library (UTM-style embed patches) gives zero-copy
framebuffer access (display listener → GPU texture), direct input injection,
and one clock domain for pacing. Consequences accepted: one VM per hosting
process (QEMU global state), a maintained QEMU fork (needed anyway for
qemu-3dfx + the CD backend), GPL-2.0 for everything that links it.

## Component map

```
┌────────────────────────── player process (Rust) ─────────────────────┐
│  ┌──────────── QEMU fork (C, linked as lib) ──────────────────────┐  │
│  │  TCG (ARM hosts) / KVM / WHPX                                  │  │
│  │  qemu-3dfx device + OpenGLide / host GL translation            │  │
│  │  d3dpt-vga / d3dpt device + host DXVK executor (D3D 5–9)       │  │
│  │  ATAPI raw-CD device ──► libdisc (Rust, staticlib, C API)      │  │
│  │  display listener ─┐   input inject ◄─┐   audio backend ─┐     │  │
│  └────────────────────┼──────────────────┼──────────────────┼─────┘  │
│           libqemu_embed.h (bindgen)      │                  │        │
│  ┌─────────────────────┼─────────────────┼──────────────────┼─────┐  │
│  │  triple-buffered fb handoff / zero-copy texture share          │  │
│  │  mode analysis → geometry/shader params (event-driven)         │  │
│  │  shader-chain / librashader-wgpu runtime (CRT presets)         │  │
│  │  geometry stage (aspect, integer scale) → wgpu present         │  │
│  │  winit events ────────────────────────┘   egui overlay         │  │
│  │  QMP-JSON over socketpair (snapshots, media, status)           │  │
│  └────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘

┌────────────── launcher process (Rust / Qt) ──────────────────────────┐
│  launcher-core (Rust engine): machine library, bundle manager,        │
│  disc shelf, snapshot engine, process supervisor. Spawns player.     │
│  Frontends: launcher-qt (Qt/QML via launcher-capi) and launcher CLI  │
└──────────────────────────────────────────────────────────────────────┘
```

- **Machine bundle** = directory with `machine.toml`, disk images, disc
  shelf references. The launcher creates/edits bundles; the player runs one.
  Power users can hand-write bundles and skip the launcher.
- One VM per player process; the launcher isolates the library UI from a
  crashed guest and allows several machines at once.

## The embed boundary: `libqemu_embed.h`

Small stable C API on the QEMU fork, written upstream-style:

- lifecycle: create/configure (from machine bundle), run, pause, reset,
  shutdown;
- display: register listener → surface + dirty rects callback (2D); a
  GPU-texture handoff for 3D output (see docs 03/04);
- input: keyboard scancodes, relative + absolute pointer injection;
- audio: caller-owned SPSC ring installed before init (`embed` audiodev);
- display refresh pull interval;
- media: disc mount/eject (drives libdisc), floppy;
- control: QMP-JSON channel over socketpair for everything else.

Rust bindings are hand-written in the `qemu-embed` crate (the API is ours
and small; `qemu_embed_api_version()` guards drift — no libclang build
dependency). The player never reaches past this header.

## Language policy (ADR-004 summary)

Rust: player, companion launcher (`launcher-core`, `launcher-capi`, `launcher`),
**libdisc** (CD disc model + format parsers, built as a staticlib with a C API
consumed by the QEMU ATAPI device), `shader-chain`, MMC exerciser, tooling.
C/C++: patches inside QEMU (embed API, ATAPI glue, TCG fast paths), `d3dpt` host
executor, `glidept` OpenGLide bridge, `launcher-qt` frontend. Era C for
guest-side drivers (`d3dpt-vga` Win9x/XP miniports/DDIs).

## Graphics stack

wgpu (Metal on macOS, Vulkan/D3D12 on Linux/Windows) with **librashader's
wgpu runtime** for the RetroArch-format slang shader ecosystem, encapsulated
in the `shader-chain` crate. Versions are coupled: librashader pins a wgpu
major (0.12 → wgpu 30); the workspace follows librashader's pin, not the
newest wgpu. Geometry stage updates are strictly event-driven via
`Gpu::guest_surface_changed` and `Gpu::resize`.

## Threading model

- **QEMU main loop + vCPU threads** — QEMU-managed (MTTCG on ARM hosts).
- **Render thread** — wgpu, librashader, present. The display listener
  (QEMU main loop) only publishes "surface updated + dirty rect" into a
  triple-buffered handoff (or shared texture handle for 3D); the render thread
  uploads and draws. QEMU never blocks on vsync; a slow host frame repeats the
  last guest frame.
- **Audio thread** — real-time; pulls from a lock-free ring fed by QEMU's
  audio backend (~20–40 ms initially, tune down).
- **Event thread** — winit; forwards input to QEMU without waiting.

Rule: no QEMU-owned thread waits on the GPU; no render/audio thread takes a
QEMU lock.

## Acceleration per platform

| Host | Win98/XP (x86 guests) | Notes |
|---|---|---|
| Linux x86_64 | KVM | performance reference |
| Windows x86_64 | WHPX (TCG fallback) | WHPX enabled with WHPX-aware bundle configs |
| macOS Apple Silicon | TCG (MTTCG) | Win98 easy; XP at 104% of 1.7 GHz P4 with TCG fast paths |
| macOS/Linux x86_64 | HVF / KVM | secondary, supported |

macOS JIT for TCG needs the `com.apple.security.cs.allow-jit` entitlement on
the player bundle — copy UTM's / qemu-3dfx-macos's approach.

## Repo layout

```
/player/              Rust: running-machine window (wgpu, librashader, embed bindings)
/launcher/            Rust: companion CLI launcher binary
/launcher-core/       Rust: engine for bundles, library, discs, snapshots, supervisor
/launcher-capi/       Rust: C ABI export of launcher-core for non-Rust frontends
/launcher-qt/         Qt/QML companion launcher GUI frontend (ADR-015)
/d3dpt/               Paravirtual Direct3D protocol, hw device, and host DXVK executor
/glidept/             Paravirtual Glide protocol and OpenGLide host wrapper
/cdshelf/             CD-ROM disc shelf protocol and structures
/libdisc/             Rust: CD disc model, format parsers, C API, exerciser
/shader-chain/        Rust: librashader-wgpu CRT shader pipeline abstraction
/packaging/           Packaging assets: macOS (.app/.dmg), Windows, Linux, Flatpak
/qemu/                submodule: upstream QEMU pinned to v9.2.4 (slimmed to 93 libs)
/third_party/qemu-3dfx  submodule: 3dfx/Mesa pass-through overlay + patch + wrappers
/patches/qemu/        our patch queue (embed API, ATAPI device, TCG fast paths, slimming)
/scripts/             prepare-qemu.sh, configure-qemu.sh, build helpers
/guest-tools/         driver ISO build (Rust tooling + Win98/XP display drivers)
/shaders/             curated slang presets
/reference/           rig captures: CRT photos, ATAPI traces, benchmarks
/docs/                design documents and tracks
```

## Open questions (status)

- **Host 3D texture sharing**: Resolved via Spike A (IOSurface on macOS,
  dma-buf on Linux, DXGI shared handles on Windows) with zero-copy texture
  import into wgpu.
- **In-proc QMP**: Resolved via `socketpair` (bidirectional in-memory pipe)
  reusing standard QMP JSON parsing without IPC overhead.
- **QEMU version cadence**: Pinned to QEMU 9.2.x series, slimmed to 93 shared
  libraries with unused subsystems stripped.
