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
  key). The core never keeps the OS's object past the call: a colour key
  set *before* a texture was mirrored is taken off the record at
  registration, one set *after* comes through `SetColorKey`. (It used to
  keep the object as an opaque `void *` and read the key off it at every
  bind, and that pointer outlived its surface: §36.)

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


### 25. Step 4 — the Direct3D DDI on 9x, and the window that was 16 KiB low (2026-09-08)

**Both halves pass, to the same bar XP is held to.** `EBTEST.EXE` — the
DirectX 3 face: `IDirect3D` v1, execute buffers, `PROCESSVERTICES` COPY and
TRANSFORM, clipped and unclipped triangles, `TEXTUREHANDLE` binding and a
colour-keyed texture — reports **5 cases, 0 failed** in a real Win98 guest.
`D3D7TEST.EXE`'s frame, drawn by Windows' own `d3dim700.dll` through our DX7
HAL and executed by DXVK on the host, is **byte-identical to
`d3dpt-dp2-test`'s golden frame**: `0 of 307200 pixels differ, max channel
difference 0`. The runtime enumerates `Direct3D HAL` *and* `Direct3D T&L
HAL`, takes a Z buffer at 16 / 32 / 32+stencil, switches to 640x480x32 from
an 800x600x16 desktop, and runs 300 frames in 5072 ms — **59.1 fps**, which
is the flip pacing of §24 holding under a real 3D load rather than a
coincidence.

**The one bug, and it is the same bug as §19's.** Everything above was
black on the first run — all five `ebtest` cases, including the one that is
nothing but a viewport `Clear`. Every call returned `S_OK`: seventeen
`DrawPrimitives2` calls, a context on the back buffer, textures registered,
`PROCESSVERTICES` bouncing to the runtime exactly as the core's DX3 bounce
protocol intends, and `d3d_readback` returning `DD_OK` every time. The tell
was in the host log rather than the guest's: **not one `ddi:` line**, so no
batch had ever reached the executor.

The 9x layer derived the Direct3D command window from the VRAM size:

```c
core.cmd_offset = h->vram_size - D3DPT_FB_CURSOR_BYTES - D3DPT_SHM_SIZE;   /* 0x03ffc000 */
```

The device places it at `vram_size - D3DPT_SHM_SIZE` (`d3dpt_vga.c`;
`d3dpt-vga: Direct3D executor ready, window at 64 MiB`) — `0x04000000`. The
cursor is not in that sum. So the driver encoded its batches 16 KiB below
the window the executor reads, rang the doorbell, and every submission
succeeded against a header nothing had written. The NT layer has never
derived it — it reads the adapter's own `D3DPT_FB_REG_CMD_OFFSET` — and the
9x layer does now.

**Why this is worth a section of its own:** it is the second instance on
this track of one failure shape — *a value the core needs, re-derived in a
second place instead of read from the one authority, failing silently and
looking like something else entirely*. The first was `DDSCAPS_EXECUTEBUFFER`
transcribed as `0x800` (§19), which killed protocol v9's vertex buffers and
showed up as a single failing `shtest` case. Neither produced a diagnostic
anywhere. When a 9x symptom is "the calls all succeed and nothing is drawn",
suspect an address or a constant the layer computed for itself before
suspecting the logic, and diff the layer against `nt/` — the NT driver is
the reference implementation of every one of these values.

**Three DirectX 6 accommodations were deleted, on evidence.** The step-4
implementation was written against an image with the in-box DirectX 6.1,
and had grown code for that runtime's older DDI generation. Rather than
delete it by inference, each was instrumented with a one-shot log and two
full rendering runs were made (`ebtest`, the DirectX 3 path, which is where
a pre-DX7 interface pointer would appear at all, and `d3d7test`):

- `unwrap_surf()`, which sniffed a pointer to tell `DDRAWI_DDRAWSURFACE_INT *`
  from `..._LCL *` because DirectX 6 passes an interface in
  `D3DHAL_CONTEXTCREATEDATA` / `D3DHAL_SETRENDERTARGETDATA` — its branch was
  never taken; it is a plain cast (`surf_lcl`) now, as on NT;
- `Clear_32`, the pre-`Clear2` CALLBACKS2 entry — never entered; CALLBACKS2
  publishes `SetRenderTarget` alone now, which is what `nt/d3dptdisp.c` has
  always done, and the suspect `dwFillDepth / 4294967295.0f` conversion went
  with it;
- `d3d7test.c`'s DirectX 6 `IDirect3D3` path — unreachable once
  `DirectDrawCreateEx` exists.

This is the code half of the decision in the next paragraph: a driver that
serves two runtime generations is a driver with two untested halves.

**A 2ksbox Win98 machine runs the last DirectX (9.0c).** The in-box
DirectX 6.1 (`DDRAW.DLL` 4.06.03.0518, no `DirectDrawCreateEx`) is an
*older DDI generation* than the one `core/` was proven against on XP, so
serving it meant new code with no XP precedent and no pixel oracle — the
DX7 path of `D3D7TEST` cannot even run there. With 9.0c installed
(`ddraw` / `d3d8` / `d3d9` 4.9.0.904, `d3dim.dll` at the DirectX 7
generation) the guest's runtime is the one XP has, and **the DX8 DDI we
already have serves DirectX 3 through 8 through the runtime's own
translation** — which is the direction the superset runs. Raising our own
DDI to 9 would be the opposite trade: strictly additive, deleting nothing,
and buying only what doc 04's matrix already routes to M4's per-game
`d3d9.dll`. `vmhal9x` reaches the same conclusion for the same reason: DDI
8, "forward compatible with DirectX runtime… allows to run DirectX 9
programs and games".

Two consequences. The desktop and DirectDraw halves are unaffected — they
are GDI and the DIB Engine, and plain DirectDraw worked on 6.1 — so the
honest statement is that stock Win98 gets the accelerated desktop and
DirectDraw, and **Direct3D wants DirectX 7 or later in the guest**. And
§21's HALINFO validation rules, which were reverse-engineered from the
6.1 `DDRAW.DLL`, still hold on the 4.9 one: it accepts our HALINFO with the
D3D globals published, and `ddprobe` reads back `DDCAPS_3D | DDCAPS_COLORKEY`
where 6.1 gave `BLTQUEUE | GDI` alone.

**The DirectX 8 half passes too, and with it step 0's last open question**
(2026-09-08, the same day). `SHTEST` reports **9 cases, 0 failed** — vs 1.1
through a declaration and its constants, from user memory and from a vertex
+ index buffer, a declaration-only shader, `D3DVSD_CONST`, ps 1.1 with a
constant and with a texture, and the FVF path again — with the runtime
reporting `vs 1.1 (96 constants), ps 1.4` and creating the device with
**hardware vertex processing**. `CKTEST` reports **4 cases, 0 failed**: a
`DDPF_PALETTEINDEXED8` texture with its own `IDirectDrawPalette`,
`SetEntries` re-colouring it live, and a source colour key with
`COLORKEYENABLE` on and off. `DXTTEST` creates every format (X8R8G8B8,
R5G6B5, A4R4G4B4, DXT1, DXT3, DXT5) in every pool (DEFAULT, MANAGED,
SYSTEMMEM); the only non-zero HRESULTs in its whole log are the six
`LockRect` calls on `D3DPOOL_DEFAULT`, which must fail, and the MANAGED
readbacks show the red and blue block texels (`0xf800` / `0x001f` — the
display is R5G6B5). Note that `DXTTEST` has no pass/fail line and no stored
XP log to diff against, so it is read against its documented behaviour
rather than against a reference run; the other three have hard verdicts.

`SHTEST` passing is what settles it. Step 0 recorded, from reading
`vmhal9x`, that "DDI 8 works on 9x" and left one question behind: *whether
a driver claiming DDI 8 may leave out the pre-DP2 HAL entries
(`RenderState`, `RenderPrimitive`, `DrawOnePrimitive`, `TextureCreate`)
that NT dropped and `vmhal9x` still implements*. **It may**: ours does not
implement them, and 9x's `d3d8.dll` drives it through `DrawPrimitives2`
alone. Both of doc 15's negotiation traps were right in the 9x layer first
time as well — the HAL-info flag without which the runtime never asks and
silently stays on the DX7 path, and the rule that `dwActualSize` is checked
against the size *inside* the GDI2 header while `dwExpectedSize` still
holds the previous query's, so an answer clamped to the outer size makes
the runtime drop the driver altogether.

So the whole M7c feature matrix now reproduces on Win98 — DirectX 3
through 8, hardware T&L, vs/ps 1.x, palettes, colour keys, the compressed
formats — on a driver whose only difference from XP's is its per-OS layer,
and every one of these passed without a change to `core/`.

**A trap for whoever updates the image next.** Installing DirectX means
booting the machine in the launcher, whose Win98 default is `-vga cirrus`
(`bundle::video_choices`) — so Windows re-detects a Cirrus, rebinds the
display to the in-box driver and drops `D3DPT9V.VXD` from `[386Enh]`.
`[boot] display.drv=pnpdrvr.drv` still looks right, and `[boot.description]`
is where it says `Cirrus Logic 5446 PCI` instead. `win98-driver-test.sh
<image> install` puts it all back; a `boot` would not.

### 26. Step 5 — real titles, and a pointer that lived in the frame buffer (2026-09-09)

The user installed six games into a Win98 machine of their own (`claude98`,
on `-vga none -device d3dpt-vga` with this driver and DirectX 9.0c) and
reported what each one did: Monster Truck Madness fine; NFS Porsche 2000
fine but silent; Total Annihilation fine "but the mouse is glitchy" inside a
skirmish; LEGO Island "graphics glitched and the mouse very glitched";
Crimson Skies glitched; Blood glitched "no matter what screen configuration
I have"; and "lots of glitches changing screen resolutions".

That is the first time this driver has met anything other than our own test
programs, and it found things none of them could.

**The harness first.** `tools/win98-game-test.sh` is the 9x counterpart of
`xp-game-test.sh`: a raw copy of the image, the driver re-staged into it, a
game started from `C:\RUN.BAT` through WIN.INI's `run=` (doc 19 §17 — `run=`
takes a program and drops its arguments, so the batch carries the command
line), the discs attached, a screendump every few seconds, and the ACPI
power button at the end. Two things about it are worth keeping.

It builds **the machine the player builds**, taken from `launcherx
--print-args` on the user's own bundle: `-cpu pentium3`, an SB16 on an
audiodev, and the disc as an `ide-cd` on `ide.1` with that audiodev on it
too. The first run of Total Annihilation had no sound card, and TA put up
*"Error: Sound system initialization failed"* and quit before it drew a
frame — which through the log reads exactly like the display driver failing:
a mode set, a primary created, and then a return to the desktop. A test
machine that is not the machine under test is not a simpler test, it is a
different one.

And it re-stages the driver every run (`NO_DRIVER=1` to leave the image's
own build alone), because the image the user plays on carries whatever build
was installed into it.

#### The pointer must not live in the frame buffer

The DIB Engine draws its cursor **into VRAM** and keeps the pixels it
covered in a save-under; `BeginAccess` / `EndAccess` is the pair that lifts
it out of the way before anything else writes there, and GDI calls them
around everything it draws. **DirectDraw does not go through GDI.** A game
that locks the primary, blits to it or flips a chain writes the frame buffer
with the Engine's cursor still standing in it and its save-under now stale,
and the next mouse move stamps that stale block back onto the screen.

Total Annihilation's log says exactly that: `Lock32 … caps=0x1000c238`,
which is `LOCALVIDMEM | VIDEOMEMORY | VISIBLE | PRIMARYSURFACE |
FRONTBUFFER | FLIP | COMPLEX` — it locks the **visible primary** and paints
it wholesale, every frame. LEGO Island does the same to a 3D back buffer and
uses the Windows pointer for its own interface, which is why *both* of its
symptoms were reported: the smearing is the pointer, and it is over
everything.

No amount of care inside the DirectDraw HAL fixes this. That half is a flat
32-bit DLL and the Engine's exclusion pair is 16-bit code behind a selector
it has no way to call.

So the pointer comes out of the frame buffer altogether, exactly as on XP
(doc 15, "The hardware cursor", register set v4): `SetCursor`, `MoveCursor`
and `CheckCursor` are C functions in `d3dpt9x.c` rather than jumps into the
Engine (`dibthunk.asm` no longer thunks them), a monochrome `CURSORSHAPE` is
converted to a8r8g8b8 into the VRAM the DirectDraw heap already stops short
of, and the device hands it to the host as a cursor sprite. Nothing
composites it into the frame, so nothing can corrupt it — not GDI, not
DirectDraw, and not a mode change.

Three details that are not obvious:

- **`C1_COLORCURSOR` has to go.** It is what asks Windows for a colour
  pointer, and a colour shape arrives in the screen's own format. The sprite
  carries monochrome; the cap is claimed only on an adapter with no sprite,
  where the Engine's software pointer (which does handle colour) is still in
  use. Anything the sprite cannot carry is handed to the Engine rather than
  dropped — a pointer that vanishes is worse than one that can be drawn
  over — and the sprite is taken off the screen first, so there is always
  exactly one pointer.
- **`SetCursor` is the name of two different functions.** win16.h declares
  the Win16 *API* (takes an HCURSOR, returns the previous one); this driver
  exports the display entry at ordinal 102 (takes a CURSORSHAPE, returns
  nothing). The API's declaration is hidden while the headers go by, the
  same trick and for the same reason as `ValidateMode` (§18).
- **No CRT, so no 32-bit multiply.** `(DWORD)y * stride` links against an
  undefined `__U4M`; `MulW` is the helper that exists.

The A/B is in the screendumps: with the driver the image was installed with,
the Windows arrow is composited into every dump; with this one it is in none
of them, and `d3dpt-vga: cursor 32x32 hot 0,0 defined` in the host log is
the sprite taking its place. The desktop, TA's menu and LEGO Island's
Information Center all come up correct. A screendump showing no pointer is
now the expected result on 9x as it already was on XP, and the player
composites the sprite itself when the pointer is grabbed
(`present_guest_frame`'s `composite_cursor`), so a machine without
`seamless_mouse` still shows one.

**What is proven here and what is not.** Proven: the Engine's pointer was in
the frame buffer, TA locks the visible primary every frame, and with the
sprite nothing of the pointer reaches VRAM. *Not* proven: the user's own
symptom, which is inside a Total Annihilation skirmish — this harness starts
a game and watches it, it does not click through menus, so what was
reproduced is the mechanism and not the match. The same caveat applies to
LEGO Island, where the before-picture was never taken.

#### One of the six was never the driver

**NFS Porsche 2000's** silence is not the display driver. Worth recording
that Total Annihilation's sound initialised in this harness with the same
SB16, so the card and its driver are broadly working in that image; the
Porsche silence and the `dxdiag` crash beside it want their own look.
The `dxdiag` crash had it (2026-09-12, doc 20 §5.3): the Portuguese
SB16 driver's wave-in name is 33 characters, DirectX 9's DSOUND.DLL
overruns its 32-byte copy of it, and SETUP's "Sound Blaster 16 device
names" component shortens it through the driver's own registry key.

**Crimson Skies was recorded here as a second one, and that was wrong** —
see §27. A parallel session found a real SafeDisc 1.50 weak-sector bug in
our CD-ROM model and fixed it, and this section originally passed that on as
the answer to the user's report. It is not: they run a patched, no-CD
executable, and their complaint was that it renders badly. The rendering bug
is this driver's and is open.

#### The DirectDraw heap ran 64 MB into the command window

`d3dpt9dd.c` published `fpEnd = vram_linear + vram_size - CURSOR_BYTES`.
The top of VRAM is not free: the command window the Direct3D half encodes
batches into sits at `D3DPT_FB_REG_CMD_OFFSET`, which the device puts at
`vram_size - D3DPT_SHM_SIZE` — 64 MB of a 128 MB aperture — and the cursor
sprite's image sits immediately below *that*. The published heap therefore
ran the whole length of the command window: a DirectDraw surface allocated
up there and the batch ring would have been the same memory. Nothing in the
suite reached it, because the heap starts at 8 MB and no title of the era
allocates 56 MB of surfaces, which is exactly why it survived four steps.

`core/`'s `dd_heap_end()` is this calculation on NT. This layer cannot call
it — that is flat 32-bit code and this is 16-bit — so it reads the same
register. **This is the third time a value re-derived here instead of read
from the one authority has cost this track**: `DDSCAPS_EXECUTEBUFFER` in
step 1, the command window itself in step 4 (§25), and now the end of the
heap. The log line to check is `d3dpt9dd: heap=… ..=…`, whose second number
must be 64 MB below the top of VRAM, not 16 KB.

`dwVidMemTotal` / `dwVidMemFree` were the same mistake in another form: they
counted the command window as free video memory.

#### An 8 bpp mode came up with an uninitialised palette

`Enable` programmed the adapter's 256 palette registers from the colour
table it keeps past the DIB Engine's PDEVICE — before anything had written
it. That table is *ours*, in the bytes `dpDEVICEsize` was grown by, and the
Engine has only just been told where it is; nothing fills it at that point.
So an 8 bpp mode came up with 256 entries of whatever the PDEVICE
allocation happened to contain, and only corrected itself when something
realised a palette. Anything that set the mode and drew without one stayed
wrong — the first Total Annihilation run caught it as a Windows message box
whose greys came out red. It now fills the table with Windows' own default
(the twenty static system colours at the two ends, a 6-6-6 cube between them
and a grey ramp in the spare twenty) and programs the device from that.

#### A full-screen DOS box, and the four calls nobody documents

Blood is the 1997 Build-engine game and it is **DOS**, so what it needs from
this driver is not a DirectDraw path at all — it is a screen switch. When a
VM goes full-screen the main VDD takes the adapter from the display driver's
hi-res mode to VGA and lets the DOS program drive it; while
`D3DPT_FB_REG_ENABLE` is set our device scans out the linear frame buffer
instead, so the program writes VGA memory and none of it reaches the screen.
That is a whole class of title, and it is the one the user described as
glitching "no matter what screen configuration I have".

The main VDD is the only thing in the system that knows a switch is
happening, and the mini-VDD is the only thing that knows about the register,
so the fix is four dispatch entries: `PRE_HIRES_TO_VGA` turns the linear
frame buffer off and `POST_VGA_TO_HIRES` turns it back on, with
`POST_HIRES_TO_VGA` and `PRE_VGA_TO_HIRES` logging so that the order is
readable. Which of the four the VDD calls for a given kind of switch is not
something the DDK headers say — this is what a real switch does:

```
d3dptvxd: hi-res -> VGA
d3dptvxd: hi-res -> VGA done
d3dptvxd: VGA -> hi-res
d3dpt9x: RestoreDesktopMode
d3dptvxd: VGA -> hi-res done
```

All four, in order, with the display driver's own callback in the middle —
`RestoreDesktopMode`, registered through `VDD_DRIVER_REGISTER`, which writes
every mode register including `ENABLE`, so the way back was already
half-built. **With that in, Blood renders full-screen at 640x480 through the
device's VGA core for the whole of its attract demo, and the desktop comes
back afterwards.**

Two runs were needed before the display was reached at all, and both
failures look like the driver:

- `run=C:\RUN.BAT` starts in `C:\`, and a DOS/4GW program's stub looks for
  `dos4gw.exe` in the **current** directory: *"Stub exec failed: dos4gw.exe
  — No such file or directory"*. `GUEST_CMD` takes one CRLF line per line
  now, so a `cd` can precede the EXE.
- Then *"src\sound.cpp(508): Sound Blaster not responding on selected
  port."* — this image's `AUTOEXEC.BAT` has no `SET BLASTER=`, which is what
  every DOS game of the era reads to find the card. The Windows side has
  sound (Total Annihilation initialises against the same SB16); the DOS side
  does not. Guest configuration rather than the driver, and very likely part
  of what the user saw.

**And one way to misread the result, which cost an hour here.** Judged from
an *interim* read of the log, half way through the run, only the first two
lines existed and the screen was still the frozen desktop — the DOS box had
not left its `PAUSE` prompt yet — which reads exactly like a one-way switch
into a black screen with no way back, and the write was backed out on that
reading. It was the run not being over. The screendumps say it plainly once
it is: twenty at 800x600 (the desktop), then thirty at 640x480, every one of
them different from the last. Wait for the run to end, and diff the shots
rather than looking at one.

Two harness bugs came out of the same hour and are fixed: `gw_wait_exit`
takes `[pid] [cap]`, so `gw_wait_exit 90` made 90 the **pid** — `kill -0 90`
on a kernel thread fails for a normal user, the wait returned success
immediately, and the run then blocked in `wait $VM` forever on a guest that
had swallowed the power button. And the run clock counted loop iterations
rather than seconds, so a tick that takes 1.6 s made `RUN_SECS=240` run for
six minutes and every `KEYS=` entry land late.

### 27. Crimson Skies: not the disc, and not Direct3D either (2026-09-09)

§26 recorded Crimson Skies as "not the driver" on the strength of a parallel
session's SafeDisc finding. **That was wrong, and the way it was wrong is
worth keeping.** The install has two binaries: `CRIMSON2.EXE`
(2000-08-27) is the original SafeDisc loader, and `CRIMSON.EXE` is dated
**2025-11-02** and is a no-CD patch the user applied over it. The other
session measured the first; the user runs the second, which never touches
the disc. Both of us had the dates and sizes in front of us. A protection
failure and a rendering failure in the same directory are not the same bug,
and "the game in that folder is fixed" is not a claim anyone had earned.

The user's own words were "the graphics were glitched", and that is what it
is.

#### The A/B

`tools/win98-game-test.sh` with `VGA=cirrus` is the control CLAUDE.md
describes, and on this it is decisive. Same image, same disc, same machine,
one variable:

| | our `d3dpt-vga` + driver | `-vga cirrus`, Windows' in-box driver |
|---|---|---|
| title screen | logo, emblem and all six menu buttons are **solid white** in the correct silhouette | renders correctly — red/silver logo, gold winged skull, gold buttons |
| colours in the dump | 682 | 4150 |

So it is ours — **with one variable that this table does not control.** The
two runs are different display devices with different registry keys, and
nothing established that both desktops were at the same colour depth. That
is two variables in a two-column comparison, and it should have been closed
before the result was called decisive. It is what `gdiprobe`'s depth sweep
exists to settle.

#### What it is not

Three negatives from the instrumented run, each of which removes a whole
area:

- **No Direct3D at all.** `ddi:` count 0, no executor frames, for the entire
  title screen. The DX7/DX8 DDI that §25 proved is not running yet.
- **No DirectDraw blit.** With `Blt32` and `SetColorKey32` logging every
  call, neither fires — nor `Lock32`. The game creates exactly one
  video-memory surface (`CreateSurface, caps 0x00004200` = `VIDEOMEMORY |
  PRIMARYSURFACE`) and nothing else; every other surface is system memory.
- **Not a colour conversion.** The bad pixels read back from VRAM as
  literally `0xffff` — every bit set — while the background beside them is a
  sane `0x4021`. A 5-5-5 / 5-6-5 mix-up or a wrong mask shifts colours; it
  does not saturate every channel of one shape and leave its neighbour
  alone.

What that leaves is **GDI drawing through the DIB Engine into VRAM**, which
is the driver's own path and the one the desktop uses successfully all day —
and that has now been tested and is **not it either**. `gdiprobe.exe` walks
the operations a 2D title of the era composites with, checking each one's
pixels itself with `GetPixel` rather than trusting a screenshot, and at
16 bpp through this driver it reports **15 cases, 0 failed**: a solid
`FillRect`, `PatBlt` BLACKNESS and WHITENESS, `BitBlt` SRCCOPY, SRCAND and
SRCPAINT (each raster op checked on its own so a failure names itself), the
two-pass masked sprite — AND a monochrome mask, then OR the image, which is
what a logo over a background *is* — `StretchBlt`, and `SetDIBitsToDevice`
from 24, 32 and 16 bpp DIBs. All correct.
The shapes and positions are right and only the fill is wrong, which says
the addressing is right and something about the *operation* is not — an
all-ones result is the signature of a raster op or a fill, not of a copy.

The driver's engine setup was diffed against `vmdisp9x`'s `enable.c` field
by field on the strength of that: `deFlags` (`MINIDRIVER | VRAM | OFFSCREEN`
plus `FIVE6FIVE` at 16 bpp), `deBitsPixel`, `deWidthBytes`, `deDeltaScan`,
`deBitsSelector`, the `BITMAPINFOHEADER` (`BI_RGB` with no masks — the
reference does the same, so the 16 bpp header is not the bug), `dpCaps1`,
`dpRaster`, `dpNumColors`. They agree, bar `C1_GAMMA_RAMP` and an 8 bpp
`RC_SAVEBITMAP`. So the remaining candidates are inside what the Engine is
asked to *do* rather than how it was set up.

**A lever that does not work, so nobody tries it twice.** The INF's
`DelReg` clears `CURRENT` and `DEFAULT` and its `AddReg` writes
`HKR,DEFAULT,Mode,,"32,640,480"`, which reads like a way to reset the
desktop depth by reinstalling the driver. It is not:
`win98-driver-test.sh <image> install` on a machine whose device is *already*
bound to this driver re-copies the files and reboots without PnP
reinstalling anything, `CURRENT` survives, and the desktop comes back up at
800x600x16 exactly as before. Changing the depth wants
`ChangeDisplaySettings` from inside the guest — which is what `gdiprobe`
should do for itself.

#### The depth bisect, and why it did not answer

`setbpp.exe` sets the desktop depth from the batch file
(`SETBPP 32` before the game), and it works: *"before 16 bpp, asking for 32
… ChangeDisplaySettings -> 0 (ok) … after 32 bpp"*, with
`linear mode on (800x600x32 pitch 3200)` from the device to confirm it. The
title screen came out white anyway.

**That does not mean depth is not the axis, and reading it that way was a
mistake made here first.** The device log from the same run has
`linear mode on (1024x768x16 pitch 2048)` a few hundred lines later, through
GDI's `Enable (hardware)` path — *the game sets its own mode, and it picks
16 bpp whatever the desktop was doing*. So the run changed the depth the
game was started from and not the depth it rendered at. What was actually
established is narrower: the **desktop's** depth does not matter, because
the game overrides it.

#### Where it stands

Eliminated, each by measurement rather than argument: Direct3D, the
DirectDraw blit callbacks, colour-format conversion, GDI through the DIB
Engine (57 probe cases at 16 and 32 bpp, including screen readback and a
screen → memory → screen round trip; the only failures are at 8 bpp and are
the probe's own palette quantisation, not the driver's), and the engine
configuration itself.

What is left is the one path none of that instrumentation covers: the game
creates **exactly one** video-memory surface, the primary, keeps everything
else in system memory, and DirectDraw can satisfy a lock on the primary from
`vmiData.fpPrimary` and `lDisplayPitch` without calling the driver at all.
So the next step is to make that path visible — and to run the game at
1024x768x16, which is the mode it actually renders in and which no run has
yet captured a frame of.

### 28. Crimson Skies: it was Direct3D after all, and the bug was the executor's (2026-09-09)

§27 eliminated Direct3D on a run whose device log had no `ddi:` line for the
title screen. The next session found the opposite on the same image and
the same disc: the **main menu** (the screen with the logo, the winged
emblem and the six buttons — the one §26's table calls the title screen)
is drawn by Direct3D, about 57 textured quads a frame through the DX7 HAL,
FVF `0x1c4`, eight 256×256 A4R4G4B4 textures bound one at a time as
`tss 0.0`. The opening splash before it is not, and a run that stopped
there saw no `ddi:` at all. The §27 eliminations were made on the wrong
screen, and everything they concluded about GDI and the primary surface
is moot for this bug.

#### The diagnosis, and what it was not

Two sessions worked it as a texture problem — staleness (`D3DPT_DDI_REREAD=1`:
no change), the VRAM → host upload (a readback of DXVK's own copy showed the
crimson logo intact), the bind path (every `SetTexture` succeeded). None of
those was it, and the `D3DPT_DP2_TRACE` frame had the answer in its first
lines all along: the state snapshot at the frame start reads
`tss 0: 1=0x3 … 4=0x4`, i.e. `COLOROP = SELECTARG2` — the diffuse, which
every vertex carries as `ffffffff` — while `ALPHAOP` used the texture. A
white silhouette with the texture's alpha is exactly that, not a texture
gone wrong.

Why the colour op was `SELECTARG2` is in an earlier run's full trace (the
context's first DP2 calls): the game sets the DirectX 5
`TEXTUREMAPBLEND` render state (`rs 21 = 2`, `MODULATE`) **with no texture
bound**, then its own `COLORARG2` and `ALPHAARG2` (`tss 0.3` / `0.6` =
`DIFFUSE`), and from then on only binds textures per draw. The executor's
legacy-blend emulation (doc 15 "The white menu text") answered the first
with "no texture: the diffuse alone" and re-evaluates on each bind only
while its `legacy_blend` flag is set — and that flag was cleared by *any*
stage-0 state 1–6, the two ARGs included. So the blend was decided once,
without a texture, and never again. GTA 2 had shown the first half of this
on XP (doc 15); Crimson Skies shows that an argument is not an op. The
executor keeps two flags now, one per op, ended only by the app's own
`COLOROP` / `ALPHAOP`; `tools/d3dpt-dp2-test.cpp` has the sequence and
fails on the old executor with both cells at the diffuse.

**Verified in the game the same day**: driven headless to the menu on the
rebuilt executor, the traced frame (23 draws; the earlier 57 counted a
transition) has the red and silver logo, the gold winged skull and the
six gold buttons over the photograph, and its snapshot reads
`tss 0: 1=0x4` — `MODULATE` — where the broken run's read `0x3`
(`build/w98game/crimson-menu-fixed.png` on this box).

This is host-side, in `d3dpt/exec/d3dpt_exec_ddi.cpp`, shared by XP and
9x; nothing in the 9x driver was wrong, and the §27 A/B against Cirrus was
right for the wrong reason (Cirrus has no 3D, so the game takes a different
path there).

#### Driving the game headless (what the two sessions learned)

`tools/win98-game-test.sh` with `GUEST_CMD=$'cd C:\\ARQUIV~1\\MICROS~1\\CRIMSO~1\r\nCRIMSON.EXE'`,
`CDS=` the `C_SKIES.cue`, a large `RUN_SECS`, `TRACE=1`, on a copy of the
user's `claude98` machine. The game opens a *Select Video Device* dialog on
the desktop, whose OK button is at 496,302 on the 800×600 desktop: the
machine has no tablet, so `qmpc.py relclick x y` walks the PS/2 mouse there
in paced steps reading the position back from the adapter's cursor
registers (`CLICKS=` uses it unless `TABLET=1`). Then the splash, and
Space a couple of times to the menu; touch `frames/trace.on` there. Each
run has its own QMP socket at `OUT/qmp.sock` now — a shared name under
`/tmp` was unlinked by the other run's QEMU on exit.

#### Still open: the QUIT button's bottom half (found the same day)

With the white fixed, the menu is right but for one thing: the bottom-most
button (QUIT) renders only its **top half**. The cut is at the quad's own
vertical midpoint, not a screen row — the button spans y 527–569 and paints
527–~549, stable across every captured frame (so not a mid-animation
capture). What it is **not**, each checked:

- **Not the texture.** Tex handle 37 is a full opaque rounded pill (alpha
  rows 0–41), and the quad's UV covers all of it (0.0–0.164 → texels 0–42).
- **Not a clip.** A temporary per-draw probe (GetScissorRect /
  GetViewport / GetRenderTarget) reported scissor 0,0..800,600, scissor
  test off, viewport 0,0 800×600, RT 800×600 for every draw including these.
- **Not depth or culling.** ZWRITE is off for the whole button block, ZFUNC
  GREATEREQUAL against a near-cleared Z, so nothing is depth-rejected; the
  bottom mechanical panel (drawn earlier, same z, rows 511–599) paints
  fully to 599. CULLMODE is NONE.
- **Not the geometry.** All four fan vertices, logged, are a clean full
  rectangle (429–498 × 527–569 for the right half, uv 0.5–0.77 × 0.0–0.164),
  z 0.9, rhw 0.00361 — identical in shape to CREDITS above it, which fills.
- **Not the executor in isolation.** A `tools/d3dpt-dp2-test.cpp` case that
  draws the same 4-vertex TRIANGLEFAN quad (opaque texture, alpha blend,
  ZWRITE off + GREATEREQUAL over a near-cleared Z) fills top and bottom
  equally. So the fan decode and DXVK's rasterisation of one such quad are
  right; the defect needs the guest's own batch to appear.

So it is a guest-batch-specific interaction not yet explained. The QUIT
quads are the last two textured fans (vertices 80–87) of a 92-vertex,
~23-draw single DP2 call. Repro: the driver script kept in
`build/w98game/CRIMSON-INVESTIGATION.md`, a `D3DPT_DP2_TRACE` frame at the
menu, and the per-draw `.ppm` dumps read at rows 527–569. Distinct from the
fixed legacy-blend bug and cosmetic (one button's bottom edge).

#### Measured: which driver files Win98 locks while running (2026-09-09)

Reinstalling the driver over the running one is not a rare case, and Win98
does lock the files that matter. Probed on `claude98` with the driver
active (`d3dpt9x: adapter found`, `linear mode on`), by copying each file
to a backup and then trying to copy it back over the live one:

| file | overwrite the running copy |
|---|---|
| `D3DPT9X.DRV` (16-bit display driver, held by GDI) | **fails — locked** |
| `D3DPT9V.VXD` (VxD, held by the VMM)               | **fails — locked** |
| `D3DPT9HL.DLL` (ring-3 HAL, loaded per game)       | succeeds — writable |

So the intuition that "9x lets you overwrite any running module" is only
true for an ordinary on-demand DLL (the HAL, which the probe overwrote
fine). The display `.DRV` and the statically-loaded `.VXD` — the two files
`SETUP` copies into `WINDOWS\SYSTEM` — are held open, and a plain reinstall
fails the copy. That is why `copy_one` schedules a boot-time replace for a
locked system file (`WININIT.INI [rename]` on 9x, `MoveFileEx` on NT): on
9x it is needed for the display driver itself, not just as an NT nicety.

**And the one that succeeds is the dangerous one** (2026-09-12, user report:
`SETUP /ALL` over the installed driver blue-screened at the restart prompt).
The HAL's overwrite going through is not 9x being permissive, it is 9x not
holding the file — while the module's pages are still demand-paged from it,
the way a `.DRV`'s discardable segments are reloaded from theirs. A module
whose file has been replaced underneath it runs the new build's bytes at the
old build's addresses the next time a page or segment comes back in. So
`SETUP`'s 9x driver step (`stage_set`) no longer overwrites any driver file
that is already there, locked or not: all seven are staged beside their
targets as `NAME.EX_` and swapped by WININIT on the restart. The doubled
`/ALL` in `tools/setup-guest-test.sh` guards it.

### 29. Leaving a DOS box, the blue screen, and the pointer over both (2026-09-09)

Three reports from the same afternoon on `claude98`, all on the driver as
§26 left it: Blood "hangs with the whole screen glitched when exiting it",
"a big mouse cursor stays over the screen" while Blood runs, and "BSODs
don't show up". They are three ends of one fact: §26 taught the adapter to
give its scanout back to the VGA core for a full-screen DOS box and to take
it back afterwards, and nothing else in the chain had been taught anything.

#### The exit was not a hang: nobody repainted the desktop

Reproduced headless with `tools/win98-game-test.sh` (Blood quit from its own
menu, 300 ms key presses — the game polls the keyboard once a frame and
under TCG a 60 ms tap is lost; `QMPC_HOLD=` in `tools/qmpc.py`). The VxD's
log on the way back is exactly the §26 sequence — `VGA -> hi-res`,
`RestoreDesktopMode`, `VGA -> hi-res done`, `linear mode on (800x600x16` —
and the screen after it is Blood's last 640×480×8 frame tiled two and a
half times across an 800×600×16 desktop, with the VGA text planes as a
green band across the top, for as long as anyone waits (130 s measured).
`info registers` twice showed the vCPU in ring 0 at `HLT`, EIP moving,
nothing pending in the PIC, and the ACPI button powered the machine off in
five seconds. Windows was idle and healthy behind a screen nothing had
asked it to redraw.

The four mini-VDD calls put the *adapter* back. What puts the *desktop*
back is an older channel the main VDD keeps with the display driver, one
the DDK sample and `vmdisp9x` (`scrsw.c`, `sswhook.asm`) both implement and
this driver did not: **INT 2Fh AX=4001h** (`SCREEN_SWITCH_OUT`) raised in
the Windows VM when the screen is about to be taken away and **AX=4002h**
(`SCREEN_SWITCH_IN`) when it is back. A display driver hooks the vector in
`Enable`, and:

- on the way out it sets `BUSY` in the DIB Engine's PDEVICE, so GDI stops
  drawing. That matters more on this adapter than on most: the desktop's
  frame buffer and the DOS program's VGA memory are the same bytes from
  VRAM offset 0, so a repaint arriving during the game lands in the game's
  screen;
- on the way back it restores the mode if the PDEVICE is still `BUSY`
  (clearing the flag) and calls **USER's screen-repaint entry, ordinal
  275** — undocumented, obtained with `GetProcAddress`, and the thing every
  window's WM_PAINT after a full-screen session comes from. `DISPLAY.500`
  (`UserRepaintDisable`) is USER's way of saying "not now"; a repaint that
  arrives while it says so is delivered when it stops.

The handler is in `dibthunk.asm` (`_SWHook`), the callbacks and the hooking
in `d3dpt9x.c`. Two mechanics worth having written down: the saved previous
vector lives in the *code* segment so the chain needs no DS, and code
segments are read-only, so it is written through a writable alias from
KERNEL's undocumented `AllocCStoDSAlias` (KERNEL.170, imported by ordinal in
the `.lnk`) — the DDK's own idiom; and the driver has one code segment
(`_TEXT`, `preload fixed`), so the asm calls the C callbacks near and the
alias is of the segment `SWHook` itself is in, which is how §13's empty
`_TEXT` trap is avoided rather than met again. With the hook in, the VxD
log reads `switched out` just after `hi-res -> VGA done` (the VDD switches
the adapter first and tells the display driver second) and `switched in`
just after `VGA -> hi-res done`, and ten seconds after Blood's Quit the shot
is the desktop, icons and taskbar, where before it was Blood's frame for as
long as the run lasted.

#### The big pointer: a sprite over a screen that has no cursor

The Windows pointer is the adapter's cursor sprite since §26, and the
player, on a machine without `seamless_mouse` (the PS/2 mouse, the pointer
grabbed — `claude98` is one), composites it into the guest frame itself.
While Blood ran the guest frame *was* Blood's VGA frame, 640×480 or
320×200, and the sprite went on being composited into it at the desktop's
coordinates and scaled with it — the "big mouse cursor". Nothing on the
guest side turns the sprite off across a screen switch, and nothing should
have to: a VGA screen has no hardware cursor. `d3dpt-vga` now reports the
sprite hidden to the console whenever `ENABLE` is off (`fb_cursor_move`
gates on `r_enable` and is re-run by every `ENABLE` write), and a reset
drops the shape. XP's full-screen console and both families' blue screens
get the same treatment for free.

One rule learned the expensive way the same evening: **never
`dpy_cursor_define(con, NULL)`**. QEMU's console takes a reference on the
cursor it is handed (`cursor_ref`, an unconditional `c->refcount++`), so a
NULL is a SIGSEGV in whatever process the device lives in — the user's
player, on the first Win98 restart after the reset started clearing the
shape (the core's stack: `cursor_ref` ← `dpy_cursor_define` ←
`d3dpt_vga_reset`). The original `CURSOR_DEFINE = 0` path had the same
call and had simply never been taken. "No cursor" to the console is a
hidden 1×1 transparent one (`fb_cursor_clear`); `tools/win98-game-test.sh`
with `GUEST_CMD='RUNDLL32 SHELL32.DLL,SHExitWindowsEx 2'` is a Windows
restart on the device, and the check is that QEMU is still there for the
second `linear mode on`.

#### The blue screen is drawn by the VDD, and it tells the mini-VDD first

A 9x blue screen — a fatal exception, a "Windows protection error", the
Ctrl+Alt+Del screen, "It is now safe to turn off your computer" — is
*message mode*: the VMM stops the world and the main VDD programs VGA text
mode **itself**, with no int 10h and no display driver drawing. How the
adapter is told turned out to be two things, and only one of them was
guessed right. **Measured** with a fatal exception in a VxD (below): the
VDD takes the ordinary road — `switched out` (the INT 2Fh notification),
`hi-res -> VGA`, `hi-res -> VGA done` — so §26's `PRE_HIRES_TO_VGA` hook is
what clears `ENABLE`, and the blue screen has in fact been visible since
that hook landed in the morning; the reports of invisible ones are from
the days before it, plus the Blood exit above, which looked like one. The
DDK's other door, `SAVE_MESSAGE_MODE_STATE` (function 45; `ddk9x/minivdd.h`
has the list, and its numbering past 43 is not the one older write-ups
give — 45 is *not* `TURN_VGA_OFF`), the VDD calls **once at boot**, before
the desktop's mode is set, and by its description for message screens that
cannot go through a VM switch; `d3dptvxd.c` answers it by clearing `ENABLE`
too, which is harmless at boot and the right thing whenever it is used for
what its name says. §15's archaeology — reading the text page out of VRAM
after the fact — stays in the harnesses, because it is also how a
*continued* blue screen is proved to have happened, but it is no longer how
a person at the window finds out.

**Not every blue screen takes that road** (2026-09-13). The patch-44
corruption of 2026-09-12 (docs/00-status.md) put up exception screens from
VTDAPI's timer event, and every one of them was invisible again: the VDD
drew the text screen without a screen switch, and the mini-VDD was told
nothing — no `PRE_HIRES_TO_VGA`, no `SAVE_MESSAGE_MODE_STATE`, no INT 2Fh —
so the adapter scanned out the frozen desktop over the message. A fault
outside any VM's own execution (an event, a timer callback) is that kind.
What every message screen does send is the VMM's own control message to
every VxD: `Begin_Message_Mode` (0x10) before the VDD programs text mode
and `End_Message_Mode` (0x11) after the key. `d3dptvxd.c` now answers both:
the first reads `ENABLE`, keeps it and clears it; the second sets it again
only if the first found it set — after a screen switch the linear mode is
already off there and the VDD's own `VGA_TO_HIRES` is the way back, and at
boot or for "it is now safe to turn off your computer" there is nothing to
put back. The test is `WHEN=event tools/win98-bsod-test.sh`: `BSOD.EXE
C:\BSODTMR.VXD` loads `bsodvxd.c` built with `-DBSOD_TIMER`, whose init
arms a one-second global time-out and whose callback executes `ud2`
(W32_DEVICEIOCONTROL answers `DIOC_OPEN` with 0 and `BSOD.EXE` holds the
handle, or the VxD is unloaded before the callback runs); `NO_DRIVER=1` is
the control on an image whose VxD predates the hook.

`tools/win98-bsod-test.sh` is the guard: it blue-screens a copy of the
image on purpose. `RUN.BAT` runs `BSOD.EXE` (`w9x/bsod.c`), which loads
`BSODVXD.VXD` (`w9x/bsodvxd.c`) through the `\\.\<path>` door — a dynamic
VxD of ours, forty lines, whose `Sys_Dynamic_Device_Init` executes `ud2` in
ring 0, so the VMM puts up "exception 06 in VxD BSODVXD(01)". Two things
were tried first and are kept as a note: the famous `C:\con\con` IFSMGR
fault, which this image turns out to be patched against (the Win32
`CreateFile` came back with an error and the program lived), and the same
path from the DOS box itself, which only ends the DOS box with an "illegal
operation" dialog — the VMM terminates a V86 VM that faults and reserves
the blue screen for a fault in the Windows VM. A trigger a test depends on
has to be something we ship. The test requires the adapter to have left the linear mode for it
(`hi-res -> VGA` or `message mode`, then `linear mode off`, all after the
desktop's first `linear mode on`), a screendump meanwhile that is mostly
the screen's blue — measured 2026-09-09: *"Ocorreu um erro fatal 06 em
0028:C14E78B9 no VXD BSODVXD(01) + 00000059 … Pressione qualquer tecla para
continuar"*, white on blue, in a headless screendump — the VRAM text page
naming a VxD or the `0028:` selector, and, after a key, `linear mode on`
again, a last screendump that is not blue, and a clean power-off. The mini-VDD also logs
the first few calls of every other notification entry (`vdd fn=…`), which
is how the sequences above were read rather than guessed: around the four §26
entries of a full-screen switch the VDD also calls `RESTORE_REGISTERS` (9),
`ACCESS_VGA_MEMORY_MODE` (11), `ENABLE_TRAPS` (13) and
`MAKE_HARDWARE_NOT_BUSY` (15), and `DISABLE_TRAPS` (14) once the desktop is
back; `SAVE_REGISTERS` (8) and `POST_CRTC_MODE_CHANGE` (29) at the mode set.
None of them needs an answer from us.

**Total Annihilation's "crash on exit"** (the third report of the
evening) did not reproduce: with the pointer walked blind to the main
menu's EXIT and clicked (`CLICKS=80:458,437`; the walk reads the sprite's
registers, and a game that hides the Windows pointer and draws its own
stops them, so `qmpc.py relclick` now goes blind after eight unmoving
steps), the HAL restored the desktop mode, the 800×600 desktop was back
and clean eight seconds later, and the machine powered off on the button.
What was not exercised is an exit from inside a skirmish, which is where
the user plays; the harness would need to drive the skirmish setup first.

`tools/win98-game-test.sh` grew two things for this: `TEXT_AT=<s>` reads
the VGA text page during the run as well as at the end, and a machine that
does not answer the power button is asked `info registers` twice and
`info pic` / `info lapic` before it is killed (`OUT/hang.txt`), the
CLAUDE.md recipe for telling a dead guest from an unrepainted one.

### 30. The blue screen was a heap overrun at the 16→8 bpp switch (2026-09-10)

The user opened Carmageddon on `claude98`, the screen switched to a low
resolution "and then it broke": a blue screen that showed but did not come
back — the desktop returned glitched and unresponsive, only the mouse
pointer alive (which on this driver is the adapter's own sprite, moved at
interrupt time, so it goes on tracking over a dead Windows VM — not
evidence of life). The message was the recoverable kind, *"Ocorreu um erro
fatal … o aplicativo em uso será encerrado"*.

**It reproduced headless on the first try.** `tools/win98-game-test.sh`
with `GUEST_CMD` starting Carmageddon (its "Enter password for uncut
version" dialog takes a single space) blue-screened every run, always just
after `linear mode on (320x200x8`: *"exceção 0E … em VxD VMM(01) … chamada
de … VTDAPI(01)"* one run, `VSB16` the next, KERNEL32 under the game
itself a third — three different callers into one fault, the signature of
memory scribbled somewhere shared rather than a bug in any one of them.
Windows' own Cirrus driver (`VGA=cirrus`) reached Carmageddon's 320×200
main menu in the same run, so the fault was ours.

**The scribble was the 8 bpp colour table, written past the display's
PDEVICE.** `ddprobe.exe` grew a `DDPROBE <w> <h> <bpp> [sys]` mode test —
an exclusive `SetDisplayMode`, a flipping primary (with
`DDSCAPS_SYSTEMMEMORY`, the way Carmageddon asks), a palette, five flips —
and it blue-screened at 320×200×8 with the game's exact signature and none
of the game's code in the way. That named the moment: setting an 8 bpp
mode. `Enable`'s GDIINFO half sizes the display PDEVICE, and it added room
for 256 `RGBQUAD`s of colour table *only when the current mode is 8 bpp*.
But GDI allocates that PDEVICE **once**, at boot, from the desktop's
depth — 16 bpp on `claude98` — and `ReEnable` reuses the same block for
every later mode change. So a game switching the desktop from 16 bpp to
320×200×8 made `Enable`'s hardware half write 256 `RGBQUAD`s (1 KiB) one
kilobyte past the end of a PDEVICE allocated without them, into whatever
VMM/VxD structure the GDI heap put next — hence a fault that surfaced in a
different VxD each run. The fix is one line: reserve the colour table in
`dpDEVICEsize` **for every depth**, so the block is large enough whatever
mode a game later asks for. With it, Carmageddon sets 320×200×8 and the
probe runs its whole flip chain, neither blue-screening (measured
2026-09-10; the same fault, the same string, gone across the game and two
probe variants).

This is the fourth "a value the 8 bpp path needs, missing where the mode
was really decided" on this driver (§13 the thunk, §15 the text page, §24
the uninitialised palette, and now the PDEVICE size), and the second
caused specifically by a boot at one depth and a game at another — the
first was the palette of §26. The desktop's depth is not a spectator: a
16 bpp desktop and an 8 bpp game share one PDEVICE, and everything that
block must hold has to be sized for the deepest colour table, not the
current one.

**And then the black screen was not the palette — it was that the game
had never been meant to get a driver mode at all (2026-09-10, later the
same day).** With the crash gone Carmageddon set 320×200×8, stayed in it
for under a second, restored the desktop mode, and the screen went black
for as long as it was left; the first reading (a system-memory primary
whose frame reaches VRAM but whose palette never reaches the DAC) was
wrong on both counts, and the way it was wrong is the lesson:

- **The "frame in VRAM" was the desktop.** Rendered, the 137-index frame
  read out of VRAM is the Windows desktop repainted at 320×200 through the
  8 bpp halftone palette — GDI's own paint after the mode switch, which is
  what any 320×200×8 VRAM holds for the first second. The game's frame was
  never there.
- **The palette route works.** `ddprobe.exe`'s mode test grew the game's
  own cooperative level (`modex` = `DDSCL_ALLOWMODEX | DDSCL_ALLOWREBOOT`)
  and a `hold`, and on the driver's 320×200×8 mode its screen came up in
  **its own palette's index 0** — solid green — so an `IDirectDrawPalette`
  set on a system-memory primary *does* reach `REG_PALETTE`, through GDI's
  `SetPalette` export. What never arrived was the content: a
  `DDSCAPS_SYSTEMMEMORY` flipping primary on a driver mode is a private
  buffer the runtime flips by pointer swap and presents to nothing — the
  HAL hears of no surface, VRAM stays at the desktop, and the probe's five
  flips changed no byte of it. (The probe's release of that chain then
  never returned, on every run: an open item, below.)
- **Where the game's frame went on Cirrus.** The inbox `cirrus.drv` has a
  320×200 mode and no DirectDraw HAL, so its primary is DCI's — the frame
  buffer through VFLATD, `lpSurface d3ebf000` in the probe's log against
  `00022bd0` on ours — and a "system-memory" primary there *is* the
  screen. That is the whole of the Cirrus difference: not a palette route.
- **What the game was written for.** Its `SSDXStart` (the debug strings
  are still in `CARM95.EXE`, though its log function is compiled to a
  return) calls `SetCooperativeLevel` with `0x53` and one plain
  `IDirectDraw::SetDisplayMode(320, 200, 8)`, then creates
  `PRIMARYSURFACE | FLIP | COMPLEX | SYSTEMMEMORY` (`0xa18`). That is the
  DirectX SDK's **Mode X recipe**: with `DDSCL_ALLOWMODEX`, a 320×200
  request that the driver does not list is answered by the runtime's own
  Mode X, which switches the display driver *out* (GDI `Disable`, the
  mini-VDD's `VDD_DISPLAY_DRIVER_DISABLING`), programs the VGA registers
  and the DAC itself and copies the system-memory chain into planar VGA
  memory on every `Flip` — system memory is the *only* place a Mode X
  surface can live. A 1997 driver with no 320×200 mode gave the game
  exactly that; ours listed 320×200 and 320×240 as driver modes **and**
  set `DDHALINFO_MODEXILLEGAL`, so the game got a real linear mode with a
  primary nothing presents, and even the probe's explicit
  `DDSDM_STANDARDVGAMODE` came back as the driver mode. The reference
  driver lists no 320-wide mode and leaves the flag clear.

**The fix is to stop offering what DirectDraw does better:** the two
320-wide modes are gone from `BuildHalInfo`'s table and
`DDHALINFO_MODEXILLEGAL` is not set. Measured with the game: `SetDisplayMode`
now takes the driver to 640×480×8 for a third of a second (the runtime's
8 bpp GDI surface for Mode X), the display driver is switched out, and the
VGA core reports `cr1=4f cr7=1f cr9=41 cr12=8f sr4=06` — Mode X 320×200,
unchained, double-scanned — with 739 DAC bytes programmed and all of the
first 256 KB of VRAM written; the screendump is Carmageddon's menu and,
after the harness's Esc, its Quit dialog in colour. The probe's
`320 200 8 sys modex` shows its own diagonal ramps the same way (its
`GetDisplayMode` caps carry `DDSCAPS_MODEX`, its primary refuses `Lock`
as a Mode X primary must, and its release and `RestoreDisplayMode` come
back clean), and the game answers the power button, which it did not
before.

**Two things the harness had hidden.** The switched-out screen stayed on
the *previous* frame for 45 s: `d3dpt-vga` holds the last linear frame
for a moment after `ENABLE` goes 0 (so a mode switch's RESET does not
flash the VGA core), and it counted that moment in display refreshes —
250 ms under the player, but a headless console refreshes every 3 s, so
the hold was 15 × 3 s over a Mode X game that was drawing all along. It
is `D3DPT_FB_VGA_GRACE_MS`, wall clock, now. And every `hang.txt` of the
probe runs was misread: `CS=F000 EIP=D40F` in V86 mode is SeaBIOS's
interrupt-stub `iret`, which is where an *idle* Windows 98 spends its
time; a machine that does not answer the power button because a
full-screen DirectDraw process is stuck is not a dead machine.

**Understood, and left as a runtime limitation (2026-09-10):** a
system-memory *flipping* primary in exclusive full-screen on a **driver**
mode hangs the process that owns it — `DDPROBE 640 480 8 sys` is the
repro (320×200 no longer reaches a driver mode at all, it is Mode X now).
It is not our flip: traced, the DirectDraw app thread blocks inside
`Flip`'s `DDFLIP_WAIT` waiting for a completion that never comes, and 52
of 60 external EIP samples are the System VM idling in V86 — the thread is
*blocked*, not busy, so the machine is healthy but the app never returns
and the ACPI power button, which the full-screen exclusive app is holding,
goes unanswered. The 8 sysmem-flip surfaces are the runtime's own
(`DDSCAPS_SYSTEMMEMORY`, guest RAM, never VRAM); the runtime composites
them to the visible primary itself and blocks on its own present. Our
`Flip32` writing the target's system address into the scanout `OFFSET`
register was wrong and is worth noting — it drops the device out of
linear mode — but declining the flip (`DDHAL_DRIVER_NOTHANDLED` for a
`DDSCAPS_SYSTEMMEMORY` target) only moved the block earlier and made it
deterministic, which is the proof the stall is the runtime's, not the
register write's. **No shipped title reaches this:** a 320×200 game gets
Mode X (which runs on the VGA core, driver switched out), and a game at
640×480 uses a *video-memory* primary, which flips through the OFFSET
register and does not block. Left as a known limitation of a
`DDCAPS_GDI` driver under the DirectX 6 HEL rather than chased into the
runtime; `ddprobe.exe`'s `sys`/`modex`/`vga`/`hold` mode test is the
standing repro.

The device now reports the VGA core's mode registers once per change
(`d3dpt-vga: vga core cr1=… sr4=…`) while the linear mode is off, because
which VGA mode a guest programmed after the driver let go — a Mode X, a
DOS game's 13h, a blue screen's text mode — was otherwise invisible in a
headless run.

The probe's log now closes and reopens after every line (`fopen(…,"a")`):
a blue screen a few DirectDraw calls later otherwise left a 0-byte
`DDPROBE.LOG` on the disk, the directory entry's size never written,
because a plain `fflush` hands the bytes to the FAT driver but not the
length to the directory. `tools/win98-game-test.sh` gained UTC timestamps
on every event (its own and QEMU's `-msg timestamp=on`), so a screen
switch shorter than one screendump interval can still be placed.

### 31. 3DMark 99: the emulator walked TB lists, not the driver (2026-09-11)

The user found 3D on Win98 "a bit underwhelming". Reproduced headless on a
raw copy of `claude98` with the user's settings (3DMark 99 Max, 800×600×16,
triple buffer, "Pentium III optimizations", TCG, `-cpu pentium3`): **3334
3DMarks, 10969 CPU 3DMarks** — the user's own run was about the same.

**The host side was idle.** DXVK, RADV and `libd3dpt_exec` together were
under 0.3 % of QEMU: the executor is not where a Win98 3D title's time
goes. **57 % went to TB invalidation** — `soft_imm_absorbs__locked` 29 %,
`tb_invalidate_phys_page_range__locked` 28 % — during the CPU-speed and
fill-rate tests, invalidating nothing: data writes to pages that also hold
translated code, each walking the page's whole TB list twice because patch
15's per-page byte range covered the whole page. Patch 35 replaces the
range with a 64-chunk code map (`patches/qemu/README.md`): **5894 3DMarks,
11648 CPU 3DMarks, +77 %**, the race test from 25 fps to the 60 Hz flip cap
and the first-person test from 4.5 to 13.6 fps.

**The vertical blank is now a real limit.** With it off (`ddflags=32768`)
the unfixed build scored 3435 against 3334: +3 %, because the fill-rate
and texture tests sat at 60 flips/s while the game tests were CPU-bound.
After patch 35 the race test is the one at the cap. The flip pacing is
right for games (doc 15) and stays; a benchmark wanting it off is the
same `ddflags` A/B.

**A finding that did not move the score.** A 10 s profile of the race put
~30 % of the vCPU in the 9x HAL's `memcpy` at `0xB00B32D0` — a C byte loop
GCC compiled to `movsb; cmp; jne`, one trip through a three-instruction
block per byte, on the path that copies every batch into the command
window. XP's `d3dptdisp.dll` had the same loop (`kcrt.c`); the 9x HAL its
own copy of it. Both now link `kcrt.c`, written as `rep movsd`/`rep movsb`
(patch 17's fast path). Measured with patch 35 in: **5898 with it, 5894
without** — the samples in the loop were the walks' cost landing on its
stores, not the loop. Kept because it is the cheaper instruction on every
path and one copy instead of two; recorded so nobody expects a score from
it.

What is left in the fixed run's profile: generated code 40 %, the softmmu
lookups ~13 % (`mmu_lookup1`, `mmu_lookup`, `do_ld4_mmu`), segment loads
2 %, and the single hottest guest block is ring-0 code at `0xC02402F6`
(a VxD; not yet named). Driving it: the scratch `3dm-run.sh` of that
session clicked New Benchmark → Benchmark over a USB tablet
(`tools/win98-game-test.sh` with `TABLET=1`, `GUEST_CMD` = `cd
\ARQUIV~1\3DMARK~1` + `3DMARK.EXE`), waited for the welcome dialog by its
pixels rather than a sleep, and read the score off a screendump.

**The first-person test, and why it is not at 60 yet** (the same day, user:
"I would expect it to also run at 60 fps"). With patch 35 in, the
first-person test (13.6 fps) was **78 % guest code**: dispatch 4.6 %,
softmmu 3 %, helpers 0.9 %, translation 0.6 %, DXVK and the executor 1 %.
Half of the window was one module, `e2_PentiumIII_cpu_mfc.dll` (MAX-FX's
Pentium III geometry, loaded at `0x1580000` — found by matching the hot
guest addresses against every 3DMark DLL at every 64 KiB base; they all
ask for `0x10000000`), an SSE transform loop. Patch 11's inline path was
engaged (`info registers`: guard 0, hand-over 1, helper exits negligible),
so the cost was the quality of the generated code, and it was uneven: a
register-only `addps` 2.7 samples per instruction, the same op with a
memory operand 10.1, a bare `movaps` load 6.1. `-d out_asm -dfilter` of
the loop showed why — every 16-byte operand was two 8-byte loads into a
register pair, two 8-byte stores into `env`, and a 16-byte `vmovdqa` of
the same bytes that cannot be store-forwarded. Patch 36 builds the vector
in registers and stores it once: **CPU 3DMarks 11642 → 13549, the
first-person test 13.6 → 15.4 fps**, every battery identical. What is
left is being taken apart the same way (the M9 track owns the TCG side):
the lane-mask round trip after every inlined SSE op, the 80→64-bit
conversion every block pays on its first x87 use, and the softmmu TLB
check on every integer access.

**Then patch 37, x87 at PC=24.** The first-person test's other quarter was
x87: 3DMARK.EXE's own lighting code (vector normalise with the `0xbe7fffff`
inverse-square-root trick, `fcomps` / `fnstsw` every few instructions)
and the DLL's float loads. At PC=24 each x87 memory op was ~95 host
instructions, and ~15 of them only maintained the inexact flag, which is
sticky and was already set. Patch 37 drops that work in TBs translated with
PE set: **CPU 3DMarks 13549 → 14690, first person 15.4 → 16.2 fps**. The
lesson it cost: a TB flag is also built by patch 20's inline lookup, and a
version without the new bit there was a quarter *slower* — every indirect
jump into the new TBs left through the epilogue. Still far from 60: what
is left is spread across the softmmu TLB check on every access, the
integer code around the geometry, the SSE lane-mask round trip and the
x87 window checks; the user has asked for an opt-in relaxed floating-point
mode for games, which is next.

**Then patch 38, indirect jumps.** With the arithmetic cheaper, MAX-FX's
C++ core (`e2mfc.DLL`, 9 % of the first-person test) showed its shape:
its hottest instructions were `ret` and `call *0x84(%eax)` — virtual
calls — at ~100 host instructions each, a fifth of them rebuilding the TB
flags' mode bits for patch 20's inline lookup. Those bits cannot change
inside a TB, so they are constants now: **CPU 3DMarks 14690 → 15389,
first person 16.2 → 17.1 fps**. Every guest with virtual calls gets it.
Where the frame goes now is flat — the softmmu check on every integer
access, SSE, x87, calls, the rest of the engine, ~4 % of large host
memcpy/memset — and the user has ruled out title-specific work: only
changes that could help any guest.
**Patch 39** gives TCG a way to branch on a vector (`vec_allsign_i32`), so
the lane check after every inlined SSE instruction stops storing its mask
to `env` and loading it back: CPU 3DMarks 15389 → 15940, first person
17.1 → 17.5 fps.

**Host-side copies.** perf could not unwind out of glibc's AVX copy loops,
so a preloaded `memcpy`/`memset` tracer (counting calls of 4 KiB and more
per return address) named them. The executor's DX7-level indexed draw
passed the batch base with `MinVertexIndex = lo` to
`DrawIndexedPrimitiveUP`, and DXVK copies vertices from index 0 up to
`MinVertexIndex + NumVertices`: every draw copied the unused prefix of
its batch, 32 GB in one run on the vCPU thread. Rebased to the touched
range as the DX8 path already was: 0.5 GB, **first person 17.5 → 18.4
fps**, 3DMarks 6014. The tracer's largest item, 110 GB of 128 KiB
`memset`s, is the softmmu TLB wipe on full flushes; a flag to skip unused
MMU indexes (tried as patch 40) changed nothing, because upstream's
`tlb_flush_by_mmuidx_async_work` already flushes only indexes marked dirty
— those wipes are of tables in use, ~2000 a second.

### 32. A read of the whole driver: shared memory, one wrong structure, blits between draws (2026-09-12)

A review of all three 9x binaries and the core they share with XP, each
finding checked against the other side of its boundary before anything was
changed. What was wrong, and is fixed:

**The HAL (`w9x/d3dpthal.c`).**
- **The core's memory came from the calling process's heap** while every
  pointer to it lives in the DLL's data, which §23 made shared: the
  surface table one game grew was still named by the shared pointer after
  it exited, and the next Direct3D process read, wrote and finally freed a
  block of its own address space that its heap never gave out (DDHELP's
  cleanup did the same). `d3dpt_os_alloc` now takes a `HEAP_SHARED` heap
  (9x kernel32 only, `0x04000000`), created on the first allocation, and
  says so in the log if it ever lands below 2 GiB.
- **`DDHAL_GETDRIVERSTATEDATA` had four invented fields** (`ddhal32.h`):
  `GetDriverState32` wrote its `ddRVal` 12 bytes past the runtime's
  20-byte structure and left the real one unset. It is the DDK's layout
  now, the same as NT's.
- **The destroy callbacks made up a surface handle** for a surface that
  had none, from the counter `surf_handle` numbers the DX3-era surfaces
  with — 101 up, the range the runtime numbers its own surfaces in — and
  then released it on the host, which could drop a live texture of a game
  with more than a hundred surfaces. They read the handle the surface
  already has, and release nothing when there is none.
- **The command-window lock** was re-entrant per *process*, so two
  threads of one game both passed as the owner; it is per thread now. A
  game that dies inside a callback dies holding it (`dp2_run` reads the
  application's own pointers), and the runtime's `ContextDestroyAll` for
  that process then waited for ever, and every later Direct3D process
  with it; `ContextDestroyAll32` breaks a lock its process left behind.
  The surface registrations in `Flip32`, `Lock32`, `CreateSurfaceEx32`,
  the destroy callbacks and `LockExecuteBuffer32` now take the lock too.
- **`GetVerticalBlankStatus` never said "in the blank"** — on XP either;
  doc 15, "The flip chain's vertical blank".

**The core (both families).**
- **A blit after a draw is sent in a record of its own.** TEXBLT,
  BUFFERBLT and VOLUMEBLT are copies the guest makes while it walks the
  stream, and the host reads a buffer or texture when it runs the draw, so
  in one record every draw saw the last blit's bytes — a managed vertex
  buffer locked, drawn, locked and drawn again arrives as BUFFERBLT DRAW
  BUFFERBLT DRAW in one call, and both draws came out with the second
  fill. `dp2_run` now sends the stream in stretches, cut before any blit
  that follows a draw; the doorbell runs each on the host before the next
  one's blits happen. A stream with no such blit is one record, as before.
  **No runtime has been seen to send one:** `DRIVER\MGDTEST.EXE` (below)
  refills a managed vertex buffer, a managed texture and — through
  `UpdateTexture`, the explicit TEXBLT — a default-pool texture between two
  draws of one scene, and all three pass on the *unfixed* core too: XP's
  d3d8.dll ends the DrawPrimitives2 call before any blit whose resource a
  draw of that call reads. The cut is kept as what the protocol needs if a
  runtime ever batches them (it costs nothing when there is nothing to cut)
  — a safeguard, not a fix anybody's frame has needed yet.
- **Long non-indexed draws are cut into pieces** the host takes (at most
  0x10000 vertices): the caps allow 0xffff primitives, and a
  `DrawPrimitive(TRIANGLELIST, 0, 30000)` was skipped whole, silently.
  Lists cut on a primitive, strips overlap by their shared vertices and a
  triangle strip's pieces start on an even triangle; a fan still skips.
- Stream 0's stride is bounded at 1024 as the others were — the host
  fails the whole stream on a wider one.
- **TEXBLT's rectangle on mip levels** rounds the right and bottom edges
  up, and a DXT rectangle starts at the block its edge is in (a dirty-rect
  update of a mipmapped texture left stale texels and blocks).
- **A DP2 `SETRENDERTARGET` becomes the context's target**, so EndScene
  reads back the target a DX8 application is rendering to rather than the
  one from `ContextCreate`.

**The mini-VDD (`w9x/d3dptvxd.c`).** The two selectors were one page
longer than their mappings (a page-granular limit is the last page, not
the count). The way back from a DOS box turned the linear mode on
unconditionally — also under the 16-colour drivers the INF hands low
modes to, and after `PhysicalDisable` — and now does only if the switch
to VGA found it on. The notification log's per-entry count was a byte
counted on past its limit, so it wrapped and logged four more lines every
256 calls, for ever.

**The 16-bit driver (`w9x/d3dpt9x.c`).** Two inline-asm VDD calls did not
declare the registers they write (`CallVDD_DriverRegister` CX and BX,
`CallVDD_Simple` whatever the VDD returns in). Latent in this build — the
only change to the generated code is a `push si` in `PhysicalEnable`'s
prologue, read off an `ndisasm` of both `.drv`s — but it was one
register-allocation change from corrupting a mode set. And a `ReEnable`
the hardware half refuses puts the mode globals back, so the next DOS
box's `RestoreDesktopMode` does not program the refused mode at the old
pitch.

**Checked and found not to matter.** GDIINFO's English and twips extents
look wrong (MM_HIENGLISH has no window extent, MM_TWIPS's viewport points
left and down), but Windows 98's GDI does not use them: a mapping-mode
probe on the unfixed driver got every one of the five metric and English
modes right, with window 254 over viewport 96 for MM_LOMETRIC — GDI builds
them from `LOGPIXELS` (96) and reports `HORZSIZE` 211 mm at 800 wide, not
the driver's 208. Left as they are. Not changed either: the runtime's
parser pointer for the DX3 bounce is stored machine-wide from the last
process that asked (right while system DLLs load at one base everywhere);
the VxD agrees to a dynamic unload while the VDD's table points into it
(nothing unloads it); two failure paths of the mini-VDD's mapping leak
arena pages; and VOLUMEBLT still refuses DXT volumes (never seen sent).

**Measured.** On a raw copy of `test98` (`tools/win98-game-test.sh`,
one boot, `RUN.BAT` starting each program with `start /w`): `ddprobe`'s
new `GetVerticalBlankStatus` loop said "in blank" 0 times in 530 887 polls
on the unfixed driver and 30 in 939 575 with the fix; `SHTEST` 13 cases 0
failed, `EBTEST` 5/0, `CKTEST` 4/0, and `D3D7TEST` byte-identical to
`d3dpt-dp2-test`'s frame twice — the second time as a fresh process after
four other Direct3D processes had come and gone, which is the case the
shared heap is for (`d3dpthal: shared heap 0x93027000`, in the shared
arena, and no private-arena line). That case was not run on the unfixed
driver, so it is not a before/after: a stale table can equally land on
memory the next process does not use. `gdiprobe`'s 57 cases fail the same
five at 8 bpp before and after (nearest-palette colours).
`tools/win98-bsod-test.sh` passes all seven of its checks with the
mini-VDD's conditional re-enable: `PRE_HIRES_TO_VGA` turned the linear
mode off, the blue screen was on the screen and in the text page, the
desktop came back after the key and the machine powered off. On XP, the whole
M7 battery on an overlay of `winxp-m7.qcow2` with the new core: `DDTEST`
at every depth (30 "in blank" answers in ~130 000 polls), `D3D7TEST` 0 of
307 200 pixels off the host frame, `SHTEST` 13/0, `CKTEST` 4/0, `EBTEST`
5/0, the nine DX8 probes as before (`PATCHTST` not offered), `D3DGAME8`
0 of 307 200 pixels off the native d3d9 frame, and no `dp2` error, refusal
or skipped draw in any run's log, and `scripts/test.sh all` 45 passed
(its guest stage's D3DGAME9 / D3DGAME8 / D3DFEAT9 among them). The new
`MGDTEST` passes its three cases on both cores (above); no probe has a
90 000-vertex draw, so the long-draw cut is proved not to break the
streams that exist, not yet to fix one that needed it.

### 33. Diablo II's sheared menu: a flip chain at a pitch the screen was not scanned at (2026-09-13)

The user's report was "the Diablo II demo only shows garbled graphics"
(the shareware v1.04, on the launcher's `base98-br`: `d3dpt-vga` with the
Voodoo 2 beside it; the same demo ran on 86Box). The game's own log
(`D2YYMMDD.TXT` in its folder) said the runs had been **DirectDraw** on
our adapter, not Glide — "Initializing for DirectDraw on Fallback
DirectDraw Device", a triple-buffered window at 800x600 — and the
Voodoo's 5 s line said `off: 0 frames`. Headless
(`tools/win98-game-test.sh`, a raw copy, `EXTRA='-device voodoo2,addr=0x05'`)
every screendump was the menu's colours smeared into horizontal streaks.

**The cause.** The mode was 800x600x8, scanned out at pitch 800
(`linear mode on (800x600x8 pitch 800`), and every `Lock` the game made
on its back buffer said **pitch 0x340 = 832**. DirectDraw sizes a flip
chain's back buffers out of the HAL's heap with the pitch rounded up to
the alignment the HAL advertises — every `vmiData.*Align` here was 64 —
while the primary's pitch is the mode's own; the HAL's `Flip` then put a
832-pitch buffer on a screen scanned at 800, and each line of the frame
landed 32 bytes further along than the one before. 800 is the one 8 bpp
width in the mode table that 64 does not divide (400x300 at 8 and 16 bpp
are the others at any depth); 640, 1024 and 1280 hid it. NT never had it:
its miniport's `bpp_pitch` rounds every mode to 32 bytes and `DdGetDriverInfo`
advertises the same 32.

**The fix: one pitch rule for a mode.** `D3DPT9X_PITCH(w, bpp)` in
`w9x/d3dpt9x.h` is the mode's bytes per line rounded up to
`D3DPT9X_PITCH_ALIGN` (64), and it is used by everything that has a pitch:
GDI's `dwPitch` in `PhysicalEnable` (and so the PDEVICE's `deWidthBytes`,
the VDD's registration and the primary's `lDisplayPitch`), the HAL mode
table `SetMode32` programs the adapter from, both "does it fit in VRAM"
checks, and `dwOffscreenAlign` itself, so the rounding DirectDraw applies
and the rounding the screen has are one number. The adapter already took
any pitch of at least width x bytes that is a multiple of 4. 800x600x8 is
scanned at 832 now; 400x300x8 and x16 at 448 and 832.

**Measured**, the same raw copy of `base98-br` before and after: the
unfixed driver shows the streaks from 10 s to the end of a 90 s run; with
the fix the adapter reports `linear mode on (800x600x8 pitch 832`, the
back buffer's locks still say 832, and the menu draws (the fire moves
from one screendump to the next; 125 page flips per 5 s both times). The
same demo with `-3dfx` — Glide on the Voodoo 2 and 3dfx's own
`glide3x.dll` — draws the menu correctly too (`voodoo2: 800x600 on: 125
frames, ~32 000 triangles` per 5 s), so the Glide path never had the
bug.

**Why the game was on DirectDraw at all** — the user had picked 3dfx in
the shareware's `D2VidTst`. It saved the pick (`Render` = 3, Glide) under
`HKCU\Software\Blizzard Entertainment\Diablo II Shareware\VideoConfig`;
the shareware's `Diablo II.exe` reads only `...\Diablo II\VideoConfig`
(the strings in the binaries say so), which here is the retail install's
key, never through its video test: `Render` = 0, DirectDraw. A Blizzard
bug, not ours. Setting the retail key's `Render` to 3 (`regedit /s` from
`RUN.BAT` on a copy) started the shareware on the Voodoo 2 with no
`-3dfx`: `voodoo2: 800x600 on: 125 frames`.

**The guard** is `ddprobe`'s new `pitch check:` line, which compares the
flipping primary's pitch with its back buffer's. `DDPROBE 800 600 8`
(`STAGE=` + `GUEST_CMD=` + `PULL=DDPROBE.LOG` through
`tools/win98-game-test.sh`) on a fresh copy of `base98-br` with the
image's own, unfixed driver (`NO_DRIVER=1`): `primary 800, back buffer
832 -> DIFFERENT`; the same copy with this build staged: `primary 832,
back buffer 832 -> same`, five flips, and `GetDisplayMode` reporting pitch
832.

### 34. Crimson Skies in flight: a Z buffer reset through a Lock (2026-09-14)

The user's report on `base98-br` (the unpatched `CRIMSON.EXE`, the disc in
the drive): the menus have glitches "where it's supposed to be drawing
text", and in flight "almost everything is transparent; some pieces leave
draw trails". It works on the Voodoo 2, slowly. Both are fixed here, and
neither was a rendering bug: one is a Z buffer the host never saw reset,
the other a texture cap the game branches on.

**What a flight frame was.** Headless on a raw copy (`tools/win98-game-test.sh`
with the play disc, the Voodoo 2 on the bus as on the machine, the Select
Video Device dialog answered by `CLICKS=`/`KEYS=` and the rest driven over
QMP — the game reads its mouse through DirectInput, so the adapter's cursor
registers never move and `relclick` cannot aim; raw relative events do, at
about 1.27 pixels a mickey), a `D3DPT_DP2_TRACE` frame at 1024x768: 454
draws, and only the 13 HUD draws (z 1.0, rhw 1.0) changed a pixel. The 440
world draws — z from 0.0 up, rhw ~0.00003, none outside [0, 1] — changed
nothing. The game uses reversed depth: `ZFUNC GREATEREQUAL`, far = 0, near
= 1, and no Clear anywhere in the frame (the executor traces every
`CTX_CLEAR`, and a traced frame runs from one readback to the next, so none
can fall outside it). So something reset depth to 0 that the host never
saw, the host's depth buffer kept the frame before's, and everything
farther than it failed — the black sky with only the HUD in it, and the
trails wherever no draw covered last frame's pixels. The same bug is what
§28 left open as the menu's QUIT button drawing only its top half: the
buttons depth-test GREATEREQUAL without Z writes against a buffer nobody
had reset on the host.

**How the game resets it: by hand.** It Locks the Z buffer
(`DDLOCK_WRITEONLY`) and writes 0 into it, every frame — not a Direct3D
Clear (no `Clear2` call reached the HAL), not a DirectDraw depth fill
handed to the driver (`Blt32` was never called). The guest's own VRAM said
so first: the Z surface read out over QMP (`pmemsave` at BAR0 + its
offset) was 480 000 pixels of `0x00000000`. The `Lock32` log did not,
because it stops after 64 lines and the intro video's back-buffer locks
spend them.

**The fix**, in the shared core (`core/core_ctx.c`, `d3d_z_written`) so
that both families have it: each layer remembers the Z buffer it locked
for writing (`Unlock32` on 9x, `DdUnlock` on XP), and on its Unlock the
core looks at it. If it holds one value, that value — in the surface's
own Z bits, through `dwZBitMask`, turned into the host's [0, 1] with
integers only, because on XP this runs in a kernel-mode display driver
that may not touch the FPU unsaved — becomes a Z-only clear on every
context whose Z buffer the surface is. A read-only lock is left alone (a
title reading its depth, for the sun's occlusion say, must not wipe the
frame's), and so is a Z buffer that is not one value, because there is no
way yet to give the host a depth image. **Not every pixel is read**: every
7th row, whole, and the last — about 15% of the buffer, spread from top to
bottom (user's call, 2026-09-14: more than that much of a screen at one
depth is not a frame anyone draws, and a fill of part of the buffer still
reads as not uniform because every sampled row spans its width). The scan
is the guest CPU's, every frame, so reading all 3 MB of a 1024x768 32-bit
Z buffer was the cost to cut. Measured in the game: the flight draws —
terrain in fog, the wreck burning after the crash nobody was steering away
from — and the QUIT button is whole. The sampled scan was checked the same
way (bridge, city, terrain, the plane drawn), and flew at 17.5 to 34
frames/s where the full scan had flown at 14.6 to 22 — two different
flights nobody was steering, so a hint of the saving rather than a
measurement of it. Z locks are logged separately now (the first 16, then
every 1024th), and the core's decision on each of the first 16.

**What was tried on the way, so nobody tries it again.** A DirectDraw
depth fill (`Blt(DDBLT_DEPTHFILL)`) from an application does not reach the
driver either: claiming `DDCAPS_BLTDEPTHFILL` alone does not route it to
`Blt32` — the runtime still fills through a Lock, which the same `Unlock32`
check now catches — and `DDCAPS_BLT`, which would route it, needs SRCCOPY
and a real blitter (the validator rules above). A depth fill of part of
the buffer therefore reaches the host only if the whole buffer ends up one
value; one that leaves two values is lost (measured: `zfilltest`'s
left-half case failed, and was taken out). That wants a depth-image upload
in the executor. XP's driver had the same gap by construction — no blit
caps at all, so dxg's HEL does every fill, through `DdLock` — and has the
same fix since 2026-09-14.

**The guard** is `ZFILLTEST.EXE` (`guest-tools/src/d3dptvid/zfilltest.c`,
built by `build-driver9x.sh`): Z known to the host at 1.0 from a Clear,
then (A) a depth fill of 0, (B) a depth fill of 0xffff and (D) a Lock that
writes 0, each followed by a quad at z 0.5 under GREATEREQUAL read back
from the back buffer. On `base98-br` with this driver A, B and D pass; with
the image's own driver (`NO_DRIVER=1`) A and D fail — the negative
control. On XP (`winxp-m7`, built by `build-driver.sh` too, as
`DRIVER\ZFILLTEST.EXE`) the same: 3 of 3 with this driver, and the driver
from the commit before (built from `git archive 3fc4dfd` into a scratch
tree and installed with `DRIVER_ISO=` on `tools/xp-driver-test.sh`) fails
A and D. Run it there with `tools/xp-driver-test.sh <overlay> cmd 'cd /d
%TEMP% & D:\DRIVER\ZFILLTEST.EXE & copy zfilltest.log E:\'` and read the
log off `OUT/scratch.img` with mtools. Run it with `STAGE=guest-tools/out/driver9x/zfilltest.exe
PULL=ZFILLTEST.LOG GUEST_CMD=$'cd C:\\\r\nZFILLTEST.EXE'
tools/win98-game-test.sh <image> zf`.

#### The menu text: a texture cap, read before any call

The Instant Action page drew every mission in its list and every name
field (Pilot Plane, Mission, Environment...) as a bar of red, green, blue
and brown blocks, while its labels ("Pilot Plane:", "Table of Contents")
were right. The trace had it in one line per string: each is a quad drawn
with texture handle 165, an **8x8 R5G6B5** texture of a diagonal
four-colour pattern with full alpha — the engine's own "missing texture"
— under UVs sized for a strip of about 131x16 texels (u = the string's
width / 131, v = 15/16), no rebind between strings. The strings' own
textures were never made: not a video-memory one, not even the
system-memory surface the labels are painted into with GDI first. Every
`CanCreateSurface` was followed by its `CreateSurface`, nothing was
refused, the game's own `GAMEZ.ERR` said nothing — so it was a decision
the game made from something the device reports, before any call a
driver could log.

The diff that found it is `DEVCAPS.EXE` (`w9x/devcaps.c`): every
DirectDraw device — with `DDENUM_NONDISPLAYDEVICES`, or the Voodoo 2 is
not enumerated at all — its DDCAPS, `GetAvailableVidMem` per kind, and
every Direct3D device's `D3DDEVICEDESC7` by field and in hex. Against the
Voodoo 2's DirectX 7 driver, on which the user says the menus work, the
two answers a text engine sizes its textures from differed: 3dfx claims
`D3DPTEXTURECAPS_POW2` and 256x256 at most, we claimed any size up to
4096. Two A/B bits (`ddflags`) settled it on the game: both together, the
text draws; **`POW2` alone, the text draws** — and the game makes 3 814
textures on that screen, all powers of two, where it had made 218. So a
device that allows any texture size sends it down a text path that never
makes its strings' textures.

**The fix** (`core/core_caps.c`, both families): the texture caps claim
`D3DPTEXTURECAPS_POW2 | D3DPTEXTURECAPS_NONPOW2CONDITIONAL` — powers of
two, other sizes only clamped and without a mip chain — which is what a
GeForce 2 to 4 or a Radeon claims, rather than `POW2` alone, which would
refuse a non-power-of-two texture to every title that makes one. The host
has no such limit; the claim is for the titles. Measured on the default
flags: the whole page's text is right, and flight draws (terrain, trees,
the plane, the HUD). `ddflags=0x2` (`DDF_TEX_ANYSIZE`) puts the old
answer back and `ddflags=0x20000000` (`DDF_TEX_256`) caps textures at
256x256 like the Voodoo 2 — the two halves of this bisection.

**Also found, and fixed, and not the cause of anything seen.** The DX7 and
DX8 caps published `dwMaxTextureAspectRatio` / `MaxTextureAspectRatio` as
0 (the structure is zeroed and the field was never set); they are 4096 now
and `d3d7test` refuses a HAL that reports less than 8. Microsoft's own RGB
device reports 0 as well, and publishing 4096 changed nothing in the game.
And `D9F_CERTIFIED` (0x01000000, the 9x half of `ddflags`) is the same bit
as the core's `DDF_NO_VOLUME`: whoever sets one sets both.

### 35. The shutdown screen's green band: the linear mode went off too late (2026-09-14)

The user's report: some of the time, turning Windows off showed "the
glitched green top" — the desktop with a bright green band across its
top — where "O Windows está sendo desligado" should be. It is not a
driver that fails to let go: every run's log has the disable and
`linear mode off`, and a QMP screendump after the power-off (QEMU's
`-no-shutdown` keeps the machine paused on its last screen) is the
shutdown logo in the VGA core's Mode X (`cr9=40 sr4=06 gr5=40 gr6=05`),
drawn right. The band lives for a quarter of a second, and only the
**player** sees it: `PLAYER=1 PLAYER_SHOT_EVERY=6 tools/win98-game-test.sh`
saves ten of its frames a second, and 22 ms after `linear mode off` one
of them was the desktop with the band, 800×600, then 300 ms later the
logo. One-a-second screendumps never catch it, and a headless console
refreshes only when asked.

**The band was in VRAM while the linear mode was still on.** A 16 bpp
desktop is shown through the adapter's shadow copy, which only changes
while `ENABLE` is 1, and the held frame (`D3DPT_FB_VGA_GRACE_MS`, §30) is
that copy — so the VGA's bytes had to be converted into it before the
linear mode went off. The log gives the window:
`DISPLAY_DRIVER_DISABLING` (mini-VDD function 26) 12 ms *before*
`linear mode off`. `PhysicalDisable` unregistered from the VDD first and
wrote `ENABLE = 0` after, and the unregister is where the VDD announces
the disable and starts putting the VGA back — writing its planes from
VRAM offset 0, which is the top of the linear frame. A display refresh
that fell in those 12 ms took them into the frame the adapter then held;
one that did not left the desktop clean. That is the "some of the time",
and a machine that powers off inside the hold ends on the band.

**The fix** is the order, twice. `PhysicalDisable` writes `ENABLE = 0`
before `VDD_DRIVER_UNREGISTER`, and the mini-VDD answers
`DISPLAY_DRIVER_DISABLING` itself (`d3dptvxd.c`, `driver_disabling_proc`)
by turning the linear mode off and forgetting any switch or message
screen in flight — the way back from a disable is the driver's own
`Enable`, so nothing may put a stale linear frame over the VGA after it.
The log now reads `linear mode off`, then `d3dptvxd: display driver
disabling` a millisecond later, which also shows the VDD sending function
26 from inside the unregister. Measured with the player on `base98-br`
(twice, one run kept paused by `-no-shutdown`): desktop, the clean desktop
held, the logo — byte for byte the logo of the run before the fix.

Driving it: on a Portuguese Windows 98 `d` in the Start menu selects
"Documentos" before "Desligar…", so the keys are `ctrl+esc`, `up` (the
menu opens with nothing selected and `up` wraps to its last item), `ret`,
`ret`. And a machine with the Voodoo 2 checkbox wants `EXTRA="-device
voodoo2"` in the harness: booted once without the card and once with it,
Windows finds it as new hardware and asks for a restart.
`tools/win98-bsod-test.sh` passes on the change, on `base98-br` — whose
fresh copy boots into a restart prompt that holds `RUN.BAT` back, so it
needs `KEYS="60:ret,150:ret" TEXT_AT=120 RUN_SECS=200` (a key for the
prompt, one for the blue screen); with the default single key at 70 s the
key answers the prompt and the blue screen that follows waits for ever.
||||||| parent of 772fc1a (d3dpt core: keep no OS surface pointer -- 3DMark2001 SE's demo froze after its loading screen)
### 36. 3DMark2001 SE's demo: a freed surface, a swallowed fault, a lock left held (2026-09-14)

The user's report, on `base98-us` with the current driver: the demo froze
on a black screen right after its dragon loading screen, and the QEMU
log's last line was `d3dptdisp: texture 0x00000058 bound` — no page flip
and no `ddi:` frame after it. Reproduced twice, headless, the same line
last both times: `tools/win98-game-test.sh` on a raw copy of the machine,
3DMark started from `RUN.BAT`, Demo and "use the recommended 640x480"
clicked over the PS/2 mouse.

**What the frozen machine was doing.** EIP never moved from `003b:03ce`,
an `int 30h` in the VMM's table of ring-3 callbacks, taken 62 000 times a
second; the ring-3 stack under it was KERNEL32, `DestroySurface32`,
`cmd_lock_acquire`. The command-window lock word was taken, by another
thread, at depth 1 — the spin was `Sleep(0)` waiting for it. Patching that
`push 0` to `push 1` through QEMU's gdbstub made the CPU idle (`HLT=1`)
and left the lock held: its owner was **blocked**, not starved by
priority, which was the first guess.

**What blocked it**, from `log int` over the whole load: the thread doing
the drawing (its stack at `0x00b6xxxx`) took a user read fault in our HAL,
at `b00b3a3d` in `d3dpt_os_surf` — `CR2=10405020`, `ECX=8aedd524`,
`EAX=10405008`. `ECX` was a surface LCL in the shared arena, and the word
where its `lpGbl` lives held a surface's caps: the runtime had freed that
LCL and reused the memory. The caller was `surf_colorkey_check`, which
read the texture's colour key off `SURF::lcl` at every bind, and nothing
ever cleared that pointer when the runtime freed a surface (`surf_forget`
clears `used`, and only when a destroy callback still finds the handle).
The fault came inside `DrawPrimitives2` with the lock held; KERNEL32
dispatched it, a handler in the thread took it (whose, the capture does
not say) — the thread ran 273
more events and then waited for good — and 300 events later the main
thread entered `DestroySurface32` and spun for the lock for ever.

**The fix: the core keeps no pointer to an OS surface.** `d3d_register_at`
and the cube faces take the source colour key off the descriptor they
already read (`surf_key_snapshot`), `surf_colorkey_check` tells the host
at the first bind from the table alone, and a key set later still comes
through `SetColorKey` → `surf_colorkey_set` — the runtime calls it because
both layers claim the colour-key caps. The one thing the old read also
saw, a key taken off a texture without a `SetColorKey` call, is not seen.
The core is shared, so XP gets the same change; there the stale read
would have been in kernel mode. The HAL's other reader, `TextureGetSurf32`
(a DirectX 5 texture handle back to its surface), keeps its own list,
filled in `TextureCreate32` and emptied in `TextureDestroy32`, which did
nothing before: the runtime holds such a handle only while its texture
lives.

**Measured**, the same run with the rebuilt driver staged into a fresh raw
copy of `base98-us`: the demo goes past its loading screen, the
`texture 0x00000058 bound` line goes by once, and the opening scene (the
statue in the rain) presents at 60 frames/s — the flip cap — at 16 000 to
30 000 draws per 5 s, with no batch refused, skipped or failed; three
minutes in the demo is in its Nature scene (trees, grass, the river),
its white frames there being the scene's own fades; it ends by itself
back at 3DMark's window, and the machine powers off on the button. On XP,
where the same core runs, `CKTEST` passes 4 of 4 with the rebuilt driver
installed on an overlay of `winxp-m7` (the first bind now logs `bound, no
key`, so it was the new build; the key arrives through `DdSetColorKey`).

**Still open:** any fault inside a callback leaks the lock the same way
and freezes the session. `cmd_lock_break` releases it only when the
faulting process dies (`ContextDestroyAll`); an exception frame in the
callbacks that releases the lock on unwind would turn the next such bug
into one failed call.
