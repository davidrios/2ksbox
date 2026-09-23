# 2. Architecture: in-process QEMU, player + launcher, process model

How the pieces fit: QEMU linked into the player, the launcher beside
it, the threads, the languages and the repository. The embed API itself
is doc 11, the display pipeline doc 03, the launcher and packaging
doc 07, and the rationale for every decision doc 10.

## Decisions (locked — doc 10 has the rationale)

1. **QEMU runs in-process** with the display (ADR-002).
2. **Standalone Rust player + companion launcher**, no RetroArch/libretro
   (ADR-005 supersedes ADR-003).
3. **Rust where possible**; C only inside QEMU/qemu-3dfx and in
   guest-side code (ADR-004).

## Why in-process

Gaming latency: framebuffer, input and audio must not cross a process
boundary. Linking QEMU as a library gives zero-copy framebuffer access
(display listener → GPU texture), direct input injection and one clock
domain for pacing. The consequences are accepted: one VM per hosting
process (QEMU's global state and incomplete cleanup), a maintained QEMU
fork (needed anyway for qemu-3dfx, the CD backend and our devices), and
GPL-2.0 for everything that links it.

## Component map

```
┌───────────────────────── player process (Rust) ──────────────────────┐
│  ┌──────────── QEMU fork (C, libqemu-embed-i386) ─────────────────┐  │
│  │  TCG + our fast paths / KVM / WHPX                             │  │
│  │  qemu-3dfx: Glide → OpenGLide, OpenGL pass-through             │  │
│  │  d3dpt-vga + Direct3D executor (DXVK / system d3d9 / Wine)     │  │
│  │  voodoo2 (86Box's chip), opl3 + mpu401 → libsynth              │  │
│  │  ATAPI raw-CD device ──► libdisc (Rust staticlib, C API)       │  │
│  │  display listener ─┐   input inject ◄─┐   embed audiodev ─┐    │  │
│  └────────────────────┼──────────────────┼───────────────────┼────┘  │
│        libqemu_embed.h (hand-written `qemu-embed` bindings)  │       │
│  ┌────────────────────┼──────────────────┼───────────────────┼────┐  │
│  │  frame handoff: 2D surface copy / 3D dma-buf or IOSurface ring │  │
│  │  mode analysis → geometry/shader params (event-driven)         │  │
│  │  shader-chain: librashader on wgpu (CRT presets)               │  │
│  │  geometry (aspect, integer scale) → wgpu present               │  │
│  │  winit input, gamepads ───────────────┘   cpal audio ◄─ ring   │  │
│  │  QMP over a socketpair (events, PLAYER_QMP_EXEC)               │  │
│  └────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘
               ▲ spawns, + a -qmp unix: socket for live control
┌───────────────────────── launcher process ───────────────────────────┐
│  launcher-core (Rust): bundles, library, disc shelf, snapshots,      │
│  shader profiles, preview, every window's state machine              │
│  front ends: launcher-qt (Qt 6/QML via cxx-qt, ships as `2ksbox`),   │
│  launcher-capi (C ABI), launcherx (toolkit-free debug verbs)         │
└──────────────────────────────────────────────────────────────────────┘
```

- **A machine bundle** is a directory with `machine.toml` and usually
  its disk; discs are on a shelf shared by every machine. The launcher
  creates and edits bundles and translates one into the player's
  command line; the player runs a QEMU command line and knows nothing
  of bundles (doc 07).
- One VM per player process; the launcher keeps the library UI away
  from a crashed guest and runs several machines at once. Live control
  (snapshots, disc swaps) is the launcher speaking QMP to a second
  monitor socket it adds at spawn, so neither binary has an IPC surface
  of its own (doc 07, "How the launcher reaches a running machine").

## The embed boundary: `libqemu_embed.h`

A small C API on the QEMU fork (`embed/`, doc 11), currently **v8**:

- lifecycle: create from a plain `qemu-system` command line, run,
  destroy; VM start / pause / reset / powerdown / shutdown;
- display: a listener with surface + dirty-rect callbacks for 2D, and
  for 3D a copied frame or a zero-copy ring (dma-buf on Linux, IOSurface
  on macOS);
- input: keyboard qcodes, relative and absolute pointer, the gamepad
  (v8);
- audio: a caller-owned SPSC ring installed before init (the `embed`
  audiodev);
- the display refresh pull interval;
- `qemu_embed_socket_to_fd()` (v7), so the QMP socket crosses a CRT
  boundary on Windows.

Everything else — media, snapshots, status — is QMP. The Rust bindings
are hand-written in the `qemu-embed` crate (the API is ours and small;
`qemu_embed_api_version()` guards drift, and no libclang is needed).
The player never reaches past this header.

## Language policy (ADR-004)

- **Rust:** the player, `qemu-embed`, `launcher-core`, `launcher-capi`
  and `launcher-qt`'s bridges (cxx-qt, with QML and a few small C++
  shims), `shader-chain`, `libdisc` (CD model and parsers, a staticlib
  with a C API for QEMU's ATAPI device), `libsynth` (the music engines,
  doc 20), `gamepad`, and host-side tools.
- **C/C++:** our QEMU patches and devices (embed, `d3dpt/`, `voodoo/`,
  `hw/audio` opl3/mpu401), the Direct3D executor, the `glidept` OpenGLide
  platform layer, and the test harnesses in `tools/`.
- **Era C and assembly** for the guest side: the XP and Win9x display
  drivers, the guest DLLs, and the guest-tools programs.
- Python (uv-managed) for test drivers and helpers.

## Graphics stack

wgpu (Metal on macOS, Vulkan/D3D12 elsewhere) with **librashader's wgpu
runtime** for the RetroArch slang shader ecosystem, wrapped in the
`shader-chain` crate that the player and the launcher's preview share.
Versions are coupled: librashader pins a wgpu major (0.12 → wgpu 30),
and the workspace follows librashader's pin, not the newest wgpu.
Geometry updates are strictly event-driven (`Gpu::guest_surface_changed`,
`Gpu::resize`).

## Threading model

- **QEMU main loop + vCPU threads**, QEMU-managed (MTTCG where TCG
  runs).
- **Render thread** — wgpu, librashader, present. The display listener
  (QEMU's main loop) only publishes "surface updated + dirty rect" into
  a triple-buffered handoff, or a ring slot index for 3D; the render
  thread uploads and draws. QEMU never blocks on vsync; a slow host frame
  repeats the last guest frame.
- **Audio thread** — real time; cpal drains the ring QEMU's mixer fills.
- **Event thread** — winit; forwards input to QEMU without waiting.

Rule: no QEMU-owned thread waits on the GPU; no render or audio thread
takes a QEMU lock.

## Acceleration per platform

| Host | x86 guests | Notes |
|---|---|---|
| Linux x86_64 | KVM (TCG fallback) | the performance reference |
| Windows x86_64 | WHPX (TCG fallback) | a bundle's `accel = "kvm"` is spelled `whpx` there |
| macOS Apple Silicon | TCG (MTTCG) | Win98 easy; XP at 104 % of a 1.7 GHz P4 with the TCG fast paths |
| macOS Intel | TCG | community build only: permitted, untested (ADR-019) |

Win98 machines default to TCG even where KVM exists (doc 07). macOS's
JIT needs the `com.apple.security.cs.allow-jit` entitlement on the app
(`packaging/macos/2ksbox.entitlements`).

## Repo layout

```
player/          the player: winit window, wgpu present, audio, gamepads
qemu-embed/      hand-written Rust bindings to libqemu_embed.h
embed/           the embed library's C sources, overlaid into qemu/embed/
launcher-core/   everything the launcher decides; src/bin/launcherx.rs
launcher-qt/     the shipped launcher (Qt 6 / QML, cxx-qt), its own workspace
launcher-capi/   launcher-core as a C ABI (non-default workspace member)
shader-chain/    librashader-on-wgpu chain shared by player and launcher
libdisc/         CD-ROM model, image formats, C API, discx
libsynth/        OPL3 / General MIDI / MT-32 engines, synthx
gamepad/         the abstract gamepad (Rust) and the guest pad devices (qemu/)
d3dpt/           Direct3D protocol, the d3dpt-vga device, the executor
glidept/         OpenGLide's window-less platform layer
voodoo/          86Box's Voodoo 2 (verbatim), its shim, the QEMU device
cdshelf/         the in-guest disc shelf protocol
guest-tools/     the guest-tools ISO: drivers, guest DLLs, SETUP.EXE, tests
patches/         our patch queues: qemu, openglide, dxvk, seabios, wine9x
qemu/            submodule: QEMU v9.2.4, prepared by scripts/prepare-qemu.sh
third_party/     qemu-3dfx, dxvk, openglide, slang-shaders; khronos headers
firmware/        our VGA BIOS builds (VBE 4F09h)
soundfonts/      the shipped General MIDI bank
shaders/         our own CRT presets (the rig's monitor)
packaging/       Linux, Flatpak, macOS, Windows assets and the icon
scripts/         build, prepare, package and test entry points
tools/           test harnesses and diagnostics (docs/testing.md)
reference/       rig captures: D3D goldens, benchmarks
docs/            design documents and tracks
```

## Open questions

- **Host 3D texture sharing:** zero-copy on Linux (dma-buf imported
  into wgpu through Vulkan) and macOS (IOSurface as a Metal texture),
  doc 12 §4. Windows still takes the CPU-copied frame; a DXGI shared
  handle is open (M11).
- **In-process QMP** is settled: a `socketpair` (a loopback pair on
  Windows), standard QMP JSON, no filesystem path (doc 11).
- **QEMU cadence:** pinned to v9.2.4 plus a patch queue; QEMU is built
  with only what is used (the `no-optionals` check, CLAUDE.md). A
  rebase to a newer QEMU has not been scheduled.
