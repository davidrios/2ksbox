# 8. Roadmap and milestones

The milestones, what each set out to do, where it stands and what is
left. The order put the riskiest bets first (in-process embedding, 3D
output into wgpu, TCG performance on Apple Silicon), and each milestone
ended in something runnable. Each design lives in its doc, the working
state in its track doc (`docs/tracks/`), the order of work across
tracks in `docs/00-status.md` "Next steps", and the decisions in doc 10.
This doc keeps no history; the track docs and the commit log do.

Reference-rig sessions (doc 09) were inputs: benchmarks before M1, CRT
photos before M2, ATAPI traces and disc dumps before M5, real-GPU
screenshots during M3/M4.

| | Milestone | Status | Design · track |
|---|---|---|---|
| M0 | Foundation | done | doc 02 |
| M1 | Architecture validation | done | docs 02, 11 |
| M2 | Pixel accuracy and input | done, leftovers | doc 03 |
| M3 | 3D for Win98 and Glide | done, leftovers | doc 12 |
| M4 | Paravirtual Direct3D device | done | doc 14 · `m4-d3d-device.md` |
| M5 | CD-ROM backend, folder discs | done | docs 05, 17 · `m5-cdrom-backend.md`, `m5-dirdisc.md` |
| M6 | Launcher and packaging | shipped, continues | doc 07 · `m6-launcher.md` |
| M7 | XP display driver | done, evolving | doc 15 · `m7-display-driver.md` |
| M8 | x87 / SSE fast paths | done | docs 13, 16 · `m8-tcg-fastpaths.md` |
| M9 | TCG on Apple Silicon | done | doc 22 · `m9-tcg-aarch64.md` |
| M10 | Win98 display driver | active (step 5) | doc 19 · `m10-win98-driver.md` |
| M11 | Windows host | done, leftovers | `build-windows.md` · `m11-windows-host.md` |
| M12 | Music | done | doc 20 · `m12-music.md` |
| M13 | Gamepads | done | `m13-gamepads.md` |
| M14 | Voodoo 2 device | active | doc 21 · `m14-voodoo2.md` |
| M15 | Direct3D executor on Wine | active (steps 5–6) | doc 14 · `m15-wine-executor.md` |

## M0: Foundation

The repository per doc 02. QEMU pinned at v9.2.4 with the qemu-3dfx
overlay and our patch queue (`prepare-qemu.sh`, `configure-qemu.sh`,
uv-managed Python). The Rust workspace with a first `player` (winit +
wgpu, integer 4:3 viewport). Built and run on Linux x86-64 and the M1
Air, and on Windows since M11. A libretro core was built on the way and
dropped (ADR-005).

## M1: Architecture validation

QEMU in-process through `libqemu-embed-<target>` and the `qemu-embed`
crate (doc 11): display, input (keyboard, USB tablet, PS/2 grab),
audio (the `embed` audiodev into cpal), QMP over a socketpair
(`player/src/qmp.rs`) and the librashader CRT chain. FreeDOS, Win98 and
XP run in the player on Linux and the Air.

- **Latency** on the Air (Win98, crt-lottes): publish → present p50
  6–10 ms, p95 15–17 ms. That is the vsync-phase floor at 60 Hz, within
  doc 03's one-frame budget (`PLAYER_LATENCY=1`).
- **XP under TCG against the rig's P4 1.7** (`reference/benchmarks/`):
  integer 1.3–2x faster. x87 was 21 % of it at first and 104 % after
  M8's patches 05/06 (Super PI 1M 9:49 → 1:57 against the rig's 2:02).

## M2: Pixel accuracy and input

Mode analysis (`player/src/mode.rs`: display aspect from the mode
table, scanline count to the preset; `player --mode-sweep` checks it),
whole-pixel geometry, native screenshots (Ctrl+Alt+S), geometry
recomputed only on surface changes. Doc 03 has the design.

**Left:** overscan crop, the curated preset pack calibrated against the
rig's CRT photos, presets with no resolution override, the player's own
overlay controls (pause, snapshot, disc swap; doc 07).

## M3: 3D for Win98 and Glide

qemu-3dfx's GL pass-through renders into the player's window-less
context on Linux (EGL), macOS (CGL) and Windows (WGL), zero-copy through
a dma-buf ring on Linux and IOSurface on macOS (doc 12). Glide 2 goes
through our own OpenGLide build (doc 12 §5, patch 33, `glidept/`),
because qemu-3dfx ships no host Glide library. `GLIDETEST.EXE` passes in
a guest.

**Left:** a Glide title by hand (the evidence is headless: Rayman 2,
Carmageddon DOS), a macOS `glide-host` check and a Glide guest on the
Air, a Windows Glide wrapper, fence sync instead of `glFinish`.

## M4: Paravirtual Direct3D device

ADR-006/007, doc 14. Guest `d3d9.dll` / `d3d8.dll` serialize the API to
the `d3dpt` device (patch 40), and the host decoder runs it on DXVK. P0
(DXVK on KosmicKrisp and RADV), P1 transport, P2 fixed function, P3
shaders and queries, P4 D3D8 over d3d9. D3DGAME9, D3DGAME8 and D3DFEAT9
are byte-identical to native DXVK. On XP the M7 driver replaced the
per-game DLLs; they remain the Win98 path and the executor's harness.

**Left:** a D3D8/9 game by hand on the DLL path, its stubs, a decoder
thread (the track doc).

## M5: CD-ROM backend and folder discs

Docs 05 and 17. `libdisc` (Rust) models cue/bin, CCD, MDS and ISO with
EDC/ECC and Q-subchannel synthesis behind the `cdimage` block driver
(patch 50) and raw ATAPI (patch 51). CD-DA plays through the drive's
`audiodev`, and the in-guest disc shelf is patch 52 (`CDSHELF`).
SafeDisc, mixed-mode and ProtectCD dumps are verified; `discx repair` is
the negative control. M5g: `isodir:` serves a host folder as a generated
ISO 9660 + Joliet disc (doc 17 §8).

**Left:** FIFA 2002's no-match, a second SafeDisc 2 title, SecuROM,
multisession, CHD (the track doc).

## M6: Launcher and packaging

Doc 07. `launcher-core` owns every rule (ADR-014). `launcher-qt` is the
shipped front end (ADR-015; the egui one was deleted, ADR-017), and
`launcher-capi` and `launcherx` are its other callers. Done: the machine
library and form (four families, per-family defaults, the adapter,
Direct3D, sound, music and optimization pickers), the disc shelf,
snapshots, shader profiles with a live preview, clone. Packages are a
Linux tarball, a Flatpak (`org.kde.Platform` 6.10, offline), a macOS
app in two builds (ADR-019) and a Windows zip.

**Left:** an AppImage, a Windows installer, the preview as a
`QQuickRhiItem`, grid thumbnails, bundle import/export, Flathub
screenshots (the track doc's "Open").

## M7: XP display driver

ADR-008, doc 15. The `d3dpt-vga` adapter with the miniport + display
DLL pair (`guest-tools/src/d3dptvid/`), register set v5.

- **M7a** framebuffer driver: the host's mode table, 8 bpp palettes,
  hardware cursor (v4), gamma ramps (v5).
- **M7b** DirectDraw: VRAM surfaces, flips paced by a vertical blank,
  colour keys.
- **M7c** Direct3D, DX3 execute buffers through a DirectX 8 DDI:
  hardware T&L, vs/ps 1.x (protocol v7), palettized textures and colour
  keys (v8), VRAM vertex/index buffers (v9), sixteen streams (v10), cube
  (v11) and volume (v12) textures, full-screen multisampling (v13),
  anisotropic filtering, the rest of DX8's texture formats, untracked
  GDI writes caught by a target shadow.

FIFA 2000, Max Payne, Vice City, Moto Racer 1997, Diablo and GTA 2 play
on it.

**Left:** a title for each probe-only DX8 feature, more 8 bpp titles, a
driver stage in `scripts/test.sh`.

## M8: CPU fast paths in TCG

x87 on the host FPU with the stack kept as host values inside TCG
(patches 05/06 and later, doc 13), SSE inline (patch 11) and MMX/integer
SIMD inline (patch 12, doc 16).

**Left:** a Direct3D title with and without `*-fast=off`.

## M9: TCG on Apple Silicon

Profile-first work on aarch64 (`-perfmap`, patch 13): JIT write-protect
per thread, TB-invalidation and TLB fixes, REP fast path, same-value SMC
filter and soft immediates, inline jump-cache lookup, and the later TLB
and TB-list patches (`patches/qemu/README.md`). Doc 22 measures the
queue at 2.34x geomean over pristine 9.2.4 on the Air. The user closed
the optimization work on 2026-09-12. Patch 21 (`pinned-regs`, doc 18)
crashes XP and is not offered in the form.

**Left:** the Air's game tests uncapped, binary32 at PC=24 on aarch64,
the measurements doc 22 still owes.

## M10: Native Win98 display driver (active)

ADR-012, doc 19. The XP driver split into an OS-independent core
(§19) plus a 9x layer: `d3dpt9x.drv`, the mini-VDD `d3dpt9v.vxd` and the
ring-3 HAL `d3dpt9hl.dll`, built with Open Watcom. Steps 0–4 are done:
PnP install from the INF alone, the whole M7c matrix on 98 (§24–§25),
visible blue screens and power-down, and `d3dpt-vga` as a new Win98
machine's default. Step 5, real titles (§26 on), is under way: Crimson
Skies, 3DMark 99 / 2001 SE, Carmageddon in Mode X, Blood in a DOS box.

**Left:** the doc 04 Win98 title matrix against the Glide / WineD3D
control, and the track doc's next steps. WineD3D-in-guest stays the
fallback until M15's last step.

## M11: Windows host

`docs/build-windows.md`. A cross build from Linux in a container (QEMU
with clang, patch 68), a portable zip with the Qt launcher, WHPX, the
WGL backend (doc 12 "The WGL rule"), DXVK as `dxvk_d3d9.dll` and
Windows' own Direct3D 9 as the below-floor fallback (ADR-007). A native
MSYS2 build for debugging on a Windows PC. Guests run on the user's PC.

**Left:** Moto Racer's speed on the PC, live control over AF_UNIX, the
Windows-built ISO in a guest, an installer, DXGI zero-copy, a check that
boots a guest, a Windows Glide wrapper (the track doc).

## M12: Music

Doc 20. An OPL3 at 0x388 and the Sound Blaster's base and an MPU-401 at
0x330 (patch 60), on three pure-Rust engines in `libsynth/` (Nuked OPL3,
SoundFont General MIDI, a CM-32L on the user's ROMs), mixed in QEMU on
the guest's clock. QEMU's Gravis Ultrasound is offered as a card. The
sound-card and music pickers, the bank in every package and the
`midi-guest` check all landed, and with them the SB16's interrupt
(patch 25) and mixer (patch 61) fixes.

**Left:** Win98's own MIDI through our port loses instruments (capture
it, doc 20 §7.2); optionally a host MIDI port (doc 20 §8).

## M13: Gamepads

QEMU had no gameport, no gamepad HID and no joystick input class, so
each path is new: the host end (`gilrs`, the `gamepad/` crate,
`PLAYER_PAD_SCRIPT` for headless runs), a key mapping for every guest
(path C), a `usb-gamepad` HID device for Windows (path A, patch 26) and
an ISA gameport for DOS (path B, patch 27). The 9x analog stack was
dropped because the USB pad already reaches DirectInput and winmm on
Win98. `pad-guest`, `pad-guest-xp` and `pad-guest-98` guard it.

**Left:** a real controller on the key mapping; the USB pad on Win98
FE / Me.

## M14: The Voodoo 2 device (active)

ADR-016, doc 21. 86Box's Voodoo 2, vendored verbatim under `voodoo/`
behind a shim, as `-device voodoo2` beside (never instead of) the Glide
pass-through. 3dfx's own driver runs Quake II, Unreal Tournament, NFS
Porsche, FIFA 2000 and Carmageddon. The command FIFO lives in guest RAM
(`ramfifo=on`, Quake II 41 → 147.5 fps). The 8 MB board is the default.

**Left:** a second Glide game after one quits sometimes starts
glitched; a client resuming on a dead ring; the Air and Windows builds;
patches 64 and 71 upstream (the track doc).

## M15: The Direct3D executor on Wine, on the host (active)

ADR-018, doc 14 §"The executor on Wine, in another process". A Linux or
macOS host below DXVK's Vulkan 1.3 floor runs the same executor on
Wine's d3d9 in a child process. Steps 1–4 are done: the spike, the
transport, the guest on XP and Win98, the launcher's third verdict and
the packages (the macOS community build carries the Wine pair, and no
package ships a Wine).

**Left:** step 5, a real game on a below-floor host (the community app
on a real macOS 15); then step 6, WineD3D-in-guest removed in one commit
and the Flatpak's Wine decided.

## Post-v1 candidates

Recording and streaming, CRT bezel packs, VRR pacing, suspend/resume,
a host MIDI port for a real module (doc 20 §8), QEMU in its own process
(ADR-010), upstreaming (libdisc, the embed API).

## Standing risks

| Risk | State |
|---|---|
| GL → wgpu interop | Resolved: zero-copy rings on Linux and macOS (embed API v5/v6). |
| Host-side Glide | Resolved: our OpenGLide build (doc 12 §5), and the Voodoo 2 device beside it. |
| Apple's GL deprecation | Watched. ANGLE or Zink is the escape hatch. |
| XP on TCG on Apple Silicon | Addressed: integer beyond the P4 1.7, x87 at its speed (M1, M8, M9). |
| Hosts below DXVK's Vulkan floor | Addressed: Windows' own d3d9 (ADR-007), Wine on the host (ADR-018). |
| Drift from upstream QEMU | Pinned at v9.2.4, with a patch queue re-applied by `prepare-qemu.sh`. |
| QEMU dependency bloat | Addressed: unused backends disabled, 175 → 93 shared libraries (the `no-optionals` check). |
| Protection checks needing dump features | `discx` inspects, verifies and makes negative controls. |
