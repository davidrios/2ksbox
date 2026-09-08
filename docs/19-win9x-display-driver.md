# 19. A native Win98 display driver (ADR-012, M10)

XP has a real display driver for our `d3dpt-vga` adapter: a video
miniport, a display driver DLL, a DirectDraw DDI and a Direct3D DDI that
turns the runtime's DP2 token stream into `d3dpt` protocol records the
host executor runs on DXVK (doc 15). Win98 has none of that — it boots
`-vga cirrus` with the inbox driver and gets its 3D through the
qemu-3dfx Glide wrappers and the WineD3D DLLs in a game's folder.

This track gives Win98/Me the same driver, on the same adapter, over the
same protocol — and, because two drivers doing the same job in two source
trees is how both rot, it first **splits the XP driver into an
OS-independent core plus a thin per-OS layer** and rebuilds XP's driver
on that core before the 9x one is written.

Read doc 15 first: everything it says about the adapter, the register
set, the flip chain, the DP2 stream, palettized textures, colour keying,
execute buffers and the DX8 DDI is what the core *is*. This doc is only
about the split and about what 9x does differently.

## Why a native driver instead of the wrapper stack

- The Glide/WineD3D path needs per-game files in per-game folders. The
  driver needs nothing next to a game: XP's own `ddraw.dll` / `d3dim.dll`
  / `d3d8.dll` drive it, and the same is true of 98's.
- It is the only way a 9x title gets an accelerated *desktop* as well —
  modes from our table, page flips that are register writes, no copy
  inside QEMU (doc 15, "Shape").
- The DirectX 3–7 titles that matter on 98 are exactly the ones M7
  already made work on XP through the HAL (execute buffers, colour keys,
  palettized textures, 8 bpp modes). That work is spent; on 9x it should
  cost the per-OS layer and nothing else.
- Everything shipped by the M4 track (the paravirtual device, the guest
  DLLs) stays as it is. This does not replace it, and the DLL path
  remains the fallback wherever the driver is not installed.

## The split

Today the driver is two C files against the NT DDI: `d3dptvid.c` (the
video miniport, 555 lines) and `d3dptdisp.c` (3 708 lines) — the GDI
`Drv*` entry points, the DirectDraw callbacks dxg asks for, the Direct3D
DDI, the surface table, the caps tables and the DP2 walker, all in one
translation unit. Roughly three quarters of it never mentions a fact
about NT.

**OS-independent (the core).** Everything that is about *our* adapter and
*our* protocol:

- the DP2 walker — tokens to `D3DPT_DP2_*` records, stream bindings, the
  DX8 rewrite, `TEXBLT`, `BUFFERBLT`, the vs/ps 1.x validation, the body
  sizing table (`walk*`, ~800 lines, the single most expensive piece);
- the surface table: handles, mip levels, formats and their row/pitch
  arithmetic (including DXT), VRAM offsets, registration, dirty ranges,
  the colour-key and palette bookkeeping;
- the caps: `DDCAPS`, the pixel-format lists, `D3DCAPS7`, `D3DCAPS8`,
  the `ddflags` bisection knobs;
- contexts, render targets, `Clear2`, readback, scene capture;
- the flip chain: the offset register, the frame counter, the wait, the
  timeout that keeps a stalled refresh from hanging a game;
- the heap layout (primary, DirectDraw heap, cursor, command window) and
  the encoder in `d3dpt_enc.h`, the doorbell, the debug log through the
  DEBUG register.

**Per-OS (the layer).** Everything that is about the operating system:

- entry points and packaging: NT is a `win32k`-loaded kernel DLL plus a
  `videoprt` miniport; 9x is a 16-bit display driver with a ring-0 VxD
  beside it (see "What 9x does differently");
- kernel services: memory, VRAM and register mapping, the IOCTL to the
  miniport, a monotonic tick for the flip timeout;
- the DirectDraw/Direct3D structures the OS hands us — NT's
  `DD_SURFACE_LOCAL` / `D3DNTHAL_*` against 9x's `DDRAWI_DDRAWSURFACE_LCL`
  / `D3DHAL_*`. **The core must never see either.** The layer fills a
  neutral descriptor (`d3dpt_surf_desc`: memory, pitch, width, height,
  pixel format, caps, colour key, next mip level) and the core works on
  that. If the two layouts turn out to agree field for field on
  everything we read — they are related structures, NT's were derived
  from 9x's — the accessors collapse to inlines and nothing is lost;
  if they do not, one struct changing is one file changing.
- GDI: NT's `Drv*` DDI against 9x's `.drv` DDI over the DIB engine.

The layout, as it landed (2026-09-07; §19 has how it went):

```
guest-tools/src/d3dptvid/
  core/     d3dpt_ddi.h   the DDI structures that are the same on every
                          Windows: the DirectX caps shapes, the DP2
                          command header and its token numbers, D3DCAPS8
            d3dpt_core.h  the core's types, the hooks it calls back into
            core_dp2.c    the DP2 walker                        (784 lines)
            core_surf.c   surfaces, formats, registration, keys (453)
            core_caps.c   the caps and format tables            (328)
            core_flip.c   the heap layout, the vertical blank,
                          the debug log, the command window     (196)
            core_ctx.c    contexts, targets, clear, readback    (179)
  nt/       d3dptdisp.c   Drv* + the dxg callbacks and the OS hooks,
                          thunked onto the core                 (1975)
            d3dptvid.c    the video miniport                    (555)
            d3dptdisp.def, d3dptvid.inf
  w9x/      d3dpt9x.c     the 16-bit .drv: GDI over the DIB engine, modes,
                          palette, cursor (Watcom), + dibthunk.asm, res/
            d3dptvxd.c    the mini-VDD: the adapter, VRAM, the DOS boxes (Watcom)
            d3dpt9x.inf
            d3dpthal.c    to come: the ring-3 HAL DLL, the DDHAL / D3DHAL
                          callbacks thunked onto the same core (mingw, like NT's)
  ddk/      the NT DDI headers; ddk9x/ the 9x ones
```

Only `w9x/d3dpthal.c` links the core; the other two 9x binaries never see
it. And because the core is linked into a **kernel-mode** DLL on NT and a
**user-mode** one on 9x, it may call no operating-system service at all —
every one it needs (map VRAM, read a tick, allocate, write the debug
register) arrives through the per-OS layer. That constraint is what makes
the core portable, and it is close to true already: `d3dptdisp.c`'s core
three quarters calls almost nothing but the encoder.

**The refactor is a refactor.** It changes no behaviour on XP, and the
proof is that the M7 suite is byte-identical across it: `d3dpt-dp2-test`
and `d3d7test` against the same golden BMP, `shtest` / `cktest` /
`ebtest` / `dxttest` with the same case counts, `d3dgame8` still matching
the native oracle, Moto Racer and Vice City still drawing. Split first,
land it green, then start on 9x — never both at once, or a 9x bug and a
refactor bug are indistinguishable.

## What 9x does differently (step 0, answered 2026-09-06)

Answered by reading JHRobotics' **`vmdisp9x`** (the Win9x display
minidriver behind SoftGPU, itself derived from Michal Necasek's VirtualBox
minidriver, with Philip Kelley's `boxv9x` for QEMU's `-vga std`) and its
companion **`vmhal9x`** (the DirectDraw/Direct3D HAL). Both are cloned to
`build/ref/` (gitignored) — read, not vendored. Line references below are
to those trees at the commits cloned on 2026-09-06.

### 1. A 9x display driver is three binaries, not two

| | XP (what we have) | Win98/Me |
|---|---|---|
| GDI | `d3dptdisp.dll`, kernel, `Drv*` DDI, loaded by win32k | a **16-bit NE `.drv`** exporting the Win3.x-style DDI by ordinal (`BitBlt.1 … ValidateMode.700`), ring 3 |
| drawing | GDI draws into the engine bitmap = VRAM | every drawing export **jumps straight to the DIB Engine** (`DIBENG.DLL`); the driver keeps only `Enable`/`ReEnable`/`Disable`, `Control`, palette, cursor, `ValidateMode` |
| hardware | the video miniport `d3dptvid.sys` (`videoprt`) | a ring-0 **mini-VDD `.vxd`** (`system win_vxd dynamic`) that owns the adapter, maps VRAM, and arbitrates the DOS boxes |
| DirectDraw / D3D HAL | **in the kernel display driver**, called by `dxg.sys` | a **ring-3 32-bit DLL** loaded into the *game's* process by `ddraw.dll` |

The last row is the structural surprise and it is good news: on 9x the
part of the driver that carries our DP2 walker and surface table is an
ordinary user-mode Win32 DLL. The 16-bit `.drv` and the VxD never see the
core at all.

### 2. How the 32-bit HAL is published from a 16-bit driver

The 16-bit driver answers GDI's `Control` (escape) `DCICOMMAND` (0x0C03)
with four sub-commands (`vmdisp9x/control.c`, `dddrv.c`):

- `DDNEWCALLBACKFNS` hands the driver DirectDraw's `lpSetInfo`;
- `DDGET32BITDRIVERNAME` returns a `DD32BITDRIVERDATA` — **a DLL file
  name, an entry-point name (`"DriverInit"`) and a 32-bit context value**.
  DirectDraw loads that DLL into the calling process and calls the entry;
- `DDCREATEDRIVEROBJECT` builds `DDHALINFO` and calls `lpSetInfo`;
- `DDVERSIONINFO` reports `DD_RUNTIME_VERSION`.

The context value is the **linear address of a structure shared between
the two halves** (`VMDAHAL_t`: a far pointer for the 16-bit side, a flat
pointer for the DLL — 9x maps everything above 2 GiB into every process,
so one linear address is valid everywhere). `DriverInit` in the DLL fills
that structure's `cb32` table with its own 32-bit callbacks; the 16-bit
side then copies them into `DDHAL_DDCALLBACKS` / `DDHAL_DDSURFACECALLBACKS`
and sets the matching `DDHAL_CB32_*` flags. There is no registry entry
and no signing: whoever the `.drv` names is what gets loaded.

### 3. The DDI structures: identical calls, rearranged objects

This is the fact the whole split hinges on, and it is now checked rather
than hoped for.

**The per-call data structures are field-for-field identical.** NT's
`DD_CREATESURFACEDATA` and 9x's `DDHAL_CREATESURFACEDATA` have the same
six members in the same order; so does the one that matters most —
`D3DNTHAL_DRAWPRIMITIVES2DATA` and `D3DHAL_DRAWPRIMITIVES2DATA` are the
same fourteen fields in the same order, down to the
`lpDDVertex`/`lpVertices` union and `dwVertexSize`/`ddrval`. Only the
*names of the pointer types* differ (`PDD_SURFACE_LOCAL` against
`LPDDRAWI_DDRAWSURFACE_LCL`). Calling convention is `__stdcall` on both.

**The DirectDraw object structures are not.** NT's `DD_SURFACE_LOCAL` is
ten members; 9x's `DDRAWI_DDRAWSURFACE_LCL` is twenty-six, in a different
order, with `lpSurfMore` first instead of `lpGbl`. Every field our driver
reads exists on both sides under the same name — `lpGbl->fpVidMem`,
`lpGbl->lPitch`/`dwLinearSize`, `lpGbl->ddpfSurface`,
`lpSurfMore->dwSurfaceHandle`, `lpSurfMore->dwMipMapCount`, `ddsCaps`,
`dwFlags`, `ddckCKSrcBlt` — at different offsets, and
`lpGbl->wWidth`/`wHeight` are `WORD` on 9x against `DWORD` on NT. So the
neutral descriptor of "The split" is required, not a precaution, and
filling it is a mechanical per-OS accessor rather than a translation.

**The DP2 token stream is the same stream.** 9x's `d3dhal.h` and our
`d3dnthal.h` agree on every opcode value we handle (`TEXBLT` 38,
`CLIPPEDTRIANGLEFAN` 58, `DRAWPRIMITIVE2` 59, `BUFFERBLT` 64,
`SETVERTEXSHADERFUNC` 76 …), and `vmhal9x` sees the full DX8 set in a
real 98 guest — stream sources, vertex/index buffers, shader create/set,
palettes, state sets. The walker — the expensive three quarters of
`d3dptdisp.c` — is portable as it stands.

### 4. The DDI does reach DirectX 8 on 9x

`GetDriverInfo2` works exactly as on NT: same `GUID_GetDriverInfo2`
(aliased to `GUID_DDStereoMode`), same `D3DGDI2_MAGIC`, same
`D3DGDI2_TYPE_GETD3DCAPS8` / `GETFORMATCOUNT` / `GETFORMAT` / `DXVERSION`
sub-types, same `D3DCAPS8`. `vmhal9x` announces it by setting
`DDHALINFO_GETDRIVERINFO2` in the flags the 16-bit half passes on, and
its README claims "current DDI is 8, this means support up to DX9
programs and games". So the DX8 DDI (hardware T&L, the token rewrite,
vs/ps 1.x) is not XP-only and the whole of M7c is in scope for 98.

One difference to expect: 9x's runtime still offers the pre-DP2 HAL
entries (`RenderState`, `RenderPrimitive`, `DrawOnePrimitive`,
`TextureCreate`) that NT dropped, and `vmhal9x` implements them. Whether
a driver claiming DDI 8 can leave them out — as ours would — is the one
question of this section that only a guest can answer.

### 5. Caps rules are *not* the same as NT's

- `DDCAPS_GDI` is **normal on 9x** ("the hardware is shared with GDI")
  and `vmdisp9x` sets it. On NT it makes dxg drop the whole HAL (doc 15).
  Two OSes, opposite answers, same bit — a per-OS caps table, not a
  shared one.
- On 9x a HAL callback may return `DDHAL_DRIVER_NOTHANDLED` and
  DirectDraw's HEL takes over; on NT a declined `DdBlt` reaches the
  application as `E_NOTIMPL` (doc 15, "Blit caps and the HEL"). So 9x can
  claim `DDCAPS_BLT` cheaply — though `vmhal9x` implements blits in
  software anyway rather than leave them to the HEL.

### 6. Modes come from the registry, not from the adapter

On NT our miniport enumerates the host's mode table and `DrvGetModes`
reports it. On 9x the mode list is **written into the registry by the
INF** (`HKR,"MODES\<bpp>\<w>,<h>"`), the driver only validates and sets
them; `vmdisp9x` ships a `vesamode.exe` and a tray applet to rewrite the
list from the adapter afterwards. Consequence for us: either the INF
carries a superset of the host table, or we ship the equivalent of that
utility. M2's "the mode table comes from the player" needs an answer on
98 that it does not need on XP.

### 7. Installation

A plain `Class=DISPLAY`, `signature="$CHICAGO$"` INF matched on
`PCI\VEN_1234&DEV_3D00`, with `HKR,DEFAULT,drv,,<name>.drv`,
`HKR,DEFAULT,minivdd,,<name>.vxd`, `HKR,,DevLoader,,*vdd` and the mode
list. Selected in Device Manager or Display Settings; no `DRVINST.EXE`
equivalent needed.

### 8. Ring 3 reaches the hardware through the VxD

`vmhal9x` opens the VxD with `CreateFileA("\\\\.\\<name>.vxd")` and drives
it with `DeviceIoControl` (its `OP_FBHDA_*` ops), and reads VRAM through a
linear address the VxD published in the shared structure. For us that
gives two options for the doorbell, and the choice belongs to step 4:

- **map the register page into the process** (it is one 4 KiB page,
  `D3DPT_FB_REGS_SIZE`) and let the HAL write `DOORBELL` directly — no
  ring transition per batch, the same cost profile as XP; or
- **an ioctl per submission** through the VxD — slower, but the VxD can
  then serialise submissions.

Serialisation is the real question behind that choice, and it is new:
on NT the command window has exactly one writer because the HAL lives in
the kernel behind dxg. On 9x every process with a Direct3D device has its
own copy of the HAL and they would share one window at the top of VRAM.

### 9. Toolchain: two compilers, and a header-provenance question

- The 16-bit `.drv` and the ring-0 `.vxd` need **Open Watcom** (`wcc`,
  `wcc386`, `wasm`, `wlink`; `system windows dll` and `system win_vxd
  dynamic`), plus a small `fixlink`-style post-pass to fix the NE/VxD
  header flags wlink leaves wrong. mingw-w64 cannot produce either
  format. Open Watcom is a new build prerequisite, and one
  `scripts/build.sh` must treat the way it already treats a missing mingw:
  skip the artefact and say which one is behind. It is not a Linux-only
  prerequisite: the snapshot has a host directory per platform and
  `build-driver9x.sh` picks one from `uname`, so the Air builds both
  binaries natively too (2026-09-07, docs/build-macos.md).
- The ring-3 HAL DLL — **the one that links our core** — builds with the
  `i686-w64-mingw32` toolchain we already use; `vmhal9x` does exactly
  that, freestanding with its own tiny CRT, which is also how our NT
  driver is built. mingw-w64 already ships `ddrawi.h`, `d3dhal.h` and
  `dmemmgr.h`, so the ring-3 side needs almost no vendored headers.
- The 16-bit side is the gap: `gdidefs.h`, `dibeng.h`, `minivdd.h`,
  `valmode.h` are Windows 98 DDK headers that `vmdisp9x` vendors. Our
  rule is "no Microsoft DDK" (doc 15), so their provenance has to be
  settled before we copy anything — as does linking the `.drv` without
  Watcom's `clibs.lib` (our NT driver already builds `-nostdlib` with a
  30-line `kcrt.c`, and the VxD link in `vmdisp9x` already uses
  `option nodefaultlibs`).
- `dibeng.lib` is not a Microsoft file: it is generated by `wlib` from a
  text import list, so the DIB Engine can be imported without a DDK.

### 10. What this means for the split

The core stays freestanding C with no operating-system calls at all —
because on NT it is linked into a kernel-mode DLL and on 9x into a
user-mode one, and nothing may assume either. Every OS service it needs
(map VRAM, read a tick, allocate, write the debug register) arrives
through the per-OS layer, and every DirectDraw object arrives as the
neutral descriptor. The 9x side is then three binaries of which only one
— the ring-3 HAL DLL — links the core.

## What else moves when this lands

- ~~`-vga cirrus` stops being the Win98 answer~~ — **done 2026-09-07**:
  every Win98 machine the launcher writes is `-vga none -device
  d3dpt-vga,addr=0x02` (with the NIC pinned below it at `0x03`, so it
  cannot slide when networking is turned off), and CLAUDE.md's "Win98
  stays on `-vga cirrus`" line is gone. It applies to machines that
  already exist, since the command line is derived from the family at
  launch rather than stored in the bundle: such a guest finds an unknown
  adapter on its next start, comes up in plain VGA, and wants the driver
  installed from the guest-tools ISO (§16) before it has its desktop
  back. The test tools keep their own cirrus machines — that is where the
  inbox driver stays exercised.
- `SETUP.EXE` grows a display-driver component for the 98/Me role. Today
  `tools/setup-guest-test.sh win98` *fails the run if that component is
  even offered*; that check inverts, and the Win98 boot in it moves onto
  the adapter.
- The Win98 acceptance titles (doc 04) get a second path to be measured
  on: the driver against the Glide/WineD3D stack, same image, same host.

### 11. The mini-VDD is not optional (measured 2026-09-06)

Step 0 read this as "a ring-0 VxD for the 32-bit work and DOS-box VGA
arbitration", i.e. something a first cut could leave out. It cannot. The
first `.drv`-only driver was built and run in a real Win98 guest, and the
adapter's PCI base addresses tell the story:

```
BARs  bios   BAR0: prefetchable memory at 0xf0000000  BAR1: memory at 0xfebf0000
BARs  30s    BAR0: (not mapped)                       BAR1: (not mapped)
```

SeaBIOS maps both BARs at POST; **Windows 98 unmaps them within the first
half-minute of boot** and never puts them back. Nothing claimed the
device's resources, so the Configuration Manager took them away — and a
16-bit driver that then reads the BARs out of PCI config space finds
zeros, fails, and GDI falls back to VGA (a 16-colour desktop, which is
how the run reports itself). Claiming those resources is the ring-0
half's job on 9x, which is exactly why `vmdisp9x` says it moved "most
calls to the 32-bit mini-VDD driver": the mini-VDD registers with the
main VDD (`VDD_REGISTER_DISPLAY_DRIVER_INFO`) and hands the 16-bit driver
a mapped frame buffer rather than letting it map anything itself.

So the 9x work has a fixed order that the XP work did not: **the VxD comes
before the display driver can do anything at all.** What the `.drv`-only
attempt did establish, and what carries over unchanged:

- Open Watcom builds a 16-bit NE display driver on Linux — and on macOS
  arm64, out of the same tarball's `armo64` — and the binary
  checks out: module `DISPLAY`, the ordinal export table, imports from
  `KERNEL` and `DIBENG` only, no C runtime (§9's licence question
  answered — nothing OWPL-licensed enters the binary).
- **A display driver must carry `oembin` resources** (`config.bin`,
  `colortab.bin`, `fonts.bin`, `fonts120.bin`): the machine metrics, the
  Control Panel's colour table and the three system LOGFONTs live inside
  the `.drv`, and GDI needs them. They are small fixed structures from the
  Windows 3.1 DDK, written as C and linked to raw binaries.
- **The INF path works with no clicks.** With `d3dpt9x.inf` and the driver
  in `C:\WINDOWS\INF`, Win98's PnP matches `PCI\VEN_1234&DEV_3D00`,
  installs silently, asks to restart, and writes `drv=d3dpt9x.drv` into
  `Services\Class\Display\0000` — the registry is correct, `SYSTEM.INI`
  keeps `display.drv=pnpdrvr.drv` and puts the device description in
  `[boot.description]`, which is what a real driver's install looks like.
- **`SYSTEM.INI` is not a shortcut.** Setting `[boot] display.drv=` by
  hand and skipping the INF does not work: Windows rewrites the line when
  it re-detects the adapter.
- **Two debug channels, and the driver wants both.** The adapter's DEBUG
  register cannot say anything before the register page is mapped, which
  is where the interesting failures are, so every line also goes to port
  0xE9 (`-debugcon file:…`). In this run both were silent, which is
  consistent with the BARs being gone.

`tools/win98-driver-test.sh` is the harness: it stages the driver into a
raw copy of the image (never the user's qcow2), boots on the adapter, and
prints the BARs at three points, the screen's colour count and both debug
channels.

### 12. Linking a VxD that the VMM will actually load (2026-09-06)

The mini-VDD of §11 was written and it works — it claims the adapter,
keeps the BARs, maps VRAM and the register page, checks the register set's
magic and version, and installs itself in the main VDD's mini-VDD dispatch
table:

```
d3dptvxd: Device_Init          d3dptvxd: vram=08000000
d3dptvxd: bar0=f0000000        d3dptvxd: dispatch entries=0000003e
d3dptvxd: bar1=febf0000        d3dptvxd: ready
```

(the last lines arrive twice over, once on port 0xE9 and once through the
adapter's DEBUG register into the QEMU log, which is the register mapping
proving itself). **The base addresses now survive the whole boot**, which
was §11's blocker.

Getting there cost three facts about `wlink`'s VxD output, none of which
announce themselves — a VxD the VMM dislikes is simply never loaded, with
no entry in `BOOTLOG.TXT` and nothing on any debug channel:

- **Every LE object must have base address 0 and the executable flag.**
  wlink leaves the objects at 0x10000 and 0x11000 and marks only the first
  executable. A VxD is flat: all its pages start at the beginning. (This
  is what `vmdisp9x`'s `fixlink -vxd32` does, and it is the whole of what
  it does — not the header flags, which is what one guesses first.)
- **The DDB must be at offset 0 of the *code* object.** Left to itself the
  compiler puts it in `_DATA`, the entry table then names object 2 at
  offset 0x1e0, and the loader does not find it. Putting code, data and
  constants into one segment of class CODE (`#pragma data_seg("_LTEXT",
  "CODE")` and its two siblings) links the module to a single object with
  the DDB first — the shape a real driver has: the guest's own
  `FXMEMMAP.VXD` is one object whose entry table reads *object 1, offset
  0*.
- **The entry-table bundle must be a 32-bit entry (type 3).** wlink writes
  type 2 because the exported symbol is data. With a zero offset the
  record bytes are identical either way, so this is one byte.

Where the VMM's own diagnostics would have helped, they did not: `BootLog=1`
in `MSDOS.SYS` did not produce a fresh `BOOTLOG.TXT` on this image (the one
there was four days old, and reading it as if it were current wasted a
cycle — check the file's date first). Port 0xE9 is the reliable channel in
ring 0, and `-debugcon` was verified independently by pointing it at 0x402
and watching SeaBIOS write to it.

**What claims the resources is the registry, not the code.** The VxD logs
non-zero BARs at `Device_Init`, i.e. Windows had not stripped them by
then, and its re-programming path has never had to fire. So it is
`minivdd=<file>` in the devnode's key — the devnode having a complete
driver set — that makes the Configuration Manager leave the device alone.
The re-programming stays as belt and braces.

### 13. Why GDI would not load the driver (answered 2026-09-06)

**One dangling relocation.** `d3dpt9x.drv`'s first instruction never ran,
and none of the things this section used to list as suspects — the export
table, a missing `#16` VERSIONINFO, a `_TEXT` without an `_INIT` — had
anything to do with it.
The module was simply malformed, and the linker map said so all along:

```
_TEXT   CODE      AUTO    0001:0000   0000096e
CONST…  DATA      DGROUP  0002:….
_TEXT   FAR_DATA  AUTO    0003:0000   00000000     <- empty
```

The culprit is the idiom every 9x display driver uses to lock its own code
segment:

```c
extern char __based(__segname("_TEXT")) *pText;
GlobalSmartPageLock((__segment)pText);
```

That reference lands in a *second* segment also called `_TEXT`, of class
`FAR_DATA`. In a driver as small as ours it stays empty, wlink drops it
from the NE segment table — and leaves the relocation that named it
pointing at segment 3 of a module that has two. KERNEL's loader indexes
the segment table with that number, refuses the module, and says nothing
anywhere: no `BOOTLOG.TXT` line, no dialog, no fallback message. It looks
exactly like GDI choosing a different driver.

vmdisp9x does not hit this because its `_TEXT`/`FAR_DATA` segment is not
empty (`scrsw.c` puts a variable there). Ours takes `CS` instead, which is
the same selector and needs no relocation at all.

**The build now fails instead of the boot.** `build-driver9x.sh` walks the
NE relocation records and rejects any internal reference to a segment the
segment table does not have. It also checks the map for `VXD_DDB` at
`0001:00000000`, the other silent-refusal trap (§12), which was reproduced
once more in this session by adding a single `static const` array to the
VxD: the const data was emitted ahead of the DDB and the VMM stopped
loading the module without a word.

### 14. Getting the adapter into ring 3 (2026-09-06)

With the module loading, `DriverInit` ran, the mini-VDD answered, and the
driver read its registers as **zero** — a register set that looks like a
different device, from a driver whose every debug write vanished. Two
things were changed to fix it, and only the second is *proven* to have
been the cause; both are described because the first is right on the
documentation's own terms and the second was hiding behind it.

**The mapping is now one ring 3 can reach.** `_MapPhysToLinear` is the
obvious call and, on paper, the wrong one here: it maps into the system
arena, and the vendored header's own comment on `PC_USER` reads "make the
pages ring 3 accessible", which a system-arena mapping is not. So the
mini-VDD now uses `_PageReserve(PR_SHARED, …)` followed by
`_PageCommitPhys(…, PC_USER | PC_WRITEABLE | PC_INCR)` — the shared arena,
with the user bit set — and ring 0 may read a user page, so the one mapping
serves the VxD and the driver both. Changing this on its own did **not**
make the registers readable, and the page tables of the old mapping were
never dumped, so whether it would have worked once the accesses were the
right width is not known.

The flag set is not negotiable and not obvious: **this VMM refuses
`PC_PRESENT`**, and refuses it silently, returning zero from
`_PageCommitPhys` with everything else correct. `PC_INCR` is required —
without it the whole 128 MB aliases onto one physical page — and it is
accepted only once `PC_PRESENT` is gone. `_CopyPageTable` on the first and
last page of a mapping is the check that settles it in one boot:
`f0000a07` … `f7fffa07` is right, the same value twice is not.

**The registers are 32 bits wide and the device accepts nothing else.**
`d3dpt-vga`'s register BAR is `valid.min_access_size = 4`. A 16-bit
compiler turns `*(DWORD __far *)p` into two word accesses, and QEMU answers
each of those with zero and drops the writes — no fault, no log line, on
either side. So `RegGet` / `RegPut` are hand-written 32-bit accesses
(`mov eax, es:[bx]`, the module is `.386` throughout). **The XP driver
never met this**, because it is 32-bit code and the compiler emitted what
the device wanted; any future 16-bit half of this stack has the same
obligation. The adapter needs no change for 9x, which is what the track
hoped for.

What settled it, in order, was: read the registers through the selector
*from ring 0* (`mov fs, ax` / `mov eax, fs:[0]` — the descriptor is fine),
hand the driver an LDT selector as well as the GDT one (both zero from ring
3 — not the selector), and then disassemble `RegGet` out of the NE image,
where the two `mov ax,[es:bx]` say it.

### 15. The strip of garbage was the message (2026-09-06)

The driver loads, claims the adapter through the mini-VDD, and sets the
mode: `d3dpt-vga: linear mode on (640x480x32 pitch 2560 offset 0)`, and the
driver's own `d3dpt9x:` lines arrive through the DEBUG register exactly as
the XP driver's do. GDI draws into guest VRAM — the DIB Engine's software
cursor lands at the right place and the right scale, which is the proof
that the frame buffer's address, pitch and depth all agree.

Then the screen stops changing: black, a wait cursor, and a band of
coloured noise across the top of the frame buffer. That reads exactly like
a desktop that is merely slow to paint, and it is not. **The band is a VGA
text screen showing through**, and what it says is:

```
                                   Windows
   A fatal exception 0D has occurred at 036F:000003C1.  The current
   application will be terminated.
```

Two facts make it invisible, and each is worth keeping.

**QEMU's VGA core stores its planes interleaved, four bytes to a character
cell, from offset 0 of the same VRAM.** So the text page the VMM writes at
0xB8000 lands in the first 32 KB of the linear frame buffer — 12.8 lines of
a 640×480×32 mode — and a character cell of `20 17` (a space, light grey on
blue) is scanned out as one dark navy pixel. The band is *exactly* 32 KB
wide because that is the size of the 0xB8000 text window; the tell that it
is not our drawing is that it survives the driver's own clear of the whole
frame buffer, because the VMM writes it afterwards.

**A 9x fatal exception is a text-mode screen, and nothing switches the mode
back.** On an adapter Windows knows, the VDD returns the hardware to VGA
before writing it. Ours is a device the VDD cannot put back — that is the
whole point of a mini-VDD — so the message is written into a frame buffer
that is still scanning out 32 bpp. Every earlier run of this track that
"stopped at a black desktop" was a machine that had already faulted and was
holding up a message nobody could read.

`tools/win98-driver-test.sh` now reads that page out of VRAM after every
run and prints it (`text  Windows has a VGA text screen up behind the frame
buffer:`). It is the single most useful thing the harness does: without it
a fault and a slow paint look the same, and both look like the driver
almost working.

It has to prove the page *is* text first, or it invents messages: once the
driver runs, those 32 KB are the top of the desktop, and every fourth byte
of a 16 bpp desktop read as a character code produced three convincing
lines of nonsense. Three things are true of a text page and of almost no
pixel data — plane 3 untouched (plane 2 is not: that is the character
generator), a handful of distinct attributes, and mostly spaces — and it
takes all three, because a 32 bpp desktop passes the first on its unused
byte.

The fault itself is inside **DIBENG.DLL**, at the first instruction of a
blit-shaped routine: `lds si,[bp+0x32]` followed by `deFlags`,
`deBeginAccess` / `deEndAccess`, `deBitsOffset` / `deBitsSelector`,
`deDeltaScan` and a jump table on `deBitsPixel`. So the Engine is
dereferencing a `DIBENGINE` far pointer it was handed, and the selector in
it is not a selector. What we hand it out of band is `lpDriverPDevice`,
pushed by every `DIBTHK` thunk in `dibthunk.asm`; that is where to look
first, and the reference driver's own thunk list — which does *not* thunk
`ExtTextOut` or `SetPalette`, and reaches the cursor entries through C — is
the thing to hold ours against.

### 16. Installing it, the way every other driver installs (2026-09-07)

PnP installs the driver from `d3dpt9x.inf` with no clicks and writes both
halves into the adapter's own registry key — `drv=d3dpt9x.drv`,
`minivdd=d3dpt9v.vxd`, `vdd=*vdd`, `DevLoader=*vdd`, `Mode` and the mode
list — and then rewrites SYSTEM.INI's `[boot] display.drv` to
`pnpdrvr.drv`.

**That is the correct configuration, not a failure.** `pnpdrvr.drv` is the
name Windows writes for every PnP display driver; there is no such file on
disk and there is not meant to be one, and the pristine test image says the
same thing with `[boot.description] display.drv=Cirrus Logic` beside it.
Anything that has to name the driver in SYSTEM.INI to be loaded is working
around a bug of its own.

Ours was one, and the bug was in the INF: **no `DelReg`**. A display
adapter that has been running the inbox VGA already has a `CURRENT` key
naming that driver and a `MODES` tree describing its modes, and an AddReg
does not remove what it does not mention — so the leftovers are what GDI
resolves through and the new values are never reached. The reference driver
deletes `Ver`, `DevLoader`, `DEFAULT`, `MODES` and `CURRENT` first. With
that, and with the two 4 bpp rows that hand `MODES\4\640,480` to `vga.drv`
and `MODES\4\800,600` to `supervga.drv` by name (a machine sitting in a
16-colour mode has to resolve to *something*, and it is not us):

```
$ NAME_IN_INI=0 tools/win98-driver-test.sh ~/vms/win98.qcow2 install
==> restarting to finish the install
...
d3dptvxd: Device_Init          <- from the registry's minivdd value
d3dpt9x: DriverInit entered    <- from its drv value, through pnpdrvr.drv
d3dpt-vga: linear mode on (800x600x16 pitch 1600 offset 0)
shutdown   clean
```

Nothing anywhere names either half. Proven 2026-09-07 on a pristine image;
`NAME_IN_INI` defaults to off and is kept only for the other question,
which is "does *this build* of the driver work" rather than "does it
install".

Two things about that run worth keeping. The mode came out **800x600x16**,
not the `DEFAULT,Mode` of `32,640,480` the INF writes — Windows picked from
the `MODES` list rather than the default, and 16 bpp is the depth the era's
drivers actually ran, so this is the interesting configuration rather than
a wrong one. And the run before the fix appeared to hang on the boot logo
for eight minutes after the restart: that was **patch 22's bug, not this
one** — Win98 turns its local APIC off, RESET did not put
`CPUID.01H:EDX.APIC` back, and the guest span forever on the BIOS tick
counter behind an unchanging splash screen (`patches/qemu/README.md` patch 22; `docs/00-status.md`).
A frozen 9x splash screen after a restart is that until proven otherwise.

**On the guest-tools ISO this is `DRIVER9X\`**, and `SETUP.EXE`'s display
adapter component now offers itself on 98/Me: it copies `D3DPT9X.INF`,
`D3DPT9X.DRV` and `D3DPT9V.VXD` into `WINDOWS\INF` (and the two binaries
into `SYSTEM` as well, which the INF's own `CopyFiles` ought to make
redundant and which is three kilobytes against finding out otherwise on
somebody's machine) and asks for a restart. There is deliberately no
installer to run on this side — the INF is the installer, and the boot is
what runs it. The 9x driver needs Open Watcom, which the ISO build does not
require: a host without it builds an ISO without that folder and says so
rather than shipping a disc whose component answers "not on this disc".

`NAME_IN_INI=1` still writes, in binary because SYSTEM.INI has CRLF line
endings and Python's text mode eats them:

```
[386Enh]
device=C:\WINDOWS\SYSTEM\D3DPT9V.VXD
[boot]
display.drv=d3dpt9x.drv
```

### 17. Ending a run

**End a run with the ACPI power button**, not keystrokes; a run that does
not power off leaves the FAT dirty, and the boot after it is a ScanDisk or
**safe mode** — no driver, no VxD, an empty debug log, which reads exactly
like the driver having failed and costs a full cycle to recognise. Two
things get in the way of the button and the harness now handles both: the
`install` run always ends on "to finish setting up your new hardware, you
must restart your computer" (`alt+n` is No, and the restart is the next
run's job), and a modal dialog swallows the power button as surely as it
swallows keys, so Escape goes first.

A machine that has faulted cannot be shut down at all — it is in the text
screen above, waiting for a key that only reaches DOS. That is not a
harness bug to chase; it is the fault, and the printed text screen says so.

### 18. Display Settings showed one resolution (fixed 2026-09-07)

With the driver installed, Display Settings offered 640×480 at 16 and 32
bpp, 800×600 in 16 colours, and nothing else — while the registry held
all eight `MODES\16` / `MODES\32` rows the INF writes. The rows were
there; the applet was *asking the driver* about each one and dying.

The applet (32-bit, in `rundll32`) thunks down and calls the driver's
`ValidateMode` (ordinal 700, the export valmode.h says must exist) once
per registry row. Ours GPFed on the first call, and the reason is a
compiler rule: **Open Watcom takes a function's attributes from its first
declaration.** `valmode.h` prototypes `ValidateMode` as plain `WINAPI`,
so the `__loadds` on the definition was ignored, and the export ran on
the caller's DS — a thunk's 16-bit alias of the applet's 32-bit stack.
`AdapterFind()` read `wRegsSel` from that stack, found a nonzero word and
took it for "already asked"; `ModeOk()` loaded the same word into ES for
`RegGet(CAPS)` and faulted (`mov es,cx` with CX=0x12, GPF error code
0x10). Every other export had the prologue, because nothing else in the
DDK headers prototypes them. The `-d int` trace is how it was found: the
fault sits at `031f:0021` — the CS the driver logs at `DriverInit`, and
offset 0x21 is `RegGet` — with DS a 2 MB selector that is not ours.

The 16 colour mode the applet still offered came from the `MODES\4`
rows, which name `vga.drv`/`supervga.drv` and so are never put to us;
why the two 640×480 entries survived the fault was not established (the
current mode is presumably listed before the driver is asked).

The fix hides the header's prototype (`#define ValidateMode …` around the
include) so the definition is the first declaration, and
`build-driver9x.sh` now refuses a `.drv` in which any export neither jumps
straight to the DIB Engine nor loads DGROUP in its first eight bytes —
the third silent failure the build catches (§13, §14 are the others).
Proved on the `test98` image: all eight modes come back `VALMODE_YES`
in the debug log, the slider has four stops, and applying 800×600 from
the applet switches the adapter live (`linear mode on (800x600x32 …)`)
and back.

A note on driving the applet headless, for the next time: on a
Portuguese Windows the Start menu's Run item is "Executar" (`e`, not
`r` — `r` lands on nothing and `d` is "Desligar", which restarted the
machine), `control desk.cpl,,3` from Run lost its arguments and opened
the Control Panel folder, and Ctrl+Tab did not switch the property
sheet's tabs. What worked: a right-click on the desktop through the USB
tablet, Up + Enter for Properties, and a click on the tab itself.

### 19. The split, as it landed (2026-09-07)

`d3dptdisp.c` was 3 708 lines against the NT DDI. It is now 1 975 lines
of NT plus 1 940 of core in five files, and the core includes no DDK
header of either family. XP is unchanged: the same driver, the same
answers, the same log lines.

**Two boundaries, not one.** The plan above said "a neutral descriptor",
and that turned out to be the answer for exactly one of the two kinds of
structure the driver handles:

- The **DDI payloads** — `D3DDEVICEDESC_V1`, `D3DPRIMCAPS`, the extended
  caps, `D3DCAPS8`, the `DP2COMMAND` header, the `GetDriverInfo2` shapes
  — are the same on NT and 9x, field for field (§3). They were already
  *our* definitions rather than a DDK's: the vendored `d3dnthal.h` is a
  ReactOS-derived transcription, spelled out with trailing underscores so
  a kernel build never has to reach `d3dtypes.h`. So they moved out of it
  into **`core/d3dpt_ddi.h`**, one definition, and `d3dnthal.h` includes
  that and keeps only what is genuinely NT: the callback *data*
  structures (`D3DNTHAL_CONTEXTCREATEDATA` and friends) and the callback
  tables. The 9x `d3dhal.h` will do the same, and the day one of the two
  disagrees is the day the disagreement gets its own struct.
- The **surface objects** are not the same: NT's `DD_SURFACE_LOCAL` and
  9x's `DDRAWI_DDRAWSURFACE_LCL` hold the same facts under the same names
  at different offsets. That is where `d3dpt_surf_desc` goes — a flat
  record of everything the core reads off a surface (handle, caps,
  caps2, flags, w, h, pitch, linear size, `fpVidMem`, the resolved
  D3DFORMAT, the pixel format's flags for the log, the source colour
  key). The core also keeps the OS's object as an opaque `void *`,
  because a colour key set *after* a texture was mirrored is read off it
  when the texture is next bound.

**Six hooks, and nothing else.** The core reaches the OS through
`d3dpt_os_alloc` / `d3dpt_os_free` (NT: `EngAllocMem` with the pool tag),
`d3dpt_os_ticks` (`EngQueryPerformanceCounter` / `Frequency`, the flip
timeout's clock), `d3dpt_os_surf` (fill a descriptor),
`d3dpt_os_attached` (the surfaces attached to this one that are *not*
mip levels — a flip chain's other buffers, a Z buffer) and
`d3dpt_os_next_mip`. The attach-list walks stayed shaped as they were:
the *filtering* moved into the hook, the breadth-first search over a
chain stayed in the core, where the comment about GTA 2's unregistered
back buffer belongs.

**And the constraint is now a build check.** `build-driver.sh` compiles
the five core files on their own and asks `nm` what they still want from
outside: every undefined symbol must be one of the core's own, one of the
`d3dpt_os_*` hooks, or `memcpy` / `memset`. Nothing in the *source* stops
a `Eng*` call from being written into the core — it would build fine here
and fault on 9x, where there is no `win32k` at all — so the check is the
only thing that does. It was proved by breaking it on purpose:
`ERROR: the core calls out of itself: _EngDebugPrint`.

**What stayed in the layer, and why.** The callback *tables* dxg is
handed (`D3DNTHAL_CALLBACKS`, `DD_D3DBUFCALLBACKS`) name this file's
functions, so filling them is the layer's; what they *claim* is
`core_caps.c`'s. `d3d_caps_init` used to do both, and pulling the tables
out of it left them uninitialised for one build — the kind of mistake
this refactor is most likely to make, and the reason to run the whole M7
suite rather than trust that it compiles. `DdCreateSurface`'s sizing of a
compressed surface for dxg's heap stayed too (the arithmetic is
`surf_dxt_size` in the core; the DDK dance around `fpVidMem =
DDHAL_PLEASEALLOC_BLOCKSIZE` is NT's), as did every `Drv*` entry, the
mode list, the palette, the hardware cursor and the memory mapping.

**The DirectDraw *API* structures are not the problem.** `DDPIXELFORMAT`,
`DDSURFACEDESC`, `DDSCAPS` and the `DDRAWISURF_*` flag values come from
the public `ddraw.h` and are identical on both; the core uses them
directly. Only `DDSCAPS_EXECUTEBUFFER` needed spelling out, because
mingw's public header has dropped the DirectX 3 name that the DirectX 8
runtime's vertex and index buffers still arrive under.

**The one bug it introduced, and what it cost.** Moving a constant is
the hazard this arrangement has: `DDSCAPS_EXECUTEBUFFER` is not in
mingw's public `ddraw.h`, so the core header spelled it out — as
`0x00000800`, which is `DDRAWISURF_HASCKEYSRCBLT`'s value, not its. The
right one is `0x00800000`; the DDKs write it `DDSCAPS_RESERVED2`, the
name it was renamed to when execute buffers left the public API. Nothing
failed to build, and the NT layer was unaffected (it includes
`ddrawint.h`, whose definition won the `#ifndef`), so only the *core*
believed it — and what the core uses it for is deciding whether a
surface is a vertex or index buffer. The DirectX 8 runtime's vertex
buffers carry no `DDSCAPS2_VERTEXBUFFER` (its index buffers do carry
`DDSCAPS2_INDEXBUFFER`, which is why only half the path broke), so
protocol v9's video-memory vertex buffers quietly stopped being
recognised. `SHTEST`'s "vs 1.1 from a vertex + index buffer" case drew
the *previous* case's colour and the run came back `9 cases, 1 failed`.
That is the whole argument for running the guest battery over a
refactor: it compiles, it installs, it draws a perfect D3D7TEST frame,
and one case in nine says the value is wrong.

The fix is not just the right number. The core now defines its three
DirectDraw-internal bits under its own names
(`DDSCAPS_EXECUTEBUFFER_`, `DDRAWISURF_HASCKEYSRCBLT_`,
`DDRAWISURF_HASPIXELFORMAT_`) and the NT layer, which can see both, fails
to compile if any of them disagrees with the DDK's. The 9x layer will
carry the same three lines against `ddrawi.h`.

**The size the driver lost.** `.text` went from 0xa890 to 0xa070 — about
two kilobytes — because the old single translation unit inlined
`d3d_register_at` and the pixel-format helpers into several callers and
the split cannot. Nothing was removed: the set of functions before and
after differs only by the additions, plus `surf_format` / `surf_caps` /
`surf_next_mip`, which are now the NT half of `d3dpt_os_surf`.

### 20. Publishing the HAL: the chain works, the runtime does not bite yet (2026-09-08)

M10 step 3's first half. All three of the 9x binaries now exist —
`d3dpt9x.drv` (16-bit, Watcom), `d3dpt9v.vxd` (ring 0, Watcom) and
**`d3dpt9hl.dll`** (ring 3, mingw, the only one that will link the core)
— and the whole publication path of §2 runs end to end in a real guest:

```
d3dpt9x: QUERYESCSUPPORT(DCICOMMAND) -> 0x00000100
d3dpt9x: DCICOMMAND version=00000200 command=0000000d   (DDVERSIONINFO)
d3dpt9x: DCICOMMAND version=00000200 command=0000000b   (DDGET32BITDRIVERNAME)
d3dpt9dd: shared block at=8a7cf000 regs linear=80018000 vram linear=80019000
d3dpthal: DriverInit, block at 0x8a7cf000 regs 0x80018000 vram 0x80019000
          mode 0x028001e0 bpp 0x00000020
d3dpt9x: DCICOMMAND version=00000200 command=0000000c   (DDNEWCALLBACKFNS)
d3dpt9x: DCICOMMAND version=00000200 command=0000000a   (DDCREATEDRIVEROBJECT)
d3dpt9dd: hal for mode=028001e0 dd callbacks=00000011
d3dpt9dd:   the DLL read magic=42463344 version=00000004
d3dpt9dd: DirectDraw took the HAL
```

Read the fourth and fifth lines together, because between them is the
thing this step existed to prove: **DirectDraw loaded our DLL into the
probe's own process, called `DriverInit` there, and the DLL read the
adapter's `MAGIC` and `VERSION` registers** — `42463344` is `D3FB` —
**straight through the linear address the mini-VDD mapped, from ring 3,
with no ioctl.** That settles §8's open question in favour of its first
option: the doorbell can be a direct register write, the same cost
profile as XP, and the VxD needs no `DeviceIoControl` handler at all.

**What was not done then, and is now: §21.** `DDHAL_SetInfo` returned
TRUE and the 32-bit runtime kept using its own HEL anyway: `ddprobe` still sees
`dwCaps 0x02000000`, no video memory, and `E_NOTIMPL` from
`WaitForVerticalBlank` — the one callback the DLL publishes. So the
16-bit half is satisfied and the 32-bit half is not, and the next
session's question is exactly that seam. What has been ruled out
already: the callbacks' flags and table offsets (checked against the
DDI), the DLL's module handle (published through the block and returned
from `DDCREATEDRIVEROBJECT`, `hinstance=6ff40000`), the video-memory
heap (one linear heap behind the primary), the mode list, and the
pointer *kind* in `DDHALINFO` (below).

**Three facts this cost, each of which looked like something else:**

1. **`QUERYESCSUPPORT(DCICOMMAND)` must answer `DD_HAL_VERSION`, not 1.**
   The return value is how DirectDraw learns what kind of driver this is.
   Answering 1 — the obvious "yes, supported" — tells it DCI and nothing
   more: it sends one DCI `DCICREATEPRIMARYSURFACE` (version `0x100`,
   command 1) and never asks a DirectDraw question again. No
   `DDVERSIONINFO`, no `DDGET32BITDRIVERNAME`, no HAL, and no error
   anywhere — the driver simply looks like a 1994 DCI driver forever.
2. **DPMI cannot tell you a GDT selector's base.** The `.drv` holds
   selectors onto the register page and VRAM, and the ring-3 DLL needs
   the linear addresses behind them; `int 31h AX=0006` is the obvious
   way and it is wrong here, because the mini-VDD builds those selectors
   in the **GDT** and DPMI only knows the LDT. It answers with junk and
   ignores its own carry flag: the DLL was handed a register page at
   `0x28d7`. The VxD knows both addresses — it now returns the register
   page's in `EDI` alongside VRAM's in `ESI` (`d3dpt9v.h`), and nobody
   has to derive anything.
3. **The pointers in `DDHALINFO` are 16-bit far pointers, not linear
   addresses** — `lpDDCallbacks`, `lpDDSurfaceCallbacks`,
   `lpDDPaletteCallbacks`, `lpModeInfo`, `vmiData.pvmList`. It is the
   *16-bit* runtime that walks them. Handing it linear addresses instead
   makes `DDHAL_SetInfo` refuse the whole HALINFO. The tables still live
   in the shared block, but for the other reason: the DLL reads them too,
   by offset from the block's linear base.

**The shared block** (`w9x/d3dpt9hal.h`) is what the two halves agree on:
a magic and a version the DLL refuses to read past, the adapter's two
linear addresses and VRAM size, the current mode, the `cb32` table the
DLL fills, the DLL's own module handle, the DirectDraw tables themselves,
and a word for serialising the command window between processes (§8's
second question, still open and not yet needed). It is allocated by the
`.drv` through DPMI, which puts it in the shared arena above 2 GiB —
`8a7cf000` — where every process can see it. Both toolchains compile it,
so every field is a fixed-width type and the 16-bit half asserts at
compile time that each DDI structure fits the slot reserved for it.

**How to run it.** Nothing on a Win98 desktop calls `DirectDrawCreate`,
so `tools/win98-driver-test.sh` grew a way to start a program:
`PROG=<file.exe>` stages it and names it in WIN.INI's `[windows] run=`,
which the shell honours once it is up — this harness has no serial line
and no shell to type at, so that is the only hook there is. The program
built for this is `ddprobe.exe`: it creates a DirectDraw object, prints
the HAL and HEL caps and calls `WaitForVerticalBlank` twice, leaves
`C:\DDPROBE.LOG` for the harness to read back, and draws nothing.

```sh
WATCOM=$HOME/.local/opt/open-watcom guest-tools/build-driver9x.sh
PROG=guest-tools/out/driver9x/ddprobe.exe \
  tools/win98-driver-test.sh ~/.local/share/2ksbox/machines/test98/disk.qcow2 install
```

**A build check came with it**, in the same spirit as §13/§14/§18's: a
freestanding DLL has no CRT startup, so its entry point has to be named
by hand — and `ld` only *warns* when it cannot find one, leaving
`AddressOfEntryPoint` zero. Windows would then call address zero the
moment DirectDraw loads the DLL into a game, which on 9x is a silent
reboot. `build-driver9x.sh` fails the build on a zero entry point, on a
missing `DriverInit` export, on an import from anything but `kernel32`,
and on an instruction past the Pentium III floor.

### 21. The runtime read the HALINFO and threw it away (2026-09-08)

§20 left the DirectDraw half exactly one fact short: the chain ran, the
DLL was loaded into the game's process, `DDHAL_SetInfo` returned TRUE —
and `ddprobe` still saw `dwCaps 0x02000000` (`DDCAPS_NOHARDWARE`), no
video memory, `DDERR_NODIRECTDRAWHW` from a video-memory surface and not
one of the DLL's callbacks ever entered. The 16-bit half was satisfied
and the 32-bit half was not.

**The cause is one bit: `DDCAPS2_CERTIFIED` in `ddCaps.dwCaps2`.**
"Certified" is something the *runtime* says about a driver, never
something a driver says about itself, and DirectDraw's HALINFO validator
refuses any driver that claims it.

**Why it is invisible.** The two halves of the runtime do different jobs.
The 16-bit `DDHAL_SetInfo` a driver calls from `DDCREATEDRIVEROBJECT`
only *stores* the HALINFO — it validates nothing, and returns TRUE. The
validation happens afterwards, in 32-bit `ddraw.dll`, in the function
that builds the `DDRAWI_DIRECTDRAW_GBL` out of the stored HALINFO; when
that returns NULL its caller silently builds an emulation-only object
instead. So the driver's own log says "DirectDraw took the HAL", every
application sees a HAL that was never published, and nothing anywhere
says which of the two happened.

**How it was found**, since no amount of guessing had moved it: the
guest's own `DDRAW.DLL` (DirectX 6.1, 299 008 bytes, `ImageBase`
`0xbaaa0000`) was pulled out of the image with mtools and disassembled.
`DirectDrawCreate` → the escape routine at `0xbaab6634` (five
`push $0xc03` sites, one per `DCICOMMAND`) → the object builder at
`0xbaab3dc7` → its first gate, the HALINFO validator at `0xbaab4d63`. The
validator is a plain list of rules, and reading it is worth more than any
documentation of the interface:

| the rule | what the driver must do |
|---|---|
| `dwSize` is 0x130, or 0x1cc, or larger | the DX3 or the DX5+ `DDHALINFO`; ours is 0x1cc |
| the whole structure is readable | it is `IsBadReadPtr`'d for `dwSize` bytes |
| every heap has a non-zero `fpStart`, and is not `VIDMEM_ISNONLOCAL` | a heap that is `VIDMEM_ISHEAP` also needs `DDCAPS2_NONLOCALVIDMEM` |
| `vmiData.ddpfDisplay.dwSize == 32` | and if it is `DDPF_PALETTEINDEXED8`, `dwRGBBitCount == 8` |
| each callback table's `dwSize` is a multiple of 4 and at least its own minimum | 0x28 or ≥0x30 for `DDHAL_DDCALLBACKS`, ≥0x40 for the surface table, ≥0x10 for the palette one, ≥0x1c for the execute-buffer one |
| every callback a `dwFlags` bit claims passes `IsBadCodePtr` | the bit's index is the entry's index, counting from the first pointer |
| `ddsCaps.dwCaps` has no `DDSCAPS_OPTIMIZED` | |
| **`ddCaps.dwCaps2` has no `DDCAPS2_CERTIFIED`** | `testb $0x1,0x68(%ebx); jne fail` — this is the one we were failing |
| `dwNumModes > 0` implies `lpModeInfo != NULL` | |

And two rules from the builder itself, which are the 9x statement of the
caps rules doc 15 learned the hard way on NT:

- **`DDCAPS_BLT` requires a `Blt` callback** *and* requires SRCCOPY
  (ROP 0xCC) to be set in `ddCaps.dwRops` — claiming the cap with either
  missing throws the whole HAL away, exactly as a claimed cap without its
  callback does to dxg.
- **`DDSCAPS_OFFSCREENPLAIN`, `_OVERLAY`, `_TEXTURE` and `_ZBUFFER` each
  require their `vmiData` alignment** to be non-zero *and even*.

**What was ruled out on the way**, each by a boot of its own, so that no
one repeats them: `DDHALINFO_ISPRIMARYDISPLAY` (missing at first — it is
correct to set and it is not what was wrong), `DDHALINFO_MODEXILLEGAL`
either way, `dwHALVersion` as `DD_HAL_VERSION` or `DD_RUNTIME_VERSION`
(DirectX 6.1 takes both), the reference driver's whole rich caps set,
`dwVidMemTotal`/`Free` left zero, and `EmulationOnly` in the registry
(absent). The bisection they were run with is the 9x half of
`-device d3dpt-vga,ddflags=N`: the `.drv` reads the same DDFLAGS register
the NT core does, in the **high half** so the two can never collide
(`D9F_*` in `w9x/d3dpt9x.h`), and `tools/win98-driver-test.sh` takes
`DDFLAGS=`. One bit is kept, `D9F_CERTIFIED`, which puts the bad bit back:
a failure this silent at both ends is worth being able to reproduce.

**What the fix bought, measured in the guest** (`ddprobe`, on the
`test98` machine): `HAL dwCaps 0x00000480` — our own `DDCAPS_GDI |
DDCAPS_BLTQUEUE`, where it had been `DDCAPS_NOHARDWARE` alone —
`132 972 544` bytes of video memory total and free, which is the heap
the `.drv` published; a primary surface whose caps are
`DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM` rather than
`DDSCAPS_SYSTEMMEMORY`; and a 64x64 `DDSCAPS_OFFSCREENPLAIN |
DDSCAPS_VIDEOMEMORY` surface — the request the HEL cannot satisfy at all
— created and locked. DirectDraw is allocating out of the adapter.

**Still open when this was written, and answered in §22:**
`WaitForVerticalBlank` answers `E_NOTIMPL` and the DLL's callback is
never entered. It turned out not to be about the vertical blank at all —
no callback the DLL publishes is entered, for two reasons, and the HAL
accepted here was accepted with an empty callback table.

The harness grew two things in the same session, both of which this cost:
a `boot` re-stages `d3dpt9hl.dll` and `PROG` as well as the two Watcom
binaries, so an edit-build-test cycle on the DirectDraw half is one boot
rather than a whole `install`; and it deletes `C:\DDPROBE.LOG` from the
image before booting, because a stale log read back after a run that
wrote none is a session spent on the wrong evidence.

### 22. Where the 32-bit HAL has to live (2026-09-08)

§21 got the HAL accepted and left one thing open: `WaitForVerticalBlank`
answered `E_NOTIMPL` and the DLL's callback was never entered. Publishing
two more callbacks — `CanCreateSurface` and `CreateSurface`, both
log-and-decline — answered it: **no callback the DLL publishes is ever
entered**, so it was never a vertical-blank quirk. Two separate causes,
one hiding the other.

**The first is a near pointer.** The tables live in the shared block, so
`&cbDD.WaitForVerticalBlank` is a far pointer; `d3dpt9dd.c` is compiled
in the small model, where a plain `DWORD *` is a *near* pointer. The
`*(DWORD *)&…` that stored each 32-bit callback address therefore
truncated the far pointer to its offset and stored through DS, into the
driver's own data segment. The slots stayed zero.

From outside, that reads as success at every step: `dwFlags |= flag` goes
through the far struct properly, so the driver logs the callbacks as
published and DirectDraw sees the flags; and DirectDraw's validator only
`IsBadCodePtr`s entries that are *non-zero*, so a table of nulls sails
through. §21's accepted HAL was standing on exactly that — the video
memory and the mode list were real, the callback table was empty.

**The second is the address space, and it is the real problem.** With the
store fixed the entries are genuine — and DirectDraw then refuses the
whole HAL. Following it back through the runtime, with the probe asking
the same questions from inside the guest:

- **DirectDraw loads the 32-bit HAL and calls `DriverInit` in
  `DDHELP.EXE`**, once for the machine, not once per application. The DLL
  logs its own `GetModuleFileNameA(NULL)` and that is the answer.
- **Every application then validates the stored HALINFO in its own
  address space.** `IsBadCodePtr` on each published entry, and the whole
  driver object is thrown away if one fails.
- **A DLL in the private arena has a different address in every
  process.** Measured: DDHELP had it at `0x00b50000`, and the probe's own
  `LoadLibrary` of the same file got `0x00ca0000`; the probe's
  `GetModuleHandle` was NULL beforehand, so the application does not have
  it at all. DDHELP's callback addresses are bad pointers everywhere else,
  which is precisely what `IsBadCodePtr` says.

So the ring-3 HAL must live in the **shared arena above 2 GiB**, where one
address means the same thing in every process — which is why the
reference driver bases `vmhal9x.dll` at `0xB00B0000`. **And this Windows
98 will not put it there.** `--image-base` at `0xB3D00000`, `0xB00D0000`
and `0xB00B0000` — the reference's own value — were all relocated to
`0x00b50000`; clearing `--dynamicbase` / `--nxcompat` and building as a
GUI subsystem image changed nothing; and removing the relocation table
outright, so the loader must honour the base or fail, made `LoadLibrary`
fail (no `d3dpthal:` line at all, which is at least loud). How the
reference gets its DLL mapped there is the question the next session
starts on.

**Where that leaves the driver.** The far-pointer store is fixed, because
it is a real bug whatever happens next. But a callback that cannot be
reached costs the *whole* HAL — the video-memory heap and the mode list
with it, which do work — and buys nothing, so the 16-bit half now
withholds any `cb32` entry below `0x80000000` and says so:

```
d3dpthal: relocated out of the shared arena, to 0x00b50000
d3dpt9dd: the 32-bit HAL is at=00b511a0, below the shared arena: callbacks withheld
d3dpt9dd:   dd callbacks=00000000
```

with `dwCaps 0x480`, 126 MB of video memory and both surfaces allocating
and locking, as in §21. A DirectDraw that does its own drawing out of our
video memory is worth more than one that does its own drawing out of its
own, and the reason it is doing so is now a line in the log rather than a
truncated pointer.

**Three smaller things the same session settled.** The DLL links against
`kernel32` on purpose now (`-nostdlib` drops it, and the core's `alloc`,
`free` and performance counter will all come from there). The build fails
a DLL based below `0x80000000`. And `ddprobe` asks the questions this
took: the display mode, a primary surface and a video-memory-only one
each described and locked, and `GetModuleHandle` / `LoadLibrary` /
`IsBadCodePtr` on the HAL itself — the last three being the runtime's own
test, run where the runtime runs it.

### 23. The shared arena solved: IMAGE_SCN_MEM_SHARED (2026-09-08)

§22 ended on a hard blocker: `d3dpt9hl.dll` linked at `0xB00B0000` was
relocated by Windows 98 to `0x00b50000` in `DDHELP.EXE`, making its
callback addresses unreachable in game processes. Forcing the base by
stripping the relocation table caused `LoadLibrary` to fail outright.

**The cause is a PE section characteristic: `IMAGE_SCN_MEM_SHARED`
(`0x10000000`).** The Windows 9x kernel splits the 4 GiB virtual address
space into a per-process private arena (0–2 GiB) and a system/shared arena
above 2 GiB (`0x80000000`–`0xBFFFFFFF`), where mapped pages share the same
linear address and page tables across all Win32 and 16-bit processes. The
loader's rule: a PE DLL based above `0x80000000` is only permitted into the
shared arena if **every section** is marked shared (`IMAGE_SCN_MEM_SHARED`).
If any section lacks this flag, the loader considers it a private module
and relocates it down into the per-process private arena (`< 0x80000000`).
If relocations were stripped to prevent that, the loader has no choice but
to fail `LoadLibrary`.

**Why mingw produces non-shared sections.** GNU `ld` provides `-shared` to
build a shared library (DLL), but provides no switch to set
`IMAGE_SCN_MEM_SHARED` on section headers (unlike MSVC's `/SECTION:...,S`).
Standard mingw-w64 `.text`, `.rdata`, `.bss`, `.edata`, `.idata`, and
`.reloc` sections are all emitted without the shared bit.

**The fix:** A post-link Python step in `guest-tools/build-driver9x.sh`
walks the section table of `d3dpt9hl.dll`, ORs `IMAGE_SCN_MEM_SHARED` into
each section's `Characteristics`, and recalculates the PE `CheckSum` in the
optional header.

**The harness fix that came with it.** `tools/win98-driver-test.sh` stalled
in `mcopy` on the install run: when an image already had driver files
staged from a previous install, Windows or setup had given them the FAT
Read-Only attribute (`R`). Even with `mcopy -o`, `mtools` stops and prompts
`file is read only, overwrite anyway (y/n) ?` on stdin, hanging headless
runs before QEMU is ever spawned. The harness now runs `mattrib -r` across
the target driver and probe paths before each copy.

**What the fix bought, measured in the guest** (`ddprobe.exe` on `test98`):

```
DDPROBE.LOG:
          ddprobe: start
          GetModuleHandle(d3dpt9hl.dll) -> 00000000
          LoadLibrary(d3dpt9hl.dll)     -> b00b0000
            DriverInit b00b1270  IsBadCodePtr 0
          DirectDrawCreate -> 0x00000000
          GetCaps -> 0x00000000
            HAL dwCaps      0x00000480
            HAL dwCaps2     0x00080000
            HAL vidmem      132972544 total, 132972544 free
            HEL dwCaps      0xf4c08241
          GetDisplayMode -> 0x00000000  640x480x32 pitch 2560
          WaitForVerticalBlank (no coop level) -> 0x00000000
          SetCooperativeLevel -> 0x00000000
          WaitForVerticalBlank -> 0x00000000
          WaitForVerticalBlank (end) -> 0x00000000
          CreateSurface(primary) -> 0x00000000
            primary GetSurfaceDesc -> 0x00000000
            primary 640x480 pitch 2560 caps 0x1000c200
            primary Lock -> 0x00000000  lpSurface 0add4ae4  pitch 2560
          CreateSurface(64x64 vidmem) -> 0x00000000
            offscreen GetSurfaceDesc -> 0x00000000
            offscreen 64x64 pitch 256 caps 0x10004040
            offscreen Lock -> 0x00000000  lpSurface 0af00ae4  pitch 256
          ddprobe: done
```

And in the QEMU stderr log:
```
d3dpthal: DriverInit, block at 0x8a7ba000 regs 0x80018000 vram 0x80019000 mode 0x028001e0 bpp 0x00000020
d3dpthal:   in C:\WINDOWS\SYSTEM\DDHELP.EXE pid 0xfffe2541
d3dpthal:   published vblank 0xb00b1090 cansurf 0xb00b11c0 hinstance 0xb00b0000
d3dpt9dd:   dd callbacks=00000033
d3dpt9dd:   hinstance=b00b0000
d3dpt9dd: DirectDraw took the HAL
d3dpthal: WaitForVerticalBlank, flags 0x00000001
d3dpthal: CanCreateSurface, caps 0x00004200
d3dpthal: CreateSurface, caps 0x00004200 count 0x00000001
```

Both in `DDHELP.EXE` and in `ddprobe.exe`, `d3dpt9hl.dll` is at `0xB00B0000`.
`IsBadCodePtr` evaluates to 0 in the application's process. The 16-bit
driver publishes all non-zero callbacks (`dd callbacks=0x00000033`).
DirectDraw accepts the HAL and invokes `WaitForVerticalBlank`,
`CanCreateSurface`, and `CreateSurface` in the 32-bit DLL. The publication
chain and shared-memory model for the ring-3 HAL are complete.

### 24. Step 3 — DirectDraw DDI: Core linking, surface callbacks, flip pacing, and 8 bpp (2026-09-08)

With the ring-3 HAL DLL loading in the shared arena and reachable across all
processes, Step 3 (Win98's M7b) is implemented in full:

1. **Linking the OS-independent core (`d3dpt9hl.dll`):**
   - The DLL now compiles and links all five core source files from
     `guest-tools/src/d3dptvid/core/`: `core_flip.c`, `core_caps.c`,
     `core_surf.c`, `core_ctx.c`, and `core_dp2.c`.
   - Freestanding runtime: provides minimal `memcpy` and `memset` without CRT.
   - The six required `d3dpt_os_*` hooks are implemented in `w9x/d3dpthal.c`:
     `d3dpt_os_alloc` and `d3dpt_os_free` (Win32 process heap),
     `d3dpt_os_ticks` (`QueryPerformanceCounter` / `Frequency`),
     `d3dpt_os_surf` (`DDRAWI_DDRAWSURFACE_LCL` -> `d3dpt_surf_desc`),
     `d3dpt_os_attached` (`lpAttachList` traversal), and
     `d3dpt_os_next_mip` (`DDSCAPS_MIPMAP` traversal).
   - Only `KERNEL32.dll` is imported (7 functions: `GetCurrentProcessId`,
     `GetModuleFileNameA`, `GetProcessHeap`, `HeapAlloc`, `HeapFree`,
     `QueryPerformanceCounter`, `QueryPerformanceFrequency`).
   - **VRAM address translation:** On NT, `fpPrimary` is 0 and surface video
     memory offsets are zero-based. On Win9x, `vmiData.fpPrimary` is the
     linear address `pHal->vram_linear` (`0x80019000`), so all video memory
     allocations have linear addresses. `d3dpt_os_surf` subtracts
     `pHal->vram_linear` for non-sysmem surfaces, keeping `d3dpt_surf_desc.vidmem`
     as the true offset into `core.fb`.

2. **DirectDraw Surface Callbacks:**
   - Declared typed structures in `w9x/ddhal32.h` (`d3dpt_ddhal_destroysurface`,
     `d3dpt_ddhal_flip`, `d3dpt_ddhal_getflipstatus`, `d3dpt_ddhal_getbltstatus`,
     `d3dpt_ddhal_lock`, `d3dpt_ddhal_unlock`, `d3dpt_ddhal_blt`,
     `d3dpt_ddhal_setcolorkey`).
   - Implemented callbacks in `d3dpthal.c` and wired into `h->cb32`:
     - `Flip32`: Paced presentation. When `flip_done(&core)` is false, waits for
       vertical blank with `wait_frame(&core)` if `DDFLIP_WAIT` is specified,
       or returns `DDERR_WASSTILLDRAWING`. Registers moved surfaces and executes
       `d3d_readback` on live render targets. Writes the target's VRAM offset
       to `D3DPT_FB_REG_OFFSET` and calls `flip_issued(&core)`.
     - `GetFlipStatus32`: Checks `flip_done(&core)` against `D3DPT_FB_REG_FRAMES`.
     - `GetBltStatus32`: Returns `DD_OK`.
     - `Lock32`: Performs readback on live render targets before access.
     - `Unlock32`: Marks modified texture or target surfaces via `D3DPT_OP_VRAM_DIRTY`.
     - `DestroySurface32`: Releases surface tracking via `surf_forget` and `D3DPT_OP_VRAM_RELEASE`.
     - `SetColorKey32`: Sets source colorkey via `surf_colorkey_set`.

3. **Flip Chain & VSync Verification in Real Guest (`ddprobe.exe`):**
   - In exclusive fullscreen mode (`DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN`),
     created a complex flipping primary chain (`DDSCAPS_PRIMARYSURFACE |
     DDSCAPS_FLIP | DDSCAPS_COMPLEX`, `dwBackBufferCount = 1`) and retrieved
     the attached back buffer (`DDSCAPS_BACKBUFFER`).
   - Issued five consecutive `Flip(..., DDFLIP_WAIT)` calls:
     ```
     CreateSurface(flip chain) -> 0x00000000
       flipping primary Lock -> 0x00000000  lpSurface 80019000  pitch 2560
     GetAttachedSurface(back) -> 0x00000000
       back buffer Lock -> 0x00000000  lpSurface 80145000  pitch 2560
       Flip 0 -> 0x00000000  dt 6 ms
       Flip 1 -> 0x00000000  dt 14 ms
       Flip 2 -> 0x00000000  dt 16 ms
       Flip 3 -> 0x00000000  dt 14 ms
       Flip 4 -> 0x00000000  dt 20 ms
     ```
   - QEMU stderr confirmed scanout offset alternation in hardware:
     ```
     d3dpthal: Flip curr 0x8adbaae4 targ 0x8adbb630
     d3dpt-vga: scanout offset 0 -> 1228800
     d3dpt-vga: scanout offset 1228800 -> 0
     d3dpt-vga: scanout offset 0 -> 1228800
     d3dpt-vga: scanout offset 1228800 -> 0
     ```
   - Frame pacing against `D3DPT_FB_REG_FRAMES` (~60 Hz) is established.

4. **8 bpp Mode & Hardware Palette:**
   - `d3dpt9x.c`: Handled `bpp == 8` in `ModeOk` (checking `D3DPT_FB_CAP_BPP8`)
     and `ReadDisplayConfig`.
   - In `Enable(GDIINFO)`: Set `dpNumColors = 20`, `dpNumPalReg = 256`,
     `dpPalReserved = 20`, `dpColorRes = 18`, `RC_PALETTE`, and sized
     `dpDEVICEsize` to accommodate 256 `RGBQUAD` color table entries.
   - In `Enable(hardware)`: Set `PALETTIZED` flag on DIB Engine and initialized
     hardware palette registers `D3DPT_FB_REG_PALETTE` (0x400..0x7fc).
   - In `d3dpt9x.c`: Implemented `SetPalette` export (ordinal 22), removing the
     stub thunk from `dibthunk.asm`. Calls `DIB_SetPaletteExt` and converts
     `PALETTEENTRY` (R, G, B) into `0x00RRGGBB` format for `D3DPT_FB_REG_PALETTE`.
   - `d3dpt9x.inf`: Added `MODES\8` entries for 640x480, 800x600, 1024x768, 1280x1024.

