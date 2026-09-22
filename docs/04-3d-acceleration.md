# 4. 3D acceleration: qemu-3dfx and guest drivers

## Strategy

We do not write a GPU emulator. Guest 3D comes from **API pass-through and
paravirtual devices**: guest-side wrapper libraries intercept Glide/OpenGL
calls and forward them to the host GPU via QEMU devices (qemu-3dfx and
OpenGLide), while Direct3D 5–9 is handled directly by our native paravirtual
display adapter (`d3dpt-vga`, doc 14) backed by a host DXVK executor.
WineD3D-based wrappers serve as a fallback stack on hosts below Vulkan 1.3
(ADR-013). That dual-path approach covers both Win98 and XP with host
acceleration on all our platforms, including Apple Silicon.

## The pieces

| Layer | Win98 | Windows XP |
|---|---|---|
| Glide (2x/3x) | qemu-3dfx stubs → OpenGLide / host GL translation | same (fewer titles care) |
| OpenGL | qemu-3dfx MESA GL pass-through | same |
| Direct3D 5–7 | `d3dpt-vga` D3D5–7 DDI (M7/M10); the same on Wine on the host below Vulkan 1.3 (ADR-018, M15; WineD3D-in-guest until M15 lands) | `d3dpt-vga` D3D5–7 DDI (M7); the same on Wine on the host below Vulkan 1.3 (ADR-018) |
| Direct3D 8/9 | `d3dpt-vga` paravirtual D3D (doc 14, ADR-006); the executor on Wine on the host below Vulkan 1.3 (ADR-018, M15; WineD3D-in-guest until then) | `d3dpt-vga` paravirtual D3D (doc 14, ADR-006); the executor on Wine on the host below Vulkan 1.3 (ADR-018, M15; WineD3D-in-guest until then) |
| 2D/desktop | `d3dpt-vga` display driver (PnP INF install, M10); SoftGPU fallback | `d3dpt-vga` display driver; Cirrus GD5446 inbox driver (`-vga cirrus`) fallback |

Known qemu-3dfx / 3D constraints we design around:

- **Version coupling:** the guest wrappers and the patched QEMU must be built
  from matching commits (the project enforces a commit-hash signature). Our
  guest-tools ISO (P2) is therefore *generated per build* of our QEMU fork,
  never a random downloaded binary.
- **QEMU version coupling:** the 3dfx patches track specific QEMU releases;
  our fork pins whatever upstream version the patches support (v9.2.4).
- **Host GL on macOS:** pass-through lands on Apple's OpenGL framework
  (GL 4.1 core / 2.1 compat). Long-term risk. Escape hatches if Apple ever
  removes GL: ANGLE (GL ES on Metal) or a Zink-style layer.
- **Windowing:** 3dfx-era games love exclusive fullscreen and mode changes;
  handled by event-driven geometry changes and CRT shader resets.
- **Output interop (ADR-005):** guest 3D reaches wgpu textures without CPU
  readback via zero-copy surface sharing (IOSurface on macOS, dma-buf on
  Linux, DXGI shared handles on Windows) validated in Spike A.

## Fallbacks and alternatives (documented, not primary)

- **SoftGPU software rendering** (llvmpipe-style in guest): always works, no
  host GPU needed; on Apple Silicon under TCG it is slow — acceptable for 2D
  desktop + light 3D only.
- **86Box** for titles that demand a *real* emulated Voodoo (early Glide
  titles with driver-level tricks): out of scope for us; we document the
  recommendation.
- **WineD3D fallback** — *being retired* (ADR-018, 2026-09-22): a host
  below Vulkan 1.3 gets the paravirtual device with its executor on
  Wine's d3d9 *on the host* (`docs/tracks/m15-wine-executor.md`), and the
  guest-side set below goes in that track's last step, once the host
  path has drawn the reference scene. Until then: Wine 1.7.55 fork
  (wine9x) built for guests when the host lacks Vulkan 1.3.
  A host with Vulkan tests this row with **`-global d3dpt-vga.no-exec=on`**
  (doc 15): the adapter then reports no executor, exactly as a below-floor
  host's does, so the guest driver offers no Direct3D and the machine has
  to carry on the way one of those users' machines does.
  `tools/xp-wined3d-test.sh` takes the whole host's Vulkan away instead
  (`VK_DRIVER_FILES=/nonexistent.json`), which is what to use when the
  launcher's own probe and its wizard note are part of the question.

## Guest tools ISO (P2)

A single unified ISO (`2ksbox-guest-tools.iso`), built by `/guest-tools/` scripts,
organized strictly by role (`GLIDE\`, `DRIVER\`, `D3DPT\`, `OPENGL\`, `WINED3D\`,
`TESTS\`, `CDSHELF\`) with no duplicate files but WineD3D's: its per-game folders
(`WINED3D\D3D8-9\`, `WINED3D\DDRAW\`) are copied whole from Explorer, so each
carries `WINED3D.DLL` and `OPENGL32.DLL` and every DLL is under the name a game
loads (2026-09-12, user request; the same files serve 98 and XP):

- **Win98:** `d3dpt-vga` display driver (PnP INF install), SoftGPU release,
  qemu-3dfx wrappers, AC97 audio, network, CDSHELF.
- **XP:** `d3dpt-vga` display driver, qemu-3dfx wrappers + WineD3D fallback set,
  AC97/HDA audio, network, CDSHELF.
- **Installer:** `SETUP.EXE` for scriptable installation (`SETUP /ALL`,
  `SETUP /GAME`).
- A tiny in-guest `verify.exe`/batch that reports which APIs are accelerated
  (renderer strings, triangle smoke test) — our acceptance test hook.

## Acceptance matrix (M3/M4 exit criteria)

A small fixed game/benchmark set per API, run on all three host platforms:

- Win98: Quake 2 (GL), Unreal (Glide), Forsaken or Incoming (D3D6),
  3DMark99/2001SE.
- XP: Quake 3 (GL), Max Payne (D3D8), Half-Life 2 or GTA:VC (D3D9 — stretch),
  3DMark2001SE/03.

Pass = renders correctly at playable framerate with acceleration verifiably
active (renderer string ≠ software).
