# 4. 3D acceleration: strategy and paths

The overview of how a guest's 3D reaches the host GPU: which path serves
which API on which Windows, what backs each one on a host with and
without Vulkan 1.3, and what counts as done. The paths themselves are
designed elsewhere: the GL / Glide context provider in doc 12, the
paravirtual Direct3D device and its executor in doc 14, the XP and 9x
display drivers in docs 15 and 19, the Voodoo 2 in doc 21. The
guest-tools ISO's layout is `guest-tools/README.md`.

## Strategy

We do not write a GPU emulator for the era's APIs. Guest 3D comes from
**API pass-through and paravirtual devices**:

- **OpenGL and Glide** leave the guest through qemu-3dfx's wrappers and
  its devices; the host draws them with its own OpenGL, Glide through our
  build of OpenGLide (doc 12 §5).
- **Direct3D 3–9 and DirectDraw** go through our own display adapter,
  `d3dpt-vga`, whose driver speaks the DDIs into a command window that a
  native host executor runs on a real Direct3D 9 (docs 14, 15, 19).
- **One chip is emulated after all**: a Voodoo 2 (`-device voodoo2`,
  86Box's code, ADR-016, doc 21), for the Glide titles the wrapper cannot
  reach — Glide 3, statically linked Glide, LFB tricks. It runs 3dfx's
  own driver at a software rasteriser's speed and sits beside the
  pass-through, not instead of it: the wrapper stays the fast path for
  what it covers, and OpenGL has no chip equivalent.

That covers Win98 and XP with host acceleration on every platform we
ship, Apple Silicon included.

## The paths

| API | Win98 | Windows XP |
|---|---|---|
| Glide 2 / 3 | qemu-3dfx wrapper → OpenGLide (Glide 2; `GLIDE2X.OVL` for DOS games); or the Voodoo 2 device with 3dfx's driver (Glide 2 and 3) | the same (few titles care) |
| OpenGL | qemu-3dfx GL pass-through (`OPENGL32.DLL`) | the same |
| DirectDraw, Direct3D 3–7 | `d3dpt-vga`'s driver (doc 19); below the Vulkan floor, see "Fallbacks" | `d3dpt-vga`'s driver (doc 15); the same |
| Direct3D 8 / 9 | the driver's DirectX 8 DDI under Windows' own runtime, or the per-game `D3DPT\` DLLs (doc 14); the same fallbacks | the same |
| 2D desktop | `d3dpt-vga`'s driver (the default adapter); `-vga cirrus` with Windows' in-box driver one pick away | the same |

Which host Direct3D 9 the executor calls (doc 14 has the design, ADRs
007, 013 and 018 the decisions):

| Host | Backend | How it is picked |
|---|---|---|
| Vulkan 1.3 (Linux, Windows, macOS 26+ over KosmicKrisp) | DXVK — the default everywhere, and the only one goldens are taken with | automatic |
| Windows below the floor | the system's own `d3d9.dll` | `D3DPT_D3D9=auto\|dxvk\|system`, `-device d3dpt-vga,d3d9=…` |
| Linux / macOS below the floor, with a Wine | the executor's Windows build in a Wine process on the host, on Wine's `d3d9` (M15) | `-global d3dpt-vga.exec=wine` |
| Linux / macOS below the floor, no Wine | none: the adapter reports no executor | `-global d3dpt-vga.no-exec=on` models it |

A software Vulkan (lavapipe) counts as available: the launcher says it
will be slow, and the box decides whether it beats the fallback. The
launcher's probe (`launcher-core/src/host_gpu.rs`, `launcher
--host-check`) answers in the same terms, and the machine form's
Direct3D row offers only what this host can run.

## Constraints we design around

- **Version coupling.** qemu-3dfx's guest wrappers and the patched QEMU
  must come from the same commit (the host checks a signature stamp), so
  the guest-tools ISO is built per build of our QEMU, never downloaded.
- **QEMU version coupling.** The 3dfx patches track specific QEMU
  releases; the fork pins the one they support (v9.2.4).
- **Host GL on macOS** is Apple's OpenGL framework (4.1 core / 2.1
  compat), a long-term risk. Escape hatches if Apple removes it: ANGLE
  (GL ES on Metal) or a Zink-style layer.
- **Windowing.** Era games love exclusive full screen and mode changes;
  the player follows them with event-driven geometry changes and CRT
  shader resets (doc 03).
- **Output interop.** Guest 3D reaches wgpu without a CPU readback: a
  dma-buf ring on Linux and an IOSurface ring on macOS (doc 12 §4). On
  Windows frames are still read back; a DXGI shared handle is a next
  step (`docs/00-status.md`).

## Fallbacks and alternatives

- **WineD3D in the guest — still shipped, being retired** (ADR-018,
  2026-09-22). Until M15's last step it is what a host below Vulkan 1.3
  without an executor runs: wine9x (Wine 1.7.55 with 9x/XP fixes) over
  the GL pass-through, from the ISO's `WINED3D\` folders (`SETUP /GAME 4`
  for Direct3D 8/9, `/GAME 5` for DirectDraw and Direct3D ≤7, and on 9x
  `/I 7`, WineD3D as the machine's DirectDraw through `D3DPRE.EXE`). M15
  replaces it with the executor on Wine on the host
  (`docs/tracks/m15-wine-executor.md`) and removes these rows, the ISO
  folder and the SETUP components in one commit, once the host path has
  drawn the reference scene and run a game — not before.
  - To test this row on a host that has Vulkan, `-global
    d3dpt-vga.no-exec=on` (doc 15) makes the adapter report no executor,
    so the guest driver offers no Direct3D and the machine carries on as
    a below-floor host's would. `tools/xp-wined3d-test.sh` takes the
    host's Vulkan away instead (`VK_DRIVER_FILES=/nonexistent.json`),
    for when the launcher's probe and its note are part of the question;
    `tools/wined3d-sys-test.sh` covers the 9x system-wide install
    (`docs/testing.md`).
- **The Cirrus adapter** with Windows' in-box driver: plain 2D, no
  acceleration, the control for any display-driver question.
- **SoftGPU** (software 3D in the guest, from the wine9x author) would
  work with no host GPU at all, slowly under TCG; it is planned for the
  ISO, not shipped.

## Acceptance matrix (M3 / M4 exit criteria)

A small fixed set per API. Pass = renders correctly at a playable rate
with acceleration verifiably active (renderer string not software;
`TESTS\GLPROBE.EXE` and `DRIVER\DDTEST.EXE` / `D3D7TEST.EXE` answer that
from inside the guest).

| Guest | Planned | Where it stands |
|---|---|---|
| Win98 GL | Quake 2 | GLQuake on the pass-through (Linux, in the player); Quake II's MiniGL on the Voodoo 2 (by hand) |
| Win98 Glide | Unreal | UT on the Voodoo 2 (by hand); Rayman 2 and Carmageddon (DOS) on OpenGLide, headless |
| Win98 D3D | Forsaken or Incoming (D3D6) | neither run; 3DMark 99 / 2001 SE, Crimson Skies, Moto Racer on the 9x driver (doc 19) |
| XP GL | Quake 3 | not run |
| XP D3D8 | Max Payne | plays on the DX8 DDI with no DLL in the game folder (doc 15) |
| XP D3D9 (stretch) | Half-Life 2 or GTA:VC | Vice City (a DirectX 8 title) plays; no D3D9 title yet |
| Benchmarks | 3DMark 99 / 2001 SE (98), 2001 SE / 03 (XP) | 99 and 2001 SE run on Win98 headless (`tools/w98-3dmark*.sh`); XP not run |

Most of this evidence is from the Linux host; "on all three host
platforms" is not yet met. The per-host state and the title work still
open (M10 step 5's Win98 matrix against the Glide / WineD3D control)
are in `docs/00-status.md` and `docs/tracks/m10-win98-driver.md`.
