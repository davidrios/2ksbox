# Track: M7 — the XP display driver (doc 15, ADR-008)

The track that gave XP a real display driver for our `d3dpt-vga`
adapter: the QEMU device, the miniport / display DLL pair, their
DirectDraw and Direct3D DDIs, the executor's half that runs the
driver's records, and the tests. The design is doc 15; the Win98
driver that shares its core is doc 19 and the M10 track; the protocol
and the executor are doc 14. Read `docs/00-status.md` first for the
global picture and the track rules.

## State

M7 is **done** (roadmap `docs/08`), and the driver keeps evolving: each
new DirectX 8 feature is a protocol bump made here. Today:

- **M7a** — the framebuffer driver: the host's mode table in Display
  Properties, the desktop straight from VRAM, unattended install
  (`DRIVER\DRVINST.EXE`), 8 bpp palettized modes, the hardware cursor,
  gamma ramps. Register set **v5** (`D3DPT_FB_VERSION`); a driver accepts
  any adapter at or above its own version since 2026-09-12.
- **M7b** — DirectDraw: VRAM surfaces, page flips paced by a vertical
  blank, colour keys.
- **M7c** — Direct3D from DirectX 3 execute buffers through the DirectX 8
  DDI: hardware T&L, vs/ps 1.x, palettized and compressed textures,
  video-memory vertex / index buffers, sixteen streams, cube and volume
  textures, anisotropic filtering, full-screen multisampling. Protocol
  **v13** (`D3DPT_PROTO_VERSION`).
- Real titles on it: FIFA 2000, Max Payne, Diablo, Moto Racer 1997,
  GTA 2, GTA Vice City (played by hand by the user); the user's daily
  XP runs on it with no custom DLL anywhere. The keyboard shim
  `D3DPT\DINPUT.DLL` is TCG-only medicine for FIFA 2000, per game by
  decision (doc 15 "FIFA 2000 on the HAL").
- The same `core/` runs Win98's driver (M10), and on a host below the
  Vulkan floor the executor runs under Wine (M15, `EXEC=wine`) or on
  Windows' own d3d9 (`d3d9=system`); with `no-exec=on` the driver keeps
  DirectDraw and offers no Direct3D (doc 15).

## Scope and files

Owned by this track:

- QEMU device: `d3dpt/hw/d3dpt_vga.c`, register set `d3dpt/d3dpt_fb.h`
  (a bump only ever **adds** registers; a change that reinterprets one
  is a new `D3DPT_FB_MAGIC` — doc 15 "Newer register sets are
  accepted"), the executor loader `d3dpt/hw/d3dpt_exec_load.[ch]`.
- Guest driver, `guest-tools/src/d3dptvid/`: the NT layer `nt/`
  (miniport `d3dptvid.c`, display DLL `d3dptdisp.c`, INF, `.def`) over
  the OS-independent `core/` (DP2 walker, surface table, caps, flip
  chain; shared with M10, doc 19 §19), `kcrt.c`, `drvinst.c`,
  `setmode.c`, the vendored `ddk/`, and the test programs beside them
  (`ddtest`, `d3d7test`, `ditest`, `dxttest`, `shtest`, `cktest`,
  `ebtest`, `gammatest`, `zfilltest`, and the DX8 probes over
  `d3d8probe.h`). Built by `guest-tools/build-driver.sh`, which
  `build-wrappers.sh` runs to stage `DRIVER\` on the ISO.
- Executor, the driver's half: `d3dpt/exec/d3dpt_exec_ddi.cpp` (VRAM
  surfaces, contexts, the DP2 interpreter, readback) and
  `d3dpt_exec_int.h`; `tools/d3dpt-dp2-test.cpp`.
- Tests: `tools/xp-driver-test.sh`, `tools/xp-fifa-match.sh` +
  `xp-fifa2000.bat`, `xp-diablo.sh`, `xp-motoracer.sh` +
  `motoracer-state.py`, `xp-maxpayne.bat`, `xp-vicecity.sh`.

Shared (rebase first, edit minimally, name the other track in the
commit): `d3dpt/d3dpt_proto.h`, `d3dpt/exec/d3dpt_exec.cpp` /
`d3dpt_exec.h` and `d3dpt/hw/d3dpt_mm.c` (M4), `core/` (M10), the
remote executor files (M15), `scripts/test.sh`, `player/`.

## Where the design lives

Doc 15 is the specification; its sections by topic:

| Topic | Doc 15 section |
|---|---|
| adapter, miniport, mode table | "Shape (M7a)", "Building kernel-mode PE files with GCC" |
| DirectDraw HAL, dxg's caps rules | "M7b — the DirectDraw DDI", "Blit caps and the HEL" |
| flip pacing; a flip swaps roles, not memory | "The flip chain's vertical blank", "A DirectX 6 title's flip chain" |
| 8 bpp, palettes, colour keys | "8 bpp palettized modes", "Palettized textures and colour keying" |
| GDI writes into a Direct3D target | "Untracked writes" |
| cursor, gamma | "The hardware cursor", "Gamma ramps" |
| DX3 execute buffers | "Execute buffers — the DirectX 3 path" |
| DX7 HAL, DX8 DDI, d3d8.dll's findings | "M7c — the Direct3D DDI", "M7c — the DirectX 8 DDI" |
| shaders, VRAM buffers, streams, cubes, volumes, formats, MSAA | their own subsections under the DX8 DDI |
| one probe per DX8 feature | "The DX8 feature probes" |

## Build and test loop

```sh
scripts/build.sh                         # QEMU, the executor, both driver ISOs
guest-tools/build-driver.sh              # just the driver: guest-tools/out/d3dpt-driver.iso
scripts/build-d3dpt-exec.sh              # after touching d3dpt/exec/
build/d3dpt-dp2-test x.bmp               # the records through the executor, no guest (the d3dpt-dp2 check)
```

A protocol bump needs QEMU, the executor and the guest-tools ISO rebuilt
before any guest run (`scripts/build.sh` does all three); one side alone
is `protocol mismatch` in the QEMU log and no Direct3D. XP loads the
driver from `system32`, not the ISO, so **reinstall after every driver
build**:

```sh
tools/xp-driver-test.sh <image> install     # DRVINST from the ISO, restart, desktop on the driver
tools/xp-driver-test.sh <image> ddtest      # DirectDraw at 8/16/32 bpp + windowed
tools/xp-driver-test.sh <image> d3d7        # the DX7 HAL; frame must equal d3dpt-dp2-test's
tools/xp-driver-test.sh <image> d3dgame8    # M4's scene through XP's d3d8.dll; diffed against the native oracle
tools/xp-driver-test.sh <image> probes      # every DX8 feature probe in one boot
OUT=build/xd/sh tools/xp-driver-test.sh <image> shtest   # also cktest, ebtest, gamma, modes
tools/xp-driver-test.sh <image> cmd 'D:\DRIVER\DDTEST.EXE 800 600 32 300'
```

Keep `OUT=` short (the QMP socket path limit). Useful knobs:
`DDFLAGS=`, `EXEC=wine`, `NO_EXEC=1`, `VGA=cirrus` (the inbox-driver
control), `GAME_ISO=`, `SHOTS=`, `SHOT_KEYS=`, `CPU=pentium3`,
`DRIVER_ISO=` (another build's driver), `KEEP=1`. Every tool and its
options: `docs/testing.md`.

The game loops, each a script (details in `docs/testing.md`):
`xp-fifa-match.sh kvm|tcg`, `xp-diablo.sh install|play`,
`xp-motoracer.sh install|play`, `xp-vicecity.sh play`, and
`GAME_ISO=… tools/xp-driver-test.sh <image> bat tools/xp-maxpayne.bat`
(or `xp-fifa2000.bat`).

**Images.** Run on an overlay or copy of an XP image with the driver
installed, never the user's own: `~/vms/winxp-m7.qcow2` is theirs.
Sessions have kept scratch copies (`winxp-m7g` with Diablo and the
game installs) — make one with `install` if none is at hand.
`~/vms/winxp.qcow2` is M4's cirrus image.

## Diagnostics

- The driver's debug output is the DEBUG register → the QEMU log
  (`d3dpt-vga: guest: …`). No WinDbg, no serial KD.
- `d3dpt-vga: ddi: …` lines are the executor's (unsupported states and
  tokens, once each); `batch N: error` a refused record;
  `d3dptdisp: dp2 0x…` a DrawPrimitives2 the host failed;
  `dx8 draws skipped … why …` the driver's (bits: 1 shader, 2 no FVF,
  4 no stream, 8 stride < FVF, 16 vertex range, 32 index range, 64
  primitive); `N page flips in 5.0 s` a title's real frame rate
  (none: it blits to the primary); `N untracked guest pixels` GDI writes
  caught by the target shadow.
- `D3DPT_DP2_TRACE=<flag file>` dumps one whole frame per `touch`:
  states, tokens, each draw's vertices, texture levels and the render
  target after every draw as `.ppm`. `D3DPT_DDI_REREAD=1` and
  `D3DPT_DDI_NOFOG=1` rule out a stale host texture and fog.
- `-device d3dpt-vga,ddflags=N` withdraws one feature at a time for an
  A/B without a reinstall. The bits are `DDF_*` in
  `core/d3dpt_core.h` (the NT driver reads the low half, the 9x one
  `D9F_*`); the ones used most: `0x20` no Direct3D, `0x1000` no T&L,
  `0x2000` a DX7 driver to d3d8.dll, `0x8000` no vertical blank, and one
  per protocol feature from v9 on.
- `ddraw.dll` and `dxg.sys` can be pulled out of an image (`qemu-img
  convert` + `7z x`) and disassembled with `i686-w64-mingw32-objdump`
  when the DDK documentation runs out — most of doc 15's rules came from
  there.

## Traps

The rules themselves are in doc 15 and CLAUDE.md; these are the ones a
session meets first.

- Kernel mode with mingw-w64: miniport headers are `ntdef.h` +
  `ddk/miniport.h`, never `ntddk.h`; GCC emits `memcpy`/`memset` calls
  even freestanding (`kcrt.c`); `build-driver.sh` checks the import
  lists.
- dxg drops the whole HAL, silently, to `DDCAPS_NOHARDWARE` for
  `DDCAPS_GDI`, palette caps, colour-key caps without a Blt callback, or
  a mode without Direct3D. A device with `DRAWPRIMITIVES2EX` must answer
  `GUID_Miscellaneous2Callbacks` with `GetDriverState`.
- Never re-register the flip chain in `DdFlip`: on NT a flip exchanges
  the surfaces' roles, not their memory.
- The executor must never let DXVK throw or assert: its exceptions
  abort QEMU through DXVK's own unwinder, and its shader compiler
  asserts on an unknown opcode. Validate every index, count and shader
  (`sm1_valid`) before the call.
- Never claim `D3DPMISCCAPS_CLIPTLVERTS`; the runtime's clipped fans
  are stream-0 draws into its own clip buffer.
- XP SP3's driver-signing Logo dialog ignores every policy; DRVINST
  presses it, one dialog per unsigned file, for as long as the install
  runs.
- Keys typed while a full-screen DirectDraw window is up are lost, and
  the Run dialog truncates long lines silently: chain a test into one
  short `cmd` line or stage `E:\RUN.BAT` (`bat`).
- On macOS a standalone `qemu-system-i386` with no Vulkan loader on
  `DYLD_LIBRARY_PATH` segfaults in DXVK at the guest's first Direct3D
  request, which reads as "the guest never reached its shell";
  `xp-driver-test.sh` sets the environment itself, as `scripts/test.sh`
  does.

## Next steps

1. **An XP whose driver is refused shows black** on the adapter: the VGA
   core in a chained 256-colour 800×600 mode (`sr4=0a gr5=50`) that
   renders nothing, while BIOS text and mode 13h render. It blinds every
   headless look at a failed install (found in M15, its track doc
   step 3).
2. **A `driver` stage in `scripts/test.sh`**: boot on `d3dpt-vga` and run
   `d3d7` + `shtest` + `probes` from a snapshot, as the XP D3D stage does.
   Until then `tools/xp-driver-test.sh` is the check.
3. **Titles for the features that only probes have seen** (the table
   below).
4. Present the host frame through the player's 3D path instead of the
   per-frame readback into VRAM.
5. Small items, each when a title asks: `DrvDeriveSurface` (GDI on
   DirectDraw surfaces, an optimisation now that the target shadow
   exists), a real blitter behind `DDCAPS_BLT`, a vblank from the
   player's present, the mode table fed from the player (M2), RT/N
   patches (PATCHTST is the only probe still "not offered"), a RAM-backed
   palette page for fast palette animation, more 8 bpp titles
   (StarCraft, Age of Empires, Caesar 3).
6. A bytecode validator for SM2/3 on M4's d3d9 half — not a v1 blocker
   (user decision 2026-09-16: hostile guest input is an accepted risk).

## Games to test, by feature

Before any of them: the driver reinstalled from this build's ISO and
QEMU and the executor from the same build. The evidence is the QEMU log
first (`ddi:` lines, `dx8 draws skipped`), and each feature's probe in
`DRIVER\` (doc 15 "The DX8 feature probes") runs before its title: a
title that fails where its probe passes is the title's business.

| Feature (probe) | Title | Look for |
|---|---|---|
| streams, v10 (STRMTEST) | Unreal Tournament 2003/2004 | meshes, terrain, characters whole; `ddflags=0x200000` the A/B |
| vertex shaders 1.1 (SHTEST) | 3DMark2001 SE, Morrowind | no `vertex shader … refused` / `not valid vs 1.x` |
| pixel shaders 1.1–1.4 (SHTEST) | 3DMark2001 SE PS tests, Morrowind water | water shaded, not milk |
| point sprites (SPRTEST) | 3DMark2001 SE Point Sprites | sized particles |
| DOT3, EMBM (BUMPTEST) | 3DMark2001 SE, C&C Renegade water, Dungeon Keeper 2 | relief, not flat |
| cube maps, v11 (CUBETEST) | 3DMark2001 SE Nature | sky and trees in the water |
| anisotropic (ANISTEST) | UT2003/2004 `LevelOfAnisotropy` | sharp floors at a grazing angle |
| DX8 by hand | Max Payne, first levels | alley walls and ground whole |
| regression | Vice City, D3DGAME8, Moto Racer, FIFA 2000 | same frames and rates |

3DMark2001 SE has run its whole benchmark through the shared core on
**Win98** (doc 19 §36–§39: Nature, the Pixel Shader ocean's cube map);
on XP it has not been run.
