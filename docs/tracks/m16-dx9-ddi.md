# Track M16: a DirectX 9 driver, and no custom DLLs (ADR-021)

The display driver becomes a DirectX 9 driver: Microsoft's own
`d3d9.dll` reaches the host through the driver's DDI, on XP and on
Win98, with shader model 3.0. Then every per-game graphics DLL on the
guest-tools ISO goes: `D3D8.DLL`, `D3D9.DLL` and `DDRAW.DLL` from
`D3DPT\`, and finally `OPENGL32.DLL` from `OPENGL\`, replaced by an
OpenGL ICD that the display driver installs (user, 2026-09-25: "our
native directx 9 driver. no more custom dlls anywhere"). `DINPUT.DLL`,
the FIFA 2000 keyboard shim, is not graphics and stays per game (user,
same day: "leave dinput for now").

A driver component that Windows itself loads is not a custom DLL in
this sense: the display DLL, the 9x HAL (`D3DPT9HL.DLL`) and the ICD
are installed once, by the driver's INF, and no game folder or system
file changes. The rule the track ends at is **nothing beside a game,
and no Windows file replaced**.

ADR-021 is the decision. Doc 15 "The DirectX 8 DDI" is where the driver
stands today; doc 19 §25 is the same DDI on 9x; doc 14 is the protocol
and executor. Read `docs/00-status.md` first for the track rules.

## State

Opened 2026-09-25. Step 0 is built and run on today's driver; the rig's
two baselines are the part left (they are the user's runs).

- **The suites in a guest** (2026-09-25). Wine 11.0 is the pin: its
  test EXEs import only functions that XP's and Win98's own
  `kernel32`, `user32`, `gdi32` and `msvcrt` export (checked against the
  export tables of `~/vms/winxp.qcow2` and `~/vms/win98.qcow2`), so no
  older tag was needed. `guest-tools/build-winetests.sh` builds
  `d3d8_test.exe`, `d3d9_test.exe`, the runner `wtrun.exe` and the
  rig's `RUNALL.BAT` into `build/winetest/out/`; two patches
  (`patches/winetest/`) fence off Windows 10 and WoW64 code mingw cannot
  compile. `tools/xp-driver-test.sh <image> winetest` runs them;
  `tools/winetest-summary.py` compares.
- **Today's driver** (`reference/winetest/xp-driver-dx8.txt`, the DX8
  driver of `0c9d7b3` on `winxp-m7`, KVM):

  | test file | executed | failures | state |
  |---|---|---|---|
  | d3d9 stateblock | 14738 | 182 | ran |
  | d3d9 device | | 12 | crash in `test_update_volumetexture` |
  | d3d9 visual | | 149 | crash in `test_updatetexture` |
  | d3d9 d3d9ex | | | skipped (no `Direct3DCreate9Ex` on XP) |
  | d3d8 device | | 4 | crash after device creation failed |
  | d3d8 stateblock, visual | 67 | 0 | nearly all skipped: no device |

  A crash ends a file, so the counts are floors.
- **What it found on the first run:**
  1. d3d8 creates no device: the tests ask for an A8R8G8B8 back buffer
     on the X8R8G8B8 desktop, and the format list never gives A8R8G8B8
     `D3DFORMAT_OP_SAME_FORMAT_UP_TO_ALPHA_RENDERTARGET` (0x20).
  2. `UpdateTexture` crashes inside Microsoft's `d3d9.dll` on its first
     call (both d3d9 crashes; withdrawing volume textures,
     `ddflags=0x1000000`, does not move it).
  3. Cube and volume textures of a non-power-of-two size are created
     where real drivers refuse them (device.c:10178, 10194).
  4. The stateblock failures are the DX9 work itself: pixel shader
     integer and boolean constants refused (`D3DERR_INVALIDCALL`) by a
     runtime that sees ps 1.4.

Today the driver is a DirectX 8 driver (`DXVERSION` 0x802, `D3DCAPS8`,
vs 1.1 / ps 1.4). Microsoft's `d3d9.dll` accepts it and shows a game
those caps, so a DX9 title that needs shader model 2.0 either refuses to
start or picks a DX8 path. Today a DX9 game with shaders runs only on
the `D3DPT\` DLLs, which bypass the runtime. That path has its own
stubs (doc 14 "The guest DLLs"), and on 9x it reaches only the session's
first DirectDraw program (doc 19 §42).

## Why SM3 in one step

The shader bytecode passes through the driver untouched and DXVK
compiles every version up to 3.0, so the shader model the driver claims
is a set of caps fields. The work is the DX9 DDI, which 2.0 and 3.0
share. SM3 adds vertex texture fetch, instancing
(`SETSTREAMSOURCEFREQ`) and a few caps on top, and none of it changes
code written for 2.0. So the driver claims 3.0 from the first DX9
build, and a flag caps the claim at 2.0 as the A/B: a title that works
at 2.0 and breaks at 3.0 has its bug in the SM3 part. The reference
rig's GeForce 6200 (doc 09) is an SM3 card, so it answers for 3.0.

## Scope and files

- Guest driver: `guest-tools/src/d3dptvid/core/` (caps, the GDI2
  queries, the DP2 walker), `nt/` and `w9x/` where the two OSes differ,
  the DDI header `core/d3dpt_ddi.h`. `core/` and `w9x/` belong to M10
  and `nt/` to M7 (done); this track edits them for the DX9 DDI and
  names M16 in the commit message.
- Executor: `d3dpt/exec/d3dpt_exec_ddi.cpp` (the driver's half of the
  decoder), `d3dpt/d3dpt_proto.h` (one bump per wire change),
  `tools/d3dpt-dp2-test.cpp` (a section per new record, hostile ones
  included).
- The Wine conformance suites: a build script in `guest-tools/`, a
  runner on the ISO's `TESTS\`, a host-side summary in `tools/`, the
  baselines under `reference/winetest/`, a stage in `scripts/test.sh`.
- Removals (steps 7 and 8): `guest-tools/src/d3dpt/` except `dinput.c`,
  SETUP's file sets 1 and 3, the SysBus `-device d3dpt`
  (`d3dpt/hw/d3dpt_mm.c`, QEMU patch 40) once nothing uses it, the
  device mapper (`MAPPER\`, SETUP component 2) once the ICD no longer
  needs it. `FXMEMMAP.VXD` that 3dfx's own Voodoo 2 driver installs is
  3dfx's and stays.
- Docs: doc 15 (a "DirectX 9 DDI" section), doc 19 (its 9x half),
  doc 14 (the DLL sections shrink to history), `guest-tools/README.md`,
  `docs/testing.md`, this doc.

## The test loop

**Wine's Direct3D test suites are the conformance check** (doc 14
"Reference workloads and conformance"): `dlls/d3d9/tests/` (`visual`,
`device`, `stateblock`, `d3d9ex`) and `dlls/d3d8/tests/` (`visual`,
`device`, `stateblock`). `visual.c` draws and reads back pixels for
fixed function, shaders 1.x to 3.0, fog, sRGB, float targets, multiple
render targets and depth bias; `device.c` checks the API (textures,
surfaces, volumes, queries, declarations, lost devices), `stateblock.c`
the state blocks. Wine writes them to pass on real Windows drivers and marks
where real drivers differ with `broken()`. A failure on our driver is
therefore most likely a driver bug. Run in the guest, they go through
Microsoft's runtime into our DDI, so they test exactly the path this
track builds.

- **The oracle is the rig**: the same binaries on the GeForce 6200 under
  XP and under Win98 give the pass list, per test file.
- **The check** is "nothing fails here that passes on the rig": a
  baseline in `reference/winetest/` is one line per failing check, keyed
  by its source line (stable for one pinned tag), and
  `tools/winetest-summary.py --baseline` exits 1 on a key the baseline
  lacks or one failing more often.
- **The rig's run**: copy `build/winetest/out/` to the rig, run
  `RUNALL.BAT` from inside it under XP and under Win98 (DirectX 9.0c),
  bring back `C:\2KSBOX\WINETEST`, and save each with
  `tools/winetest-summary.py <dir> --save reference/winetest/rig-xp.txt`
  (`rig-98.txt`).
- **The other oracles stay**: D3DGAME9 / D3DGAME8 / D3DFEAT9 against the
  rig golden and the native DXVK frame (doc 14 P0a), now through
  Microsoft's runtime instead of our DLLs, and the DX8 probes of doc 15
  for the regression side.

## Steps

0. **The suite in a guest.** Done on 2026-09-25 except the rig's two
   runs (State, above). Wine 11.0 loads on XP and 98; no test imports
   D3DX or `d3dcompiler`. The first fixes, before step 1: the three
   driver bugs the first run found, since the crashes hide everything
   after them in a file.
1. **The DX9 face.** `GetDriverInfo2` answers `DXVERSION` 0x900, the DDI
   version query, `GETD3DCAPS9` (a `D3DCAPS9` with vs/ps 3.0, the
   `VS20Caps` / `PS20Caps` and instruction-slot fields), the format list
   with the DX9 `D3DFORMAT_OP_*` bits, the query types and the
   multisample quality levels. The DX8 path stays as it is behind a
   flag, as `DDF_NO_DX8` keeps the DX7 face. Two new flags are needed
   (the DX8 face, and 2.0 as the cap), and `ddflags` has two bits left
   (0x40000000, 0x80000000); decide here whether they take the last two
   or a second property starts. Expect `d3d9.dll` to check the answers
   the way `d3d8.dll` did (doc 15: `dwActualSize`, the HAL-info flag),
   and read its disassembly before guessing. Done when D3DGAME9 fixed
   function runs through Microsoft's `d3d9.dll` on the driver and
   matches the native frame.
2. **The DX9 tokens.** What the DP2 stream carries for a DX9 driver:
   vertex declarations (`D3DVERTEXELEMENT9`) and their create / set /
   delete, shader functions for vs/ps 2.0 and 3.0, integer and boolean
   constants, the scissor rect, `SETSTREAMSOURCE2` with its stride,
   render target and depth-stencil binds (`SETRENDERTARGET2`, several
   targets), queries (create / issue / response), the blits and colour
   fill, `GENERATEMIPSUBLEVELS`, the DX9 sampler states (sRGB among
   them). Each is a record in `d3dpt_proto.h` (bump per change), a
   decoder case in `d3dpt_exec_ddi.cpp` that validates it, and a
   `d3dpt-dp2-test` section. Done when D3DFEAT9 through Microsoft's
   runtime is byte-identical to the native DXVK run.
3. **SM3 and the DX9 formats.** Vertex texture fetch (the four vertex
   samplers, the vertex-texture format op), instancing
   (`SETSTREAMSOURCEFREQ`), float textures and targets (R16F to
   A32B32G32R32F) with the blending and filtering ops DXVK really has,
   sRGB read and write, multiple render targets, the depth-bias and
   stretch-filter caps. The 2.0-cap flag lands here.
4. **XP against the rig.** The Wine suites on XP down to the rig's
   failures, file by file. A `dx9` stage in `scripts/test.sh` runs them
   from an XP snapshot (it may be the `driver` stage M7 still owes).
   The DX8 probes and D3DGAME8 through `d3d8.dll` must not move.
5. **Win98.** The same `core/` answers the same queries through the 9x
   HAL (doc 19 §4: `GetDriverInfo2` reaches a 9x driver). DirectX 9.0c
   is Win98's last runtime and the one to test. The suites on Win98
   against the rig's Win98 run.
6. **Titles.** DX9 titles of the era, SM2 and SM3, on XP and Win98, with
   the user by hand. Each gets a row in the table below as it is tried.
7. **Retire the Direct3D DLLs.** `D3D8.DLL`, `D3D9.DLL` and `DDRAW.DLL`
   leave the ISO with their source, generators and SETUP's file set 1
   (the set numbers after it do not move). The guest checks in
   `scripts/test.sh` that ran D3DGAME9 / D3DGAME8 / D3DFEAT9 through
   the DLLs run them on `d3dpt-vga` through Microsoft's runtime. Vice
   City's 256 MB video-memory question, which `DDRAW.DLL` answered,
   gets the driver's own answer. The SysBus `-device d3dpt` (patch 40)
   has no user left and goes. `D3DPT\` keeps `DINPUT.DLL` alone; the
   folder's name is settled then.
8. **The OpenGL ICD.** qemu-3dfx's GL wrapper, today `OPENGL32.DLL`
   copied beside a game, becomes an installable client driver
   registered under `OpenGLDrivers` by both INFs, so a game loads
   Windows' own `opengl32.dll`, which loads the ICD. The ICD reaches
   `hw/mesa` through the display driver (an escape that maps the
   window) rather than the mapper. Open until then: whether it is a
   patch on qemu-3dfx's wrapper or our own ICD around its code, and
   `WRAPGL32.EXT`'s per-title extension cap in a world with no file
   beside the game. The check is `GLPROBE`, `wglgears`, Wine's
   `opengl32` tests (WGL and pixel formats), GLQuake and a Quake III
   timedemo. Then `OPENGL\`, SETUP's set 3, the mapper component and
   `FXPTL.SYS` go.

## Traps known before starting

- `ddflags` is full after step 1 (above).
- Every protocol bump makes the executor and the ISO stale silently
  (`protocol mismatch`); `scripts/build.sh` rebuilds both.
- **Never claim `D3DPMISCCAPS_CLIPTLVERTS`** (doc 15): the DX9 caps
  inherit the DX8 rule.
- The DX8 runtime's filter numbering (doc 15 "Filter numbering") is
  per interface version; `d3d9.dll`'s `dwhContext` value has to be read
  from a real context create before the rewrite can be trusted.
- The executor's `system` and Wine backends are second rasterisers
  (doc 14): goldens stay DXVK's, and both oracles run on all three
  whenever the decoder changes.

## Titles, by feature

Filled in as step 6 runs. A title gets a row when it has been tried.

| Title | API / SM | OS | Result |
|---|---|---|---|
