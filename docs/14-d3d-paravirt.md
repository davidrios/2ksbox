# 14. Paravirtual Direct3D device (ADR-006)

Direct3D 8/9 calls leave the guest as a command stream and run in native
host code: one protocol, one decoder, one executor over four possible
Direct3D 9 libraries. This doc covers the device, the protocol, the guest
DLLs and the executor. The XP display driver's Direct3D DDI (doc 15) and
the Win9x driver's HAL (doc 19) reuse the same protocol and executor
through a command window in `d3dpt-vga`'s VRAM. Doc 04 has the 3D
strategy as a whole; the M4 and M15 track docs have their test loops;
`docs/testing.md` has the tools and `docs/development.md` the env knobs.

WineD3D in the guest (the wine9x build on the guest-tools ISO) is the
older fallback for hosts with no executor. ADR-018 retires it in M15's
last step; until then it stays, and nothing of it is removed early.

## Why a device and not a better WineD3D

Under TCG every guest instruction costs ~10–20 host instructions. WineD3D
in the guest spends most of a frame translating D3D state into GL state
(shader generation, state tables, resource tracking) *before* anything
crosses to the host, and then crosses once per GL call. A serializer
crosses once per batch with almost no guest-side work, and the
translation runs natively. The same reasoning made qemu-3dfx pass GL and
Glide through instead of emulating a GPU.

## Shape

```
guest (XP / Win98)                      host (QEMU process)
 game.exe                               d3dpt (SysBus) or d3dpt-vga's window
   └ d3d9.dll (ours, C)  ──batch──▶       └ libd3dpt_exec (dlopened)
   └ d3d8.dll (over d3d9)                     decoder + handle mirror
 shared window: records, data, bulk           └ IDirect3D9: DXVK / system d3d9 /
 FXPTL.SYS / FXMEMMAP.VXD map it                 Wine's d3d9 in a child process
                                          present → embed presenter (doc 12)
```

- **Transport.** The qemu-3dfx model: a SysBus device (`d3dpt/hw/d3dpt_mm.c`,
  patch 40) with a 4 KiB register page at `D3DPT_MM_BASE` (0xdfffe000)
  and a 64 MiB RAM window at `D3DPT_SHM_BASE` (0xd8000000), fixed
  guest-physical addresses the guest maps through `FXPTL.SYS`'s
  `\\.\MAPMEM` on NT or `FXMEMMAP.VXD` on 9x. On `d3dpt-vga` the window is
  the top `D3DPT_SHM_SIZE` of the VRAM BAR instead (doc 15). The guest
  writes records until a sync point (Present, a Lock readback,
  GetRenderTargetData, a query, device creation) and rings
  `D3DPT_REG_DOORBELL` once. The batch executes synchronously on the vCPU
  thread under the BQL; a decoder thread is deferred until a measurement
  asks for it.
- **Protocol.** `d3dpt/d3dpt_proto.h` is the one header for guest DLL,
  QEMU device and executor (`D3DPT_PROTO_VERSION`, 13 today; bump it on
  any wire change, and rebuild the executor and the ISO, which do not say
  they are stale — the suite fails as `protocol mismatch` or a guest that
  never attaches). The guest encoder is `d3dpt/d3dpt_enc.h`. The executor
  validates every record (a hostile guest must not crash the host) and a
  refused batch reports the failing record's index (`batch error N at
  record R (op O)` in the QEMU log).
- **Guest `d3d9.dll`** (`guest-tools/src/d3dpt/`): COM objects for
  IDirect3D9, the device, swap chain and resources. Each method is
  *forward* (append a record), *shadow* (state the app reads back —
  GetRenderState, GetTransform, caps — answered from a guest copy), or
  *sync*. Resource contents move through the window: Lock returns a guest
  buffer and Unlock sends the dirty box. SM1–3 bytecode passes through
  untouched. `CreateDevice` sets the x87 control word to PC=24 unless
  `D3DCREATE_FPU_PRESERVE`, like native (doc 13's PC=24 path is for
  exactly this). Code may come from current Wine (LGPL, ADR-006).
- **Guest `d3d8.dll`:** D3D8 over our d3d9 in the d3d8to9 shape (BSD-2),
  in C: `d3d8.c` includes `d3d9.c` and wraps its objects; vtables are
  generated from mingw's `d3d8.h` by `gen_vtbl8.py`, since the two headers
  cannot coexist, and D3D8-only structs keep the headers' `pack(4)`.
- **Executor** (`d3dpt/exec/`, C++ behind the C API in `d3dpt_exec.h`):
  a mirror of handles → D3D9 objects, executed through whichever
  `IDirect3D9` was loaded (below). The QEMU device `dlopen`s it
  (`d3dpt/hw/d3dpt_exec_load.c`), so the protocol evolves without a QEMU
  rebuild and QEMU stays C.
- **Present.** The device presents at `Present`, once per frame, into the
  embed presenter (`embed/embedfx.c`) — none of the front-buffer flush
  heuristics the GL path needs. Today the frame is read back through
  GetRenderTargetData; zero-copy through DXVK's Vulkan interop is open
  (M4 track).
- **Fallback.** With no executor the device reports
  `D3DPT_STATUS_NO_EXEC` and the guest carries on: without our DLLs a game
  loads Microsoft's d3d9 or WineD3D from its folder. Both stacks can
  coexist on one machine.

## The guest DLLs

On XP the display driver's DX8 DDI (doc 15) replaced per-game DLLs; the
DLLs remain the Win98 per-game path and the executor's test harness
(`D3DGAME9`, `D3DGAME8`, `D3DFEAT9`).

- **Identity and lifetime (D3D8).** Real D3D8 keeps device- and
  texture-owned objects (surfaces, levels) alive at refcount 0 and hands
  out the *same* object each time. Each wrapper is created once per
  underlying object (`w8_new`) and owned objects follow their owner
  (`surf_Release` / `res_addref`); wrappers with no identity died with the
  game's last Release and crashed Vice City behind its window.
- **Forwarding.** When the DLL cannot open the device it loads the system
  `d3d9.dll` / `d3d8.dll` from `GetSystemDirectory` and forwards, instead
  of failing. Vice City's process loads both DLLs.
- **Threads.** One encoder and one batch per process. From the first
  `D3DCREATE_MULTITHREADED` device on, every method takes `D3DPT_LOCK` (a
  recursive critical section, process-wide because the encoder is;
  `gen_vtbl.py` / `gen_vtbl8.py` wrap every method), and the QEMU log says
  `calls are serialised from now on`. Other devices pay one load and a
  branch per call. The lock is dropped at `DLL_PROCESS_DETACH`, since a
  thread killed at exit may hold it.
- **Buffer locks nest**; the last Unlock sends the union of the ranges.
  `Lock(offset, 0)` means "to the end" and returns `data + offset`.
- **State blocks** are guest-side and never on the wire. While one
  records, setters update only the shadow and the block's marks, and
  EndStateBlock restores the shadow from a snapshot taken at
  BeginStateBlock — native records without applying.
- **DEFAULT-pool offscreen plain surfaces** keep a guest shadow so a 2D
  game can Lock them; the locked rectangle goes to the host at UnlockRect,
  ColorFill (32-bit formats) and UpdateSurface keep it current, and
  nothing the host renders comes back into it.
- **DrawIndexedPrimitiveUP** copies vertices from MinVertexIndex on and
  the executor rebases the indices (DXVK reads MinVertexIndex +
  NumVertices strides from the pointer, so a copy from 0 read past the
  record). **Clear** goes as records of at most 64 rects.
- **`CheckDeviceFormat` answers the usage.** R8G8B8 is not listed (DXVK
  does not map it; 3DMark2001 SE asked for an R8G8B8 render-target
  texture, was told yes and quit on the CreateTexture). A
  `D3DUSAGE_RENDERTARGET` question gets yes only for A8R8G8B8, X8R8G8B8,
  R5G6B5, X1R5G5B5 and A1R5G5B5 — what Vulkan makes every device render
  to, since the guest cannot ask the host; a depth-stencil question and
  `CheckDepthStencilMatch` only for depth formats.
- **GetRenderTargetData** sizes rows from a format table the guest and
  executor share (`fmt_block` / `rt_bytes_per_pixel`), and a format it
  cannot size is refused.
- **The `D3DPT\DDRAW.DLL` shim** answers Vice City's DirectDraw
  video-memory check, implements `GetRasterStatus` and the gamma ramp,
  and lists all the adapter's modes. Its QueryInterface counts a reference
  on the real `IDirectDraw7` too, since Release drops both.
- **Hand-assembled shaders.** SM1 opcode numbers are the `D3DSIO_*`
  values (`m4x4` is 20; 24 is `m3x2`). A wrong opcode compiles fine in
  DXVK and draws nothing; dump the SPIR-V with `DXVK_SHADER_DUMP_PATH`
  to see what it became.
- **Stubs log once** (`D3DPT_STUB`: `d3dpt: <name> not implemented`):
  - palettized (P8) textures (`SetPaletteEntries` /
    `SetCurrentTexturePalette`) — Vice City's menu background is grey
    noise. DXVK has no P8; the plan is the DDI path's answer, expansion to
    A8R8G8B8 on upload and a re-upload on a palette change;
  - volume textures and swap-chain objects (`GetSwapChain` fails while
    `GetNumberOfSwapChains` says 1);
  - `GetFrontBuffer` (Max Payne calls it) and `ProcessVertices`;
  - `LockRect` on render targets and depth surfaces (refused with
    INVALIDCALL), and D3D8 `CopyRects` into system memory;
  - the lost-device protocol (`TestCooperativeLevel` always succeeds).

  D3D8's `GetVertexShader` / `GetPixelShader` return the handle last set,
  not one an applied state block set.

### A review of the guest DLLs

A read of `d3d9.c`, `d3d9_res.h`, `d3d9_p3.h`, `d3d8.c` and the DDRAW shim
against the executor and DXVK (2026-09-13) produced most of the behaviour
above. Its regression cases live in `D3DFEAT9`: row E (one quad each for
UpdateTexture into a DEFAULT texture, MinVertexIndex, `Lock(offset, 0)`
and nested locks), an 80-rect Clear, a state block recorded and never
applied, a lock of a DEFAULT offscreen surface and a readback from an
A16B16G16R16F target, all compared with the native DXVK run. Its device is
`D3DCREATE_MULTITHREADED`, and a loader thread creates, fills and releases
a texture and a vertex buffer beside the frames, counting failed calls
*and failed Presents* into a "getters 3" line (native: 0). The loader is
paced to two rounds a frame: DXVK frees a released resource only after the
frames that could use it, and unpaced it reached 17.8 GB on the M1 in five
seconds. `DDVMTEST` uses an object after releasing a QueryInterface'd
reference, and `d3dpt-exec-test` sends a DrawIndexedPrimitiveUP at
MinVertexIndex 0xfff000 (the old executor dumps core there).

The controls: the pre-review DLLs fail exactly `guest-F9` (CreateVertexBuffer
returns E_FAIL after the first UpdateTexture; `batch error 3` in the log),
and the DLLs with `D3DPT_LOCK` compiled out fail `guest-F9-log=native`
with `3 presents failed` and refused batches. A race is chance, so that
check fails when the race is hit, not whenever the lock is missing; the
DDRAW case is a smoke, not a proof. The D3D8 constant fix (`D3DVSD_CONST`
of a second block at its own register) has no case: D3DGAME8 has no
shader. A control's DLLs must be built with the ISO's flags — the default
UCRT imports `api-ms-win-crt-*.dll`, which XP lacks.

## The executor and its Direct3D 9

One decoder serves four D3D9 libraries; only the `IDirect3D9` behind it
changes. Nothing backend-specific goes into the decoder.

| Backend | Where | Picked by |
|---|---|---|
| DXVK (`libdxvk_d3d9`) | in process, Linux, macOS 26+, Windows | default everywhere; the goldens are DXVK's |
| the system `d3d9.dll` | in process, Windows below the Vulkan 1.3 floor | `D3DPT_D3D9=system` (ADR-007's second amendment) |
| Wine's d3d9 | a Wine process on a Linux or macOS host below the floor | `D3DPT_EXEC=wine` (ADR-018, M15) |
| none | a host with neither | `D3DPT_EXEC=none` or no library: `D3DPT_STATUS_NO_EXEC` |

`D3DPT_D3D9=auto|dxvk|system` reaches the adapter as `d3d9=` (the machine
form's Direct3D row); `D3DPT_EXEC=auto|dxvk|wine|none` as `exec=`. `auto`
for `d3d9=` is resolved by the launcher's Vulkan probe
(`launcher-core/src/host_gpu.rs`), the only part of the system that can
tell a software Vulkan device from a hardware one. `no-exec=on` refuses
before any library is opened, so it is never a way to reach a backend.

**DXVK.** DXVK's d3d9 is a complete D3D9 with a Windows-free build (the
former dxvk-native). We give it a headless WSI (`patches/dxvk/04`, `08`
for Windows; `DXVK_WSI_DRIVER=Headless`) and an off-screen swap chain.
On macOS it runs on KosmicKrisp, which needs macOS 26 (ADR-007, spike C).
DXVK with no Vulkan loader calls through a null pointer in
`Direct3DCreate9`, and unloading a library after a failed probe crashed
too: the executor looks for `libvulkan` before it tries DXVK, and never
closes a probed library.

**Depth formats.** `depth_norm()` maps what 2001 cards offered and DXVK
refuses: D32 → D24X8, D15S1 and D24X4S4 → D24S8; the guest keeps
reporting the format asked for. Without it Max Payne's D32 auto depth
buffer failed and the game said, in 32-bit modes, that it "requires a
DirectX 8 compatible display adapter". `d3dpt-exec-test` asks for D32.

**The system Direct3D 9 (Windows).** On a Windows host that DXVK cannot
serve (no Vulkan 1.3, or only a software device) a real card's own D3D9
driver is the faster answer. Everything it refuses and DXVK accepts sits
behind `Exec::native` (`d3dpt/exec/d3dpt_exec.cpp`):

- a hidden 1×1 popup is the device window;
- `Exec::scene_begin` opens a scene itself — on both backends, since the
  DX7 DDI's DP2 stream has no BeginScene — and closes it before every
  StretchRect, readback and Present;
- the back buffer is read *before* Present: `D3DSWAPEFFECT_DISCARD`
  leaves it undefined afterwards on real hardware (that is where the
  black frames went), while DXVK keeps it;
- `TestCooperativeLevel` once per batch, `Reset` on loss, and
  `exec_ddi_device_reset` drops the DEFAULT-pool mirror so every surface
  is read from guest VRAM again;
- hardware vertex processing, and a windowed back-buffer format not the
  desktop's, are retried rather than refused;
- `D3DFMT_L6V5U5` is listed by NVIDIA's d3d9 and drawn with no
  luminance, so this backend uploads it as X8L8V8U8
  (`d3dpt/exec/d3dpt_exec_ddi.cpp`).

It is a second rasteriser, so the goldens stay DXVK's and
`d3dpt-dp2-test` / `d3dpt-exec-test` run on both backends whenever either
changes; their frames were byte-identical on the user's PC. **The one
measured difference:** the X byte of an X8R8G8B8 target reads back
`0xff` from DXVK and `0x00` from NVIDIA's d3d9 (`px0 0xff203040` against
`0x00203040`, RGB identical, D3D7TEST on Win98). The format leaves it
undefined and nothing here reads it; a title that treats X as alpha would
differ — start there if one does.

### The executor on Wine, in another process

A Linux or macOS host below DXVK's floor runs the same executor on Wine's
own d3d9 (ADR-018; state and test loop in
`docs/tracks/m15-wine-executor.md`). Wine's d3d9 exists only inside a Wine
process, so the executor runs there: the Windows build of
`d3dpt_exec.dll`, unchanged, loaded by `d3dpt-exec-host.exe`
(`d3dpt/exec/d3dpt_exec_host.c`) under Wine. QEMU talks to it through
`libd3dpt_exec_remote` (`d3dpt/exec/d3dpt_exec_remote.c`), which exports
the entry points of `d3dpt_exec.h` again, so `d3dpt_exec_load.c` opens it
as it opens the in-process library — after that one's `d3dpt_exec_probe`
found no device, or when `exec=wine` asks.

- **The wire** (`d3dpt/exec/d3dpt_remote.h`): 32-byte records over the
  child's stdio, synchronous, one per call.
- **The shared file.** Every region with a size — the command window,
  VRAM, the frame the executor presents — is a region of one file the
  library owns (`d3dpt_exec_shared_alloc`). QEMU maps it as guest RAM
  with `memory_region_init_ram_from_fd` (the adapter's VRAM through patch
  73), the child with `MapViewOfFile`, so a batch runs where the guest
  wrote it and a readback lands in VRAM with no copy. A caller whose
  window is not shared memory (the host tests) is served by copy, said
  once in the log. `memory_region_init_ram_from_fd` is under
  `CONFIG_POSIX`, so both call sites carry the guard; the Windows build
  has no remote executor.
- **The reply** carries the executor's `active` flag, the presented
  frame's geometry (its pixels in the frame slot) and up to 64
  `vram_dirty` ranges, folded into one past that.
- **The child's Wine.** It sets `D3DPT_D3D9=system` for the executor
  (under Wine, Wine's builtin `system32\d3d9.dll`) and writes
  `HKCU\Software\Wine\Direct3D\renderer` = `gl` into its prefix
  (`D3DPT_WINE_RENDERER` overrides; `vulkan` over MoltenVK is a data point
  only). The library starts it with `WINEDEBUG=-all` and
  `WINEDLLOVERRIDES=d3d9=b;mscoree,mshtml=` unless set — Wine's own d3d9,
  never a DXVK dropped into a prefix, and no Mono/Gecko prompt in a new
  prefix. The prefix is `<data dir>/2ksbox/wine` (first creation ~20 s).
  On a Mac the Wine process is x86_64 under Rosetta: native arm64 Wine's
  `winemac.drv` has no OpenGL, and a `MAP_SHARED` file is the same pages
  under Rosetta.
- **The device is made at probe time.** A wined3d device takes seconds
  to create (3.5 s under Rosetta), and the first batch stalls while
  wined3d compiles shaders. Made lazily at the first `create()` — inside
  the guest's MMIO read of the status register — that stall reset a Win98
  guest, so the child makes it at `d3dpt_exec_probe`, which the adapter's
  realize runs before the guest boots.

Measured: `d3dpt-dp2-test` and `d3dpt-exec-test` through the child draw
frames **byte-identical** to in-process DXVK (the `exec-wine` check) —
WineHQ 11.17 under Rosetta on the Air, and a real macOS 15.8 on the M1's
own GL (328 fps). The exec test runs 553 fps through the pipe (copy mode)
against 929 in process. In the XP guest, D3DGAME8's frame 300 is within
the rig budget, 600 frames in 3.4 s against 2.1 s; the ten DX8 DDI probes
give the in-process verdicts; FIFA 2000 plays a match at 22.6 frames/s
against 19.0 in process on KosmicKrisp. Win98's DX7 HAL: D3D7TEST 300
frames at 57 fps, byte-identical to `dp2-test.bmp`.

## Shipping it

QEMU finds the executor through no link: it `dlopen`s `libd3dpt_exec` by a
search starting at `build/d3dpt`, and the executor `dlopen`s DXVK's `d3d9`
the same way. A package with the player and not those two has guests that
silently fall back to WineD3D, so every packager stages **both or
neither** (`lib/2ksbox/libd3dpt_exec.so` + `libdxvk_d3d9.so.0`, the second
under the soname the executor looks up); the remote library and the PE
pair (`lib/2ksbox/wine/`) likewise go all or none. The Flatpak builds them
in its sandbox against the runtime (hence `third_party/dxvk` in its source
copy: the executor also needs its `include/native` headers). No Vulkan
driver travels with the Linux packages; the macOS app carries the LunarG
loader and KosmicKrisp. The packaged player names the files to QEMU
through `player/src/companions.rs`, and `player --companions` prints what
that rule resolved, which is what the packagers check. `D3DPT_EXEC_LIB` /
`D3DPT_DXVK_LIB` point `tools/d3dpt-exec-test` at a staged pair — the
cheap proof that the files themselves work.

## Milestones (P = paravirt)

All done on 2026-09-03/04 (M4); what stayed open is in the stub list
above and the M4 track doc.

- **P0a — reference workload, golden on the rig.** `D3DGAME9.EXE` /
  `D3DGAME8.EXE` (`guest-tools/src/d3dgame9.c`, `d3dgame8.c`, shared
  `d3dgame.h`, on the ISO): a deterministic game-like scene — textured
  lit indexed cubes, a per-frame dynamic vertex buffer, additive
  particles from DrawPrimitiveUP, render-to-texture, DXT1/565/8888
  mipmapped textures, fixed-function lights, an optional SM1.1/2.0 path
  with D3DX, windowed and fullscreen. `-frames N` runs a fixed-step
  sequence; `-dump N file.bmp` writes frame N. The rig (doc 09) runs both
  flawlessly; the golden set is `reference/d3d/rig-2026-09-03/` (its README
  lists the caveats: mask the HUD bars, which are wall time; `-shader` is
  vs_1_1 with a fixed pixel stage because d3dx9_36 refused ps_1_1).
  Rendering is frozen at that build until a new golden set exists;
  `tools/bmpdiff.py` compares.
- **P0b — the executor spike.** Could DXVK's d3d9 run natively on macOS?
  Not on MoltenVK (five required features missing, two unimplementable on
  Metal); yes on KosmicKrisp with `geometryShader` and `fillModeNonSolid`
  made optional (`patches/dxvk/02`, `05`). ADR-007: DXVK everywhere.
  `tools/dxvk-d3d9-test.cpp` and `tools/d3dgame9-native.cpp` (the
  unmodified scene over the window-less `tools/d3dgame-native/win32_headless.h`)
  match the rig golden to 0.35 % of pixels beyond a channel tolerance of 8
  on RADV (1089 pixels) and KosmicKrisp (1095), HUD masked. ADR-013 kept
  "an executor on something else" as an unbuilt escape hatch; ADR-018
  takes it with Wine on the host.
- **P1 — transport + device.** Patch 40, the encoder, the executor as
  `libd3dpt_exec`, DXVK's headless WSI, the guest `d3d9.dll`. D3D9TEST
  640×480 windowed on XP under TCG (Linux, RADV): 2840 fps on the device,
  1100 on WineD3D-in-guest, 4300 replaying the same batches natively. One
  doorbell per frame.
- **P2 — resources + fixed function** (`d3d9_res.h`): buffers, textures,
  Lock/Unlock, states, transforms, lights, every Draw variant, render
  targets, depth/stencil. D3DGAME9 through the device is byte-identical
  to the native DXVK frame, windowed and fullscreen, 8888 and 565.
- **P3 — shaders + queries** (protocol v3, `d3d9_p3.h`): declarations,
  constants, queries, state blocks, cube textures, DEFAULT offscreen
  surfaces, ColorFill/StretchRect/UpdateSurface/UpdateTexture, clip
  planes. `D3DFEAT9` (hand-assembled SM1.1, no D3DX) is byte-identical
  between guest and native.
- **P4 — D3D8** (`d3d8.c`). D3DGAME8 is byte-identical to D3DGAME9.
- **P5 — later.** The "proper driver" became ADR-008 / M7: a real XP
  display driver on the same transport and executor (doc 15), and the
  Win98 driver (doc 19). The DLLs stay the 9x per-game path.

The first commercial titles on the DLL path were Max Payne (to its
tutorial level) and GTA Vice City (to its menu), both headless. **Vice
City is Direct3D 8** — every RenderWare GTA through Vice City is; San
Andreas is the D3D9 one. A user who deleted `D3D8.DLL` "because VC is
D3D9" ran the game on XP's stock d3d8 over the Cirrus, where it cannot
start. A game that seems frozen on the device has so far always been a
message box behind its full-screen window (`tools/xp-game-test.sh`, M4
track).

## Reference workloads and conformance

- **Ours:** `D3DGAME9` / `D3DGAME8` (P0a) and `D3DFEAT9`, golden or
  native-DXVK frames.
- **Wine's d3d8/d3d9 test suites** (`dlls/d3d9/tests/*.c`, LGPL):
  thousands of API and pixel tests written to pass on real Windows; they
  build with mingw and run on XP — the rig gives the pass list, then the
  device runs them. Not yet wired in.
- **Irrlicht** (zlib): D3D8 and D3D9 renderers with engine-shaped sample
  apps; builds with mingw.
- **Commercial titles** (doc 04's matrix) are the acceptance bar.

## Risks

- **Host Vulkan.** MoltenVK lacks DXVK's required features; KosmicKrisp
  needs macOS 26. Below the Vulkan 1.3 floor the executor runs on the
  system d3d9 (Windows) or Wine (Linux, macOS), and with neither the
  guest keeps WineD3D over the GL pass-through until M15's last step.
- **Doorbell cost under TCG:** each doorbell is a TCG exit; batching per
  frame keeps it to a few. qemu-3dfx's 500+ fps wglgears on the Air bounds
  the transport.
- **Lock-heavy games** (per-frame dynamic buffers): the window plus DXVK's
  upload path; the DDI path keeps vertex buffers in VRAM (doc 15, v9).
- **The WineD3D-in-guest fallback's own defects** are parked by the wine9x
  rule: FIFA 2000 on it draws the pitch as noise bands and flickers
  (the front-buffer present fires on every `glFlush`). Its nine host lines
  `program error: out of range indirect offset (+65)` are wined3d's own
  ARB offset-limit probe and harmless.
