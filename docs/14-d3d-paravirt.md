# 14. Paravirtual Direct3D device for XP (ADR-006, 2026-09-03)

Direct3D 8/9 calls leave the guest as a command stream and are executed by
native host code. Guest-side WineD3D (doc 04 fallback, guest-tools ISO)
stays for DirectDraw / Direct3D ≤7 and as the comparison baseline.

## Why a device and not a better WineD3D

Under TCG every guest instruction costs ~10–20 host instructions. WineD3D
in the guest spends most of a frame translating D3D state into GL state
(shader generation, state tables, resource tracking) *before* anything
crosses to the host, and then crosses once per GL call. A serializer crosses
once per D3D call with almost no guest-side work, and the translation runs
natively. The same reasoning made qemu-3dfx pass GL and Glide through
instead of emulating a GPU.

## Shape

```
guest (XP / Win98)                          host (QEMU process, embed lib)
 game.exe                                   d3dpt device (hw/d3dpt/)
   └ d3d9.dll  (ours, C, LGPL parts)  ──FIFO──▶  decoder / resource mirror
   └ d3d8.dll  (d3d8to9-style over d3d9)          └ executor: DXVK d3d9 (C++ behind a C shim)
 shared memory: cmd ring + data pages             └ Vulkan → KosmicKrisp (macOS, ADR-007) / native (Linux, Windows)
 d3dpt-vga / FXPTL.SYS maps the device            present → embed_fx_frame / zero-copy ring (doc 12)
```

- **Transport:** the qemu-3dfx model — a PCI device with an MMIO doorbell
  page and a guest-physical shared area (command ring, argument data, bulk
  pages for Lock/Unlock uploads). The guest maps it through the `d3dpt-vga`
  driver (or FXPTL.SYS `\\.\MAPMEM` ioctl for legacy setup); a proper
  PnP INF driver is implemented for both Win98 (M10, doc 19) and XP (M7, doc 15).
  Batched: the guest writes commands until a sync point (Present, Lock readback,
  GetRenderTargetData, queries, device creation) and rings the doorbell once.
- **Guest `d3d9.dll`:** COM objects for IDirect3D9 / Device / Swapchain /
  the resource interfaces. Each method is either *forward* (append
  opcode + args), *shadow* (state the app reads back — GetRenderState,
  GetTransform, caps — answered from a guest-side copy so no round trip),
  or *sync* (Present, Lock/Unlock, queries). Resource contents move through
  the bulk pages: Lock returns a guest buffer, Unlock copies the dirty
  box. Shader bytecode (SM1–3) passes through untouched; DXVK consumes it.
  `CreateDevice` sets the x87 control word to PC=24 unless
  `D3DCREATE_FPU_PRESERVE`, like native (that is what QEMU's inline x87
  mode 2 is for, doc 13). Code and behaviour may come from current Wine
  (LGPL; ADR-006).
- **Guest `d3d8.dll`:** D3D8 over our d3d9, the d3d8to9 approach (BSD-2):
  interface mapping, caps translation, SM1.1 passthrough.
- **Host decoder:** lives in `libd3dpt_exec` (C++), which the C device
  model dlopens, so the protocol evolves without a QEMU rebuild and QEMU
  stays C. P1 runs it on the vCPU thread; one thread per device is the
  planned shape. Owns a mirror of
  handles → DXVK objects, validates arguments (a hostile guest must not
  crash the host), executes through DXVK's `IDirect3D9` natively. DXVK's
  d3d9 is a complete D3D9 implementation with a Windows-free build (the
  former dxvk-native, upstream since 2.0) that needs a WSI shim; we give it
  an off-screen swapchain whose backbuffer we read/blit into the existing
  frame path (IOSurface ring on macOS, dma-buf on Linux). KosmicKrisp on macOS
  provides the required Vulkan 1.3 environment (ADR-007).
- **Which D3D9 library the executor calls** (2026-09-21, ADR-007's second
  amendment): DXVK on every host, and on **Windows** the system's own
  `d3d9.dll` when DXVK cannot run — no Vulkan 1.3, or only a software
  Vulkan device, where a real card's D3D9 driver is the faster of the
  two. One executor, one decoder, one protocol; only the `IDirect3D9`
  behind it changes. `D3DPT_D3D9=auto|dxvk|system` picks it, the adapter
  carries it as `d3d9=` and the machine form has the row; `auto` is
  resolved by the launcher's Vulkan probe (`host_gpu.rs`), which is the
  only part of the system that can tell a software Vulkan device from a
  hardware one. What the system implementation refuses and DXVK takes is
  all in `Exec::native`: a device with no window, a draw outside a scene
  (the display driver's DP2 stream has none), the backbuffer read after a
  DISCARD Present, a device that can be lost, and two retries — hardware
  vertex processing, and a windowed backbuffer format that is not the
  desktop's. It is a **second rasteriser**, so the goldens stay DXVK's
  and `d3dpt-dp2-test` / `d3dpt-exec-test` are run on both backends
  whenever either changes.
- **Present:** the device presents explicitly at `Present`, once per frame,
  into `embed_fx_frame` / the zero-copy ring — none of the front-buffer
  flush heuristics the GL path needed. On the system-Direct3D-9 backend
  the frame is read back *before* the flip: `D3DSWAPEFFECT_DISCARD`
  leaves the backbuffer undefined afterwards on real hardware, while
  DXVK keeps it (and keeps the order the goldens were taken in).
- **Fallback:** the `-device d3dpt` off, the guest DLLs absent → the game
  loads Microsoft's d3d9 (software/no HAL) or WineD3D from the game folder,
  as today. Both stacks can coexist on one machine.

## Milestones (P = paravirt)

- **P0a — Reference workload, golden on the rig:** `D3DGAME9.EXE` and
  `D3DGAME8.EXE` (`guest-tools/src/d3dgame9.c`, `d3dgame8.c`, shared
  `d3dgame.h`; on the guest-tools ISO): a small deterministic game-like
  scene that exercises what era titles do — textured lit indexed cubes,
  a per-frame dynamic vertex buffer (software animation), additive alpha
  particles from DrawPrimitiveUP, a render-to-texture "monitor", DXT1 /
  565 / 8888 textures with mipmaps, fixed-function lights and materials,
  an optional SM1.1/2.0 shader path when D3DX is present, windowed and
  exclusive fullscreen with mode changes, vsync on/off, keyboard camera.
  `-frames N` runs a fixed-step deterministic sequence; `-dump N file.bmp`
  writes frame N as a BMP through GetRenderTargetData / CopyRects. It must
  run perfectly on the reference rig (P4 + GeForce 6200, doc 09) and its
  BMPs are the golden images every later layer is diffed against: WineD3D
  in the guest today, the device tomorrow. No game, no crack, no disc.
  **Done 2026-09-03:** both run flawlessly on the rig; the first golden set
  (d3dgame9 frame 300 windowed, fixed function and vs_1_1) with logs is in
  `reference/d3d/rig-2026-09-03/` (README there lists the caveats of that
  build: HUD bars are wall time, mask them; ps_1_1 refused by d3dx9_36's
  HLSL compiler so `-shader` is vs_1_1 + fixed pixel stage). Rendering is
  frozen at that build until a new golden set exists. `tools/bmpdiff.py`
  compares candidates against them.
- **P0b — Spike (decides the executor):** DXVK d3d9 native on macOS over
  MoltenVK and on Linux: clear + textured triangle + a SM2 shader, off-screen,
  read back. Measure. If MoltenVK cannot run DXVK's d3d9 for D3D9-era
  features (D3D9 needs Vulkan 1.1 + a few extensions DXVK lists), the
  executor becomes host WineD3D-over-GL or a wgpu translator; the guest side
  is unchanged either way. **Decided 2026-09-03 (ADR-007, spike C):**
  MoltenVK cannot (five hard-required features missing, two of them
  unimplementable on Metal); Mesa's KosmicKrisp on macOS 26 can, with a
  one-line DXVK patch (geometry shaders optional). DXVK is the executor
  everywhere; `third_party/dxvk` + `patches/dxvk/`. **ADR-013 (2026-09-06)**
  keeps this escape hatch open and unbuilt, and settles what a host that
  cannot reach DXVK's Vulkan 1.3 gets instead: the GL pass-through with
  WineD3D in the guest, and `launcher --host-check` to say so. The off-screen test
  (`tools/dxvk-d3d9-test.cpp`) and the native build of the reference scene
  (`tools/d3dgame9-native.cpp`, unmodified `d3dgame9.c` over a window-less
  Win32 shim, `tools/d3dgame-native/win32_headless.h`) both run to DXVK's refusal on MoltenVK today and produce BMPs once
  the Air is on macOS 26. **On Linux (RADV) both pass, 2026-09-03:** the
  fixed-function frame 300 vs the rig golden differs in 0.35 % of pixels
  beyond a channel tolerance of 8 (HUD masked), visually identical — DXVK
  draws what the GeForce 6200 drew. The vs_1_1 golden has no native
  counterpart (no d3dx9 compiler off Windows) and is not diffed. **On
  macOS 26 over KosmicKrisp (Air, 2026-09-03) both pass too** after a second
  one-line patch (`fillModeNonSolid` optional, wireframe → solid): 1095
  pixels beyond tolerance 8 vs 1089 on RADV, 16 beyond 32 on both. The
  executor draws the same frame on Metal and on AMD.
- **P1 — Transport + device:** `hw/d3dpt` in the QEMU queue (patch 40),
  guest `d3d9.dll` with `Direct3DCreate9`, adapter identifier/caps from the
  host, `CreateDevice`, `Clear`, `Present` → the D3D9TEST triangle
  (guest-tools) shows in the player. Per-call and per-frame cost measured
  against WineD3D-in-guest on the same test. **Done 2026-09-03 (Linux):**
  `d3dpt/d3dpt_proto.h` (protocol), `d3dpt/d3dpt_enc.h` (guest encoder),
  `d3dpt/hw` (QEMU device, patch 40), `d3dpt/exec` (decoder + DXVK
  executor as `libd3dpt_exec`, dlopened by the device), DXVK patch 04
  (headless WSI), `guest-tools/src/d3dpt/d3d9.c` (the DLL, ISO `D3DPT\`).
  XP D3D9TEST 640×480 windowed under TCG on the Linux host (RADV): 2840 fps on
  the device, 1100 on WineD3D-in-guest, 4300 replaying the same batches
  natively (`tools/d3dpt-exec-test.cpp`). One doorbell per frame; the
  batch executes synchronously on the vCPU thread under the BQL (the
  decoder thread of the shape above is deferred until a measurement asks
  for it). Present reads the backbuffer back through GetRenderTargetData;
  zero-copy through DXVK's Vulkan interop is P2/P3 work.
- **P2 — Resources + fixed function:** vertex/index buffers, textures
  (all D3D9-era formats incl. DXT and palettized via conversion), Lock/Unlock,
  render/texture/sampler states, transforms, lights, DrawPrimitive*/UP
  variants, render targets, depth/stencil, device reset and lost-device
  protocol. **Done 2026-09-04** (`guest-tools/src/d3dpt/d3d9_res.h`):
  D3DGAME9 on XP through the device is byte-identical to the native DXVK
  build's frame (1089 pixels from the rig golden, HUD masked), windowed and
  fullscreen, 8888 and 565. Palettized formats and the lost-device protocol
  are still open; cube/volume textures, vertex declarations, queries and
  state blocks are P3.
- **P3 — Shaders + queries:** SM1–3 vertex/pixel shaders, constants,
  occlusion/event queries, StretchRect, swap-chain variants, multi-head
  ignored. Acceptance: Max Payne (D3D8 via P4 stub), GTA:VC (D3D9), the
  doc 04 matrix. **Feature set done 2026-09-04** (protocol v3,
  `guest-tools/src/d3dpt/d3d9_p3.h`): declarations, all constant types,
  queries, state blocks (guest-side, never on the wire), cube textures,
  DEFAULT offscreen surfaces, ColorFill/StretchRect/UpdateSurface/
  UpdateTexture, clip planes. `D3DFEAT9` (hand-assembled SM1.1, no D3DX)
  is byte-identical between the XP guest and the native DXVK build.
  Acceptance titles still to run; swap-chain objects and volume textures
  open.
- **P4 — D3D8:** `d3d8.dll` over d3d9. **Done 2026-09-04:** `d3d8.c`
  includes `d3d9.c` and wraps its objects (the d3d8to9 shape in C; vtables
  generated from mingw's d3d8.h by `gen_vtbl8.py` since the two headers
  cannot coexist; its D3D8-only structs carry the headers' `pack(4)`). D3DGAME8 from XP is byte-identical to D3DGAME9. Volume
  textures, swap chains, GetFrontBuffer and ProcessVertices are stubs.
- **P5 — later:** DirectDraw/D3D7 layer over the device (or keep WineD3D
  for DX7 titles), Win98 guest (the same DLLs are 9x-compatible if built
  msvcrt / no-CRT like wine9x). The "proper driver" is now **ADR-008 / M7**:
  a real display driver (framebuffer → DirectDraw DDI → Direct3D DDI) on
  the same transport and executor; this DLL stays the 9x path.

## Reference workloads and conformance (what we test the device with)

- **Ours:** `D3DGAME9` / `D3DGAME8` (P0a) — small, deterministic,
  instrumented as we like, golden BMPs from the rig.
- **Wine's d3d8/d3d9 test suites** (`dlls/d3d9/tests/*.c`, LGPL): thousands
  of API and pixel-readback tests written to *pass on real Windows*; they
  build with mingw and run on XP. Run them on the rig for the pass list,
  then against the device: the conformance suite we don't have to write.
- **Irrlicht** (zlib) ships Direct3D 8 and 9 renderers with sample apps and
  real content (meshes, lightmaps, particles, shaders); builds with mingw,
  runs on XP. Good for "engine-shaped" traffic and easy to instrument.
- **Commercial titles** (doc 04 matrix: Max Payne, GTA:VC) stay the
  acceptance bar, last. The Quake/Duke ports are OpenGL and already
  covered by the qemu-3dfx pass-through; useful for the GL side only.

## Shipping it (2026-09-07)

The executor is two files and QEMU finds neither through a link: it
`dlopen`s `libd3dpt_exec` by a search that starts at `build/d3dpt`, and
the executor then `dlopen`s DXVK's `d3d9` the same way. A package that
carries the player and not those two has XP guests that fall back to
WineD3D with nothing said anywhere, so every packager stages **both or
neither** — the macOS app since 2026-09-06, the Linux tarball and the
Flatpak since 2026-09-07 (`lib/2ksbox/libd3dpt_exec.so` +
`libdxvk_d3d9.so.0`, the second installed under the soname the executor
looks up, since nothing links it). The Flatpak builds them in its own
sandbox against the runtime's libraries; `third_party/dxvk` is in the
copied source tree for that reason and because the executor needs its
`include/native` headers to compile. No Vulkan driver travels with the
Linux packages: the host's is the right one there, and a host below
Vulkan 1.3 keeps GL + WineD3D (ADR-013). The packaged player names both
files to QEMU through `player/src/companions.rs`, and `player
--companions` prints what that rule resolved — which is what the
packagers check, rather than restating the layout in a script. Pointing
`tools/d3dpt-exec-test` at a staged pair (`D3DPT_EXEC_LIB` /
`D3DPT_DXVK_LIB`) is the cheap proof that the files themselves work: real
batches, real frames, the hostile batch still refused.

## A review of the guest DLLs (2026-09-13)

A read of `d3d9.c`, `d3d9_res.h`, `d3d9_p3.h`, `d3d8.c` and the
`ddraw.dll` shim, each record checked against what the executor does with
it and, where the executor hands a call on, against DXVK. What was wrong:

- **UpdateTexture into a DEFAULT texture refused the batch.** For a level
  with no guest shadow (the usual SYSTEMMEM → DEFAULT case) it appended a
  `SURFACE_UPDATE` naming handle 0 ahead of the real `TEXTURE_UPDATE`. The
  host refuses a batch from an unknown handle on, so every draw queued
  behind it was dropped and the next sync call failed. It was a leftover
  of an earlier shape of the function; only the real record goes now.
- **DrawIndexedPrimitiveUP with MinVertexIndex above 0** copied the
  array's first NumVertices vertices instead of those from MinVertexIndex
  on, and the executor handed DXVK the copy as vertex 0. DXVK reads
  (MinVertexIndex + NumVertices) × stride bytes from that pointer, so the
  result was wrong vertices plus a read past the record (a hostile record:
  gigabytes past the window). The guest now copies vertices
  MinVertexIndex.. and the executor rebases the indices onto them;
  `d3dpt_proto.h` says so. The bytes of every draw that worked
  (MinVertexIndex 0) are unchanged, hence no protocol bump.
- **Buffer locks.** `Lock(offset, 0)` returned the buffer's start, not
  `offset` (0 is "to the end"; DXVK returns `data + offset`). A second
  Lock while one was held replaced the first one's range, which then never
  reached the host. Locks nest now; the last Unlock sends the union.
- **Recording a state block applied it.** Every setter between
  BeginStateBlock and EndStateBlock went to the host and stayed in force;
  native records the call and leaves the device alone. A game that records
  its blocks at load time kept the last recorded values until something
  set them again. While a block records, setters now only update the
  shadow and the block's marks, and EndStateBlock puts the shadow back from
  a snapshot taken at BeginStateBlock (object references held).
- **The `ddraw.dll` shim's QueryInterface** counted a reference on the
  wrapper and not on the real `IDirectDraw7`, while Release drops both: a
  QI'd reference released again freed the real object under the
  application.
- **GetRenderTargetData** sized the executor's rows at 4 bytes a pixel for
  every format but the three 16-bit ones, so A4R4G4B4 and the 64/128-bit
  float targets came back refused or at the wrong stride. The executor has
  a table now (the guest's agrees) and refuses a format it cannot size.
- **A DEFAULT-pool offscreen plain surface could not be locked**
  (INVALIDCALL): the surface a 2D game fills and StretchRects to the back
  buffer. It keeps a guest shadow now; the locked rectangle goes to the
  host at UnlockRect, and ColorFill (32-bit formats) and UpdateSurface keep
  the shadow current. Nothing the host renders comes back into it.
- **Clear with more than 64 rects** was refused whole; it goes as several
  records of at most 64 now.
- **D3D8 declaration constants** (`D3DVSD_CONST`) of a second block landed
  right after the first block's instead of at their own register.
- Both log helpers wrote a newline past their buffer when a line overran
  (a C99 `vsnprintf` returns the untruncated length).
- **No thread safety.** One encoder and one batch per process, and no
  lock: a `D3DCREATE_MULTITHREADED` game creating resources on a loader
  thread while drawing on another interleaved the two threads' records.
  Native D3D9 serialises every call of such a device (and of no other).
  Ours does the same from the moment one exists, process-wide because
  the encoder is: `gen_vtbl.py` / `gen_vtbl8.py` now make a wrapper for
  every method, not only the traced ones, and each takes `D3DPT_LOCK` (a
  recursive critical section, so the implementation calling back through
  a vtable is fine); the log's host copy takes it too. A device without
  the flag pays one load and a branch per call. At `DLL_PROCESS_DETACH` the
  lock is dropped, since a thread killed at process exit may hold it.

The regression cases: `D3DFEAT9` has a row E (one quad each for
UpdateTexture, MinVertexIndex, Lock(offset, 0) and nested Locks), an
80-rect Clear, a state block recorded and never applied, a lock of its
DEFAULT offscreen surface and a GetRenderTargetData from an
A16B16G16R16F target — frame and "getters" lines against the native DXVK
run, as before. Its device is `D3DCREATE_MULTITHREADED` now, and a loader
thread creates, fills and releases a texture and a vertex buffer in a loop
for as long as the frames run, counting its failed calls into a "getters
3" line; nothing it makes is drawn, so the frame stays the oracle's. The
loop is **paced to two rounds a frame** (2026-09-13): DXVK frees a released
resource only once the frames that could have used it are done, and an
unpaced loader outran the frees whenever the main thread stopped
presenting — the occlusion-query wait at the dump frame is up to half a
second of that. Natively on the M1 the run reached a 17.8 GB footprint in
five seconds (1.4 GB of it resident, the rest GPU memory) and swapped the
machine to a standstill in `scripts/test.sh host`; the round counts below
are from before the pacing. `tools/d3dgame-native/
win32_headless.h` gained `CreateThread` / `WaitForSingleObject` over
pthreads for it. `DDVMTEST` releases a QueryInterface'd reference and uses
the object after it, and `tools/d3dpt-exec-test` sends a
DrawIndexedPrimitiveUP at MinVertexIndex 0xfff000 with a 1 KiB stride.

**Measured** (2026-09-13, `scripts/test.sh all`, 45 passed): the XP
guest's `D3DFEAT9` frame is byte-identical to native DXVK's with row E in
it, and all three "getters" lines equal — after a recording fill mode 3
(`D3DFILL_SOLID`) and stage 2 empty, the offscreen lock reading back its
fill (`204080`), the A16B16G16R16F readback's bytes, and the loader thread
at 0 failed calls and 0 failed Presents after 24 906 rounds beside the 600
frames (the QEMU log says `calls are serialised from now on`, and has no
`batch error` in it). `D3DGAME9`, `D3DGAME8` and
`DDVMTEST` pass as before. `tools/d3dpt-exec-test` passes on the new
executor and **dumps core on the old one**, at the far-MinVertexIndex draw.

**The control**: the same guest stage on an ISO whose `D3D9.DLL` /
`D3D8.DLL` are the pre-review ones (everything else, the test programs
included, as shipped) fails exactly one check, `guest-F9`: `D3DFEAT9`
stops at the first sync call after its UpdateTexture — CreateVertexBuffer
returns E_FAIL — and draws no frame, and the QEMU log has the refused
batch (`batch error 3`, a bad handle, in a batch of three records).
`DDVMTEST`, `D3DGAME9` and `D3DGAME8` pass on those DLLs. (The DLLs of a
control must be built with the ISO's own flags: built with this toolchain's
default UCRT they import `api-ms-win-crt-*.dll`, which XP does not have, and
no D3D program starts at all.) That log line named the record *after* the
one that failed — the executor counted a record before checking whether it
had failed, and the guest's `ret_index` came out one too high the same way;
both name the failing record now (`tools/d3dpt-exec-test`'s hostile
DrawPrimitiveUP, alone in its batch, logged `at record 1 (op 0)` and logs
`at record 0 (op 50)`). The `DDVMTEST` case has not been run
against the pre-fix shim: a use-after-free need not crash, so it is a
smoke, not a proof.

**The lock's control** (the shipped DLLs with `D3DPT_LOCK` compiled to
nothing) first **passed** every check: the loader thread's 36 396 rounds
beside the 600 frames all succeeded and frame 300 was right. The race
happened all the same — the QEMU log had two batches refused (`batch
error 3`, a RELEASE naming a handle the host did not have, in batches of
1 409 and 625 records) and the guest's `Present: batch error` twice, two
frames' draws thrown away — but the loader counted only its own calls and
the main thread ignored what Present returned. `D3DFEAT9` counts failed
Presents into "getters 3" now (native: 0), and on the same lock-less DLLs
that fails exactly one check, `guest-F9-log=native`: `3 presents failed`
after 36 384 loader rounds, with three refused batches in the QEMU log
(`batch error 3 at record 0 (op 71)`, a TEXTURE_UPDATE of the loader's
landing in the main thread's batch). With the lock: 0 failed Presents and
no `batch error` (above). A race is a matter of chance, so this is a check
that fails when the race is hit — twice in two lock-less runs so far — not a
proof that it is absent.

Found and not changed: `GetSwapChain` a stub while
`GetNumberOfSwapChains` says 1, and D3D8's `GetVertexShader` /
`GetPixelShader` returning the handle last set rather than one an applied
state block set. The D3D8 constant fix has no case: the D3D8 path's only
oracle is D3DGAME8 against the D3D9 frame, and D3DGAME8 has no shader.

**`CheckDeviceFormat` answers the usage now (2026-09-14).** It said yes to
every format in its list whatever the usage and resource type, R8G8B8
included — which DXVK's d3d9 does not map at all ("Unsupported" in
`d3d9_format.cpp`). 3DMark2001 SE on Win98 asked for an R8G8B8
render-target texture, was told yes, got `D3DERR_INVALIDCALL` from the
`CreateTexture` behind it and quit (`P_D3D::allocateMap - CreateTexture
( for a rendertarget) failed` in its `error.log`). R8G8B8 is out of the
list; a `D3DUSAGE_RENDERTARGET` question gets yes only for A8R8G8B8,
X8R8G8B8, R5G6B5, X1R5G5B5 and A1R5G5B5 — the ones Vulkan makes every
device render to, since the guest cannot ask the host; a
`D3DUSAGE_DEPTHSTENCIL` question, and `CheckDepthStencilMatch`, only for a
depth format. Measured on a raw copy of `base98-us` with the rebuilt
`D3D8.DLL` next to the EXE (`tools/win98-game-test.sh`, `STAGE=`): the
demo plays through the guest DLLs — past 8 000 presents, all `hr 0`, five
vs 1.x shaders created, no `error.log` written — and the executor's frames
show Dragothic, the Earth and Nature as they should be.

## Risks

- **Host Vulkan capabilities:** MoltenVK lacked required Vulkan features and
  was superseded by Mesa's KosmicKrisp on macOS (ADR-007). Hosts lacking
  Vulkan 1.3 fall back to GL pass-through + WineD3D (ADR-013).
- **FIFO cost under TCG:** each MMIO doorbell is a TCG exit; batching per
  frame keeps it to a few per frame. qemu-3dfx's numbers (500+ fps wglgears
  on the Air) bound the transport.
- **Lock-heavy games** (per-frame dynamic vertex buffers): bulk pages plus
  DXVK's own upload path; measure in P2 with FIFA-style software-skinned
  titles.
- **Scope creep from DX7:** kept out; WineD3D covers it.
- **C++ in the QEMU tree** (DXVK): built as a separate static library with a
  C shim, linked into the embed library only.

## Where the FIFA 2000 investigation stopped (2026-09-03)

Parked, for the record (doc 00 has the timeline): with the wine9x
WineD3D + our two fixes the game runs a match through DirectDraw/D3D6 and
the GL pass-through; the pitch texture renders as noise bands (dynamic
surfaces mapped every frame through the PBO path, or a 16-bit/palettized
upload with the wrong stride — not resolved), the screen flickers (the
front-buffer present fires on every glFlush, not once per frame), and
DirectInput stops after the match's mode switch (foreground window). The
nine "program error: out of range indirect offset (+65)" lines on the host
are wined3d's own ARB offset-limit probe and are harmless.
