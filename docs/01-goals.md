# 1. Goals and non-goals

## Vision

A Windows 98 or Windows XP machine that feels like sitting at the real thing
around 1998–2005: games install from your own disc dumps (copy protection and
all), Direct3D and Glide titles run accelerated, and the picture on screen
looks like a shadow-mask CRT fed by a VGA card — not a blurry stretched
rectangle in a window.

## Goals

1. **Cross-platform, open source.** Linux, Windows, macOS. Apple Silicon is a
   hard requirement and a first-class target, not a port. Everything in the
   stack must be open source (this ruled out VMware; VirtualBox was ruled out
   on capability — no 3D for pre-Win7 guests since 6.1).
2. **Real guest 3D.** Direct3D (via paravirtual D3D device and native DXVK executor, as well as WineD3D wrappers), Glide (via OpenGLide host wrapper), and OpenGL working in the guest with host-GPU acceleration, for both Win98 and XP.
3. **Pixel-accurate, period-accurate video.** The raw guest framebuffer is
   captured before any scaling, presented with correct aspect (including
   non-square-pixel modes like 320×200), integer/sharp scaling, and a
   high-quality CRT shader chain (libretro-format "slang" shaders via
   librashader on wgpu).
4. **Faithful CD-ROM drive emulation.** Raw dump formats (cue/bin, CCD, MDS,
   CHD, ISO) mount as a virtual drive that behaves like period hardware: CD-DA
   audio tracks, subchannel data, C2 error behavior, raw TOC, plus mounting host
   directories on the fly (`isodir:`). Era copy protection running inside the
   guest passes its checks because the drive is faithful — nothing is patched or
   bypassed.
5. **Low latency.** This is for games. QEMU runs in-process with the frontend;
   the display, input, and audio paths are designed for minimal added latency
   (see doc 03).
6. **UTM-style UX.** A machine library, guided machine creation for four
   guest families (Win98, XP, DOS, and Other), sane defaults, one-click
   driver/tools media. Nobody should need to hand-write a 40-flag QEMU command
   line. Delivered as a standalone player (one machine per window) plus a
   companion launcher (`launcher-core`, shipped `launcher-qt`, maintained
   `launcher`, and `launcher-capi`) — see docs 02/07.

## Non-goals

- **Cycle-accurate vintage hardware emulation.** That is 86Box/PCem, and they
  do it well. We target "fast machine of the era" behavior, not
  chip-accurate timing (though DOS machines provide calibrated instruction throttling).
- **Modern OS guests.** We focus on retro systems: Win9x/ME, Win2k/XP, DOS, and
  period alternative OSes (BeOS, period Linux, OS/2). Modern Windows and modern
  Linux distros are out of scope.
- **General-purpose VM manager.** We are not competing with virt-manager or
  UTM for modern guests.
- **Bypassing or stripping DRM.** The CD pillar makes protected originals
  *work* by being faithful; it is a preservation-grade drive emulation, not a
  crack. No-CD patches, key generators, activation workarounds are out of
  scope. Users supply their own install media, licenses, and dumps.
- **Networking-era features** (shared folders beyond basics, clipboard
  integration, etc.) — nice-to-haves later, not pillars.

## The core pillars

| # | Pillar | Scope | Novelty |
|---|---|---|---|
| P1 | QEMU fork + CPU fast paths | Streamlined QEMU (93 libs) with custom TCG inline fast paths (x87, SSE, SIMD, REP string, SMC same-value filter) | Integration & Optimization |
| P2 | Guest display drivers | Native `d3dpt-vga` drivers: XP miniport + display driver with DirectDraw/Direct3D DX8 DDI; Win98 mini-VDD + 16-bit DIB engine driver | **Original work** |
| P3 | Paravirtual Direct3D device | SysBus `d3dpt` device, protocol, guest DLLs, and DXVK native host executor | **Original work** |
| P4 | Player display pipeline | In-process embed + pixel-accurate CRT-shaded display pipeline (wgpu + librashader), mode analysis, event-driven geometry | **Original work** |
| P5 | Companion launcher | `launcher-core` library, shipped `launcher-qt` (Qt 6 / QML via cxx-qt), egui view, C ABI, machine wizard, disc shelf | **Original work** |
| P6 | Raw CD-ROM backend ("libdisc") | Raw optical disc model (cue/bin, CCD, MDS, ISO), ATAPI device patches, CD-DA, disc shelf, and `isodir:` directory discs | **Original work** |

P2, P3, P4, P5, and P6 are the primary contributions that don't exist anywhere
today and are designed to be upstreamable / reusable by the wider retro-VM community.
