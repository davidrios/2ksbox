# 1. Goals and non-goals

What 2ksbox is for, what it is not, and the pillars the work is
organised around. The architecture is doc 02, the milestones doc 08,
the decisions doc 10.

## Vision

A Windows 98 or Windows XP machine that behaves like the real thing
around 1998 to 2005. Games install from your own disc dumps, copy
protection included. Direct3D and Glide titles run accelerated. The
picture looks like a shadow-mask CRT fed by a VGA card, not a blurry
stretched rectangle in a window.

## Goals

1. **Cross-platform, open source.** Linux, Windows, macOS. Apple Silicon
   is a first-class target, not a port. Everything in the stack is open
   source, which ruled out VMware. VirtualBox was ruled out on
   capability: it has had no 3D for pre-Win7 guests since 6.1.
2. **Real guest 3D**, host-accelerated, on Win98 and XP. Direct3D goes
   through our paravirtual device and its host executor (DXVK, or the
   host's own Direct3D 9 or Wine below the Vulkan floor). Glide goes
   through the OpenGLide host wrapper or an emulated Voodoo 2. OpenGL
   goes through the qemu-3dfx pass-through.
3. **Pixel-accurate, period-accurate video.** The player captures the
   raw guest framebuffer before any scaling and presents it with the
   correct aspect (non-square modes like 320×200 included), integer
   scaling and a CRT shader chain (libretro slang shaders through
   librashader on wgpu).
4. **A faithful CD-ROM drive.** Raw dumps (cue/bin, CCD, MDS, ISO) mount
   as a drive that behaves like period hardware: CD-DA, subchannel data,
   error behaviour, raw TOC. A host folder mounts as a disc (`isodir:`).
   Era copy protection passes its checks because the drive is faithful.
   Nothing is patched or bypassed.
5. **Low latency.** This is for games. QEMU runs in-process with the
   front end, and the display, input and audio paths are built to add
   as little latency as possible (doc 03).
6. **UTM-style UX.** A machine library, guided creation for four
   families (Win98, XP, DOS, Other), sane defaults, a one-click
   guest-tools disc. Nobody should need to write a 40-flag QEMU command
   line. It ships as a player (one machine per window) and a launcher
   (doc 07).

## Non-goals

- **Cycle-accurate vintage hardware.** 86Box and PCem do that well. We
  target a fast machine of the era, not chip-accurate timing. The one
  concession is that a DOS machine's processor is throttled to a
  calibrated instruction rate (doc 06).
- **Modern guests.** Win9x/Me, 2000/XP, DOS and period alternatives
  (BeOS, a period Linux, OS/2). Not modern Windows or Linux, and not a
  general-purpose VM manager competing with virt-manager or UTM.
- **Bypassing or stripping DRM.** The CD pillar makes protected
  originals work by being faithful. No-CD patches, key generators and
  activation workarounds are out of scope. Users supply their own media,
  licences and dumps.
- **Integration features** (shared folders beyond a folder disc,
  clipboard sync). These are later nice-to-haves, not pillars (doc 07's
  out of scope).

## The pillars

| # | Pillar | Scope | Novelty |
|---|---|---|---|
| P1 | QEMU fork + CPU fast paths | a slimmed QEMU with TCG fast paths (x87, SSE, SIMD, REP strings, SMC, lookup and TLB work; docs 13, 16, 22) | integration and optimisation |
| P2 | Guest display drivers | `d3dpt-vga` drivers: XP miniport + display driver with DirectDraw and a DX8 Direct3D DDI (doc 15); Win9x mini-VDD + display driver (doc 19) | original work |
| P3 | Paravirtual Direct3D | the device, protocol, guest DLLs and host executor (doc 14) | original work |
| P4 | Player display pipeline | in-process embed, mode analysis, event-driven geometry, CRT shading on wgpu + librashader (docs 03, 11) | original work |
| P5 | Launcher | `launcher-core` with the Qt front end and a C ABI: library, form, disc shelf, snapshots, shader profiles (doc 07) | original work |
| P6 | Raw CD-ROM backend | `libdisc`: raw disc model and formats, ATAPI device, CD-DA, disc shelf, folder discs (docs 05, 17) | original work |

No existing project provides P2 to P6. They are written to be reusable
by other retro-VM projects.
