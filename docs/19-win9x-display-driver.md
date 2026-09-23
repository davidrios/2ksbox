# 19. A native Win98 display driver (ADR-012, M10)

XP has a real display driver for our `d3dpt-vga` adapter (doc 15): a
video miniport, a display driver, a DirectDraw DDI and a Direct3D DDI
that turns the runtime's DP2 token stream into `d3dpt` protocol records
the host executor runs. This doc is the Win98/Me driver for the same
adapter over the same protocol: why it exists, how the XP driver was
split into an OS-independent core and a per-OS layer so the two share
one implementation, what Windows 9x does differently, and — in the
numbered sections — every fact and trap the bring-up and the real titles
taught, each with its reason. A launcher Win98 machine runs this driver
by default since 2026-09-16 (`bundle::video_choices`; `-vga cirrus`
stays one pick away, doc 06).

Read doc 15 first: what it says about the adapter, the register set,
the flip chain, the DP2 stream, palettized textures, colour keys,
execute buffers and the DX8 DDI is what the core *is*. The track's
state, files and test loop are `docs/tracks/m10-win98-driver.md`; every
test tool named here is described in `docs/testing.md`.

## Why a native driver instead of the wrapper stack

- The Glide/WineD3D path needs per-game files in per-game folders. The
  driver needs nothing next to a game: Windows' own `ddraw.dll` /
  `d3dim.dll` / `d3d8.dll` drive it, on 98 as on XP.
- It is the only way a 9x title gets an accelerated *desktop* too —
  modes from our table, page flips that are register writes, no copy
  inside QEMU (doc 15, "Shape").
- The DirectX 3–7 titles that matter on 98 are the ones M7 made work on
  XP through the HAL (execute buffers, colour keys, palettized
  textures, 8 bpp modes). On 9x that work costs only the per-OS layer.
- The M4 paravirtual device and its guest DLLs are untouched; the DLL
  path stays the fallback wherever the driver is not installed.

## The split

**The core** is everything about *our* adapter and *our* protocol: the
DP2 walker (tokens to `D3DPT_DP2_*` records, stream bindings, the DX8
rewrite, `TEXBLT`, `BUFFERBLT`, vs/ps 1.x validation); the surface table
(handles, mip levels, formats and their pitch arithmetic including DXT,
VRAM offsets, registration, dirty ranges, colour-key and palette
bookkeeping); the caps (`DDCAPS`, pixel-format lists, `D3DCAPS7`,
`D3DCAPS8`, the `ddflags` knobs); contexts, render targets, `Clear2`,
readback; the flip chain (offset register, frame counter, wait,
timeout); the heap layout, the encoder, the doorbell and the debug log.

**The per-OS layer** is everything about the operating system: entry
points and packaging, kernel services (memory, mapping, a tick), the
DirectDraw/Direct3D objects the OS hands over (NT's `DD_SURFACE_LOCAL`
against 9x's `DDRAWI_DDRAWSURFACE_LCL`), and GDI (NT's `Drv*` DDI against
9x's `.drv` DDI over the DIB Engine).

```
guest-tools/src/d3dptvid/
  core/  d3dpt_ddi.h   DDI structures identical on every Windows: caps
                       shapes, the DP2 command header and tokens, D3DCAPS8
         d3dpt_core.h  the core's types and the hooks it calls
         core_dp2.c    the DP2 walker
         core_surf.c   surfaces, formats, registration, keys
         core_caps.c   caps and format tables
         core_flip.c   heap layout, vertical blank, debug log, command window
         core_ctx.c    contexts, targets, clear, readback, Z-by-Lock
  nt/    d3dptdisp.c   Drv* + the dxg callbacks + the OS hooks; d3dptvid.c
                       the miniport; .def, .inf
  w9x/   d3dpt9x.c + dibthunk.asm   the 16-bit .drv (Watcom)
         d3dpt9dd.c                 its DirectDraw escapes
         d3dptvxd.c                 the mini-VDD (Watcom)
         d3dpthal.c + .def          the ring-3 HAL DLL, links the core (mingw)
         d3dpt9x.inf, res/, the probes (ddprobe, gdiprobe, pwrprobe,
         devcaps, setbpp, bsod + bsodvxd)
  ddk/   NT DDI headers;  ddk9x/ the 9x ones (MIT, from vmdisp9x)
```

The core is linked into a **kernel-mode** DLL on NT and a **user-mode**
one on 9x, so it may call no operating-system service at all: six
`d3dpt_os_*` hooks bring in everything it needs, and `build-driver.sh`
enforces it with `nm` (§19). Only `w9x/d3dpthal.c` links the core on 9x.

The refactor was proved by the M7 suite coming out byte-identical across
it (`d3dpt-dp2-test` and `D3D7TEST` against the same golden, the probes
with the same case counts, `D3DGAME8` still matching the native oracle),
and it landed green before any 9x code — a 9x bug and a refactor bug are
otherwise indistinguishable.

## What 9x does differently

Step 0 answered this by reading JHRobotics' **`vmdisp9x`** (the Win9x
display minidriver behind SoftGPU, derived from Michal Necasek's
VirtualBox minidriver) and its companion **`vmhal9x`** (the
DirectDraw/Direct3D HAL), cloned to `build/ref/` and read, not vendored
(the track doc has the clone commands).

### 1. A 9x display driver is three binaries, not two

| | XP | Win98/Me |
|---|---|---|
| GDI | `d3dptdisp.dll`, kernel, `Drv*` DDI | a **16-bit NE `.drv`** exporting the Win3.x DDI by ordinal (`BitBlt.1 … ValidateMode.700`), ring 3 |
| drawing | GDI draws into the engine bitmap = VRAM | every drawing export **jumps to the DIB Engine** (`DIBENG.DLL`); the driver keeps `Enable`/`ReEnable`/`Disable`, `Control`, palette, cursor, `ValidateMode` |
| hardware | the miniport `d3dptvid.sys` | a ring-0 **mini-VDD** (`system win_vxd dynamic`) that owns the adapter, maps VRAM and arbitrates the DOS boxes |
| DirectDraw / D3D HAL | in the kernel display driver, called by `dxg.sys` | a **ring-3 32-bit DLL** loaded into the *game's* process by `ddraw.dll` |

Ours are `d3dpt9x.drv`, `d3dpt9v.vxd` and `d3dpt9hl.dll`. The last row
is the good news: the part that carries the DP2 walker and surface
table is an ordinary user-mode Win32 DLL.

### 2. How the 32-bit HAL is published from a 16-bit driver

The `.drv` answers GDI's `Control` escape `DCICOMMAND` (0x0C03) with four
sub-commands: `DDNEWCALLBACKFNS` hands it DirectDraw's `lpSetInfo`;
`DDGET32BITDRIVERNAME` returns a DLL name, an entry point (`DriverInit`)
and a 32-bit context value, and DirectDraw loads that DLL and calls the
entry; `DDCREATEDRIVEROBJECT` builds `DDHALINFO` and calls `lpSetInfo`;
`DDVERSIONINFO` reports the runtime version. The context value is the
linear address of a block shared by both halves — 9x maps everything
above 2 GiB into every process — into which `DriverInit` writes its
32-bit callbacks for the `.drv` to publish. No registry entry, no
signing: whoever the `.drv` names is loaded. §20–§23 are how that
actually came to work.

### 3. The DDI structures: identical calls, rearranged objects

- **Per-call structures are field-for-field identical**:
  `DD_CREATESURFACEDATA` / `DDHAL_CREATESURFACEDATA`, and
  `D3DNTHAL_DRAWPRIMITIVES2DATA` / `D3DHAL_DRAWPRIMITIVES2DATA` (fourteen
  fields in the same order). Only pointer type names differ; both are
  `__stdcall`.
- **The DirectDraw object structures are not**: NT's `DD_SURFACE_LOCAL`
  has ten members, 9x's `DDRAWI_DDRAWSURFACE_LCL` twenty-six, in another
  order. Every field we read exists on both under the same name at
  different offsets, and `lpGbl->wWidth`/`wHeight` are `WORD` on 9x. So
  a neutral surface descriptor is required (§19).
- **The DP2 token stream is the same stream**: every opcode value agrees,
  and `vmhal9x` sees the full DX8 set in a real 98 guest.

### 4. The DDI does reach DirectX 8 on 9x

`GetDriverInfo2` works as on NT (same GUID, `D3DGDI2_MAGIC`, sub-types,
`D3DCAPS8`), so the whole of M7c is in scope. 9x's runtime also offers
the pre-DP2 entries (`RenderState`, `RenderPrimitive`,
`DrawOnePrimitive`, `TextureCreate`); a DDI 8 driver may leave them out
(§25).

### 5. Caps rules are *not* the same as NT's

- `DDCAPS_GDI` is normal on 9x; on NT it makes dxg drop the whole HAL
  (doc 15). Same bit, opposite answers — a per-OS caps table.
- On 9x a HAL callback may return `DDHAL_DRIVER_NOTHANDLED` and the HEL
  takes over; on NT a declined `DdBlt` is `E_NOTIMPL` to the application
  (doc 15, "Blit caps and the HEL"). 9x's own validator rules are §21.

### 6. Modes come from the registry, not from the adapter

On NT the miniport enumerates the host's mode table. On 9x the mode list
is **written into the registry by the INF** (`HKR,"MODES\<bpp>\<w>,<h>"`)
and the driver only validates and sets modes. `d3dpt9x.inf` lists
640×480, 800×600, 1024×768 and 1280×1024 at 8, 16 and 32 bpp, plus two
4 bpp rows that belong to other drivers (§16). `vmdisp9x` ships a tool
to rewrite the list from the adapter; we have none, so "the mode table
comes from the player" (M2) holds on 98 only up to the INF's list.

### 7. Installation

A plain `Class=DISPLAY`, `signature="$CHICAGO$"` INF matched on
`PCI\VEN_1234&DEV_3D00`, with `HKR,DEFAULT,drv`, `HKR,DEFAULT,minivdd`,
`HKR,,DevLoader,,*vdd` and the mode list. PnP installs it with no clicks;
no `DRVINST.EXE` equivalent is needed (§16).

### 8. Ring 3 reaches the hardware through the VxD

The HAL reads the adapter through linear addresses the mini-VDD maps
into the shared arena and publishes in the shared block — registers
included, so **the doorbell is a direct register write from ring 3**,
the same cost as XP, and the VxD needs no `DeviceIoControl` handler
(§20). What is new against NT is that every process with a Direct3D
device runs its own copy of the HAL against one command window; the
window is serialised by a lock word in the shared block (§32, §36).

### 9. Toolchain: two compilers

- The `.drv` and the `.vxd` need **Open Watcom v2** (`wcc`, `wcc386`,
  `wasm`, `wlink`); mingw-w64 makes neither format. `build-driver9x.sh`
  picks the host directory from `uname`, so Linux, macOS arm64 and
  Windows all build both; a host without Watcom builds the ISO without
  `DRIVER9X\` and says so. The script applies the post-link fixes wlink
  gets wrong and fails on the silent refusals of §12, §13, §18 and §20.
- The ring-3 HAL builds with `i686-w64-mingw32`, freestanding like the
  NT driver; mingw-w64 already ships `ddrawi.h`, `d3dhal.h`, `dmemmgr.h`.
- The 16-bit headers (`gdidefs.h`, `dibeng.h`, `minivdd.h`, `valmode.h`)
  are vendored from `vmdisp9x` under MIT in `ddk9x/` (its README has the
  provenance); no Microsoft DDK. The `.drv` links no Watcom C runtime,
  and `dibeng.lib` is generated by `wlib` from a text import list.

### 10. What this means for the split

The core stays freestanding C with no OS calls, every DirectDraw object
arrives as the neutral descriptor, and of the three 9x binaries only the
ring-3 HAL links the core.

## The driver in the product

- Every Win98 machine the launcher writes defaults to `-vga none -device
  d3dpt-vga,addr=0x02` (NIC pinned at `0x03`) since 2026-09-16; the
  Cirrus is the other pick. The command line follows the bundle's
  `video` at launch, so changing it under an installed guest is a
  hardware change: new adapter, plain VGA, driver wanted (doc 06).
- The driver ships on the guest-tools ISO as `DRIVER9X\` and `SETUP.EXE`'s
  display-driver component installs it on 98/Me (`guest-tools/README.md`;
  §16, §28).
- The test tools keep their own `-vga cirrus` machines, where the in-box
  driver stays exercised; `VGA=cirrus` on `tools/win98-game-test.sh` is
  the standard control.
- The doc 04 Win98 title matrix — this driver against the Glide/WineD3D
  stack, same image, same host — is still to be measured (track doc).

## Bringing up the three binaries

### 11. The mini-VDD is not optional

A `.drv`-only driver cannot work: **Windows 98 unmaps the adapter's BARs
within the first half-minute of boot** when nothing claims the device's
resources, so the `.drv` reads zeros from PCI config space and GDI falls
back to VGA (a 16-colour desktop). The mini-VDD registers with the main
VDD (`VDD_REGISTER_DISPLAY_DRIVER_INFO`) and hands the `.drv` mapped
selectors; what makes the Configuration Manager leave the resources alone
is the devnode having a complete driver set — `minivdd=<file>` in its
key (§12). The VxD comes before the display driver can do anything.

Also learned here:

- **A display driver must carry `oembin` resources** (`config.bin`,
  `colortab.bin`, `fonts.bin`, `fonts120.bin`: machine metrics, the
  Control Panel colour table, the system LOGFONTs). They are small fixed
  Windows 3.1 DDK structures, written as C in `res/` and linked to raw
  binaries.
- **Two debug channels.** The DEBUG register cannot speak before the
  register page is mapped, which is where the interesting failures are,
  so every line also goes to port 0xE9 (`-debugcon file:…`). Port 0xE9
  is the reliable channel in ring 0.
- **`SYSTEM.INI` is not a shortcut**: a hand-set `[boot] display.drv=`
  is rewritten when Windows re-detects the adapter.

### 12. Linking a VxD that the VMM will actually load

A VxD the VMM dislikes is **simply never loaded** — no `BOOTLOG.TXT`
line, nothing on any channel. Three wlink facts, all fixed by
`build-driver9x.sh`:

- **Every LE object must have base address 0 and the executable flag**
  (wlink leaves 0x10000/0x11000 and marks only the first executable).
  This is all `vmdisp9x`'s `fixlink -vxd32` does.
- **The DDB must be at offset 0 of the code object.** Code, data and
  constants go into one segment of class CODE (`#pragma
  data_seg("_LTEXT", "CODE")` and siblings), giving one object with the
  DDB first. A single `static const` array added to the VxD once moved
  the DDB and the VMM silently stopped loading it; the build checks the
  map for `VXD_DDB` at `0001:00000000`.
- **The entry-table bundle must be type 3 (32-bit)**; wlink writes type 2
  because the symbol is data. One byte.

`BootLog=1` in `MSDOS.SYS` did not refresh `BOOTLOG.TXT` on the test
image: check the file's date before believing it.

### 13. Why GDI would not load the driver

**One dangling relocation.** The idiom every 9x display driver uses to
lock its code segment,

```c
extern char __based(__segname("_TEXT")) *pText;
GlobalSmartPageLock((__segment)pText);
```

references a *second* segment called `_TEXT`, class `FAR_DATA`. In a
small driver it stays empty, wlink drops it from the NE segment table
and leaves the relocation naming it; KERNEL's loader refuses the module
and says nothing anywhere — it looks exactly like GDI choosing another
driver. Ours takes `CS` instead (same selector, no relocation). The
build walks the NE relocation records and rejects any reference to a
segment the table does not have.

### 14. Getting the adapter into ring 3

- **The mapping must be one ring 3 can reach.** `_MapPhysToLinear` maps
  into the system arena; the mini-VDD uses `_PageReserve(PR_SHARED, …)`
  and `_PageCommitPhys(…, PC_USER | PC_WRITEABLE | PC_INCR)`, one
  mapping for the VxD and the driver. **This VMM refuses `PC_PRESENT`**,
  silently (`_PageCommitPhys` returns 0), and `PC_INCR` — without which
  all 128 MB alias onto one page — is accepted only once `PC_PRESENT` is
  gone. `_CopyPageTable` on the first and last page settles it in one
  boot (`f0000a07` … `f7fffa07` is right; the same value twice is not).
- **The registers are 32 bits wide and the device accepts nothing else.**
  `d3dpt-vga`'s register BAR is `valid.min_access_size = 4`; a 16-bit
  compiler turns `*(DWORD __far *)p` into two word accesses, which QEMU
  answers with zero and drops, with no log on either side. `RegGet` /
  `RegPut` are hand-written 32-bit accesses (the module is `.386`). The
  XP driver, being 32-bit code, never met this; any 16-bit code against
  the adapter must.

### 15. The strip of garbage was the message

A 9x fatal exception is a VGA **text** screen, and on this adapter it can
hide behind the linear frame buffer:

- **QEMU's VGA core keeps its planes interleaved, four bytes to a
  character cell, from VRAM offset 0**, so the text page at 0xB8000
  lands in the first 32 KB of the linear frame — a band of coloured
  noise across the top that survives the driver's own clear, because the
  VMM writes it afterwards.
- If nothing turns the linear mode off, the message sits there unread.
  Before the mini-VDD's screen-switch hooks (§26, §29), every run that
  "stopped at a black desktop" was a machine holding up a message.

`tools/win98-driver-test.sh` (and `win98-game-test.sh`, with `TEXT_AT=`
mid-run) reads that page out of VRAM and prints it — believe it over the
screendump. It first proves the page *is* text (plane 3 untouched, a
handful of attributes, mostly spaces — all three, because a 32 bpp
desktop passes the first on its unused byte), or it invents messages
from desktop pixels. The first fault read this way was inside
`DIBENG.DLL`, dereferencing the `DIBENGINE` pointer the `DIBTHK` thunks
in `dibthunk.asm` push; the reference driver's thunk list (which thunks
neither `ExtTextOut` nor `SetPalette`, and reaches the cursor entries
through C) is what ours is held against.

### 16. Installing it, the way every other driver installs

PnP installs from `d3dpt9x.inf` with no clicks, writes `drv=d3dpt9x.drv`,
`minivdd=d3dpt9v.vxd`, `DevLoader=*vdd` and the mode list into the
adapter's key, and sets SYSTEM.INI's `[boot] display.drv=pnpdrvr.drv`.

**`pnpdrvr.drv` is correct, not a failure**: it is what Windows writes
for every PnP display driver, there is no such file, and it resolves
through the adapter's key. What once stopped ours loading was the INF
lacking a **`DelReg`**: an adapter that ran the in-box VGA keeps a
`CURRENT` key and a `MODES` tree naming that driver, an AddReg does not
remove what it does not mention, and GDI resolves through the leftovers.
The INF deletes `Ver`, `DevLoader`, `DEFAULT`, `MODES` and `CURRENT`
first, and hands `MODES\4\640,480` to `vga.drv` and `MODES\4\800,600` to
`supervga.drv` by name (a 16-colour mode must resolve to *something*).

- Windows picks the first mode from the `MODES` list, not `DEFAULT,Mode`
  (`32,640,480`): a fresh install comes up at 800×600×16.
- `NAME_IN_INI=1` on `tools/win98-driver-test.sh` names both halves in
  `SYSTEM.INI` instead, for "does this build work" rather than "does it
  install". Edit `SYSTEM.INI` in binary: it has CRLF line endings and a
  text-mode rewrite eats a section header.
- A 9x splash screen frozen after a restart is patch 22's APIC bug
  (`patches/qemu/README.md`, `22-upstream-apic-reset-cpuid`), not the
  driver.
- On the ISO the driver is `DRIVER9X\`; `SETUP.EXE` copies the INF and
  the three binaries into `WINDOWS\INF` (and the binaries into `SYSTEM`)
  and asks for a restart — the INF is the installer and the boot runs
  it. Over an installed driver every file is staged and swapped at the
  restart (§28).

### 17. Ending a run

**End a run with the ACPI power button** (`system_powerdown`), never
keystrokes: a machine that does not power off leaves the FAT dirty and
the next boot is ScanDisk or **safe mode** — no driver, no VxD, an empty
debug log, which reads exactly like the driver failing. A modal dialog
swallows the button as it swallows keys, so the harnesses press Escape
first, and answer the `install` run's restart prompt with `alt+n` (the
restart is the next run's job). A machine that has faulted cannot be
shut down: it waits for a key on the text screen of §15, which the
printed text page says.

### 18. Display Settings showed one resolution

The Display applet calls the driver's `ValidateMode` (ordinal 700) once
per registry row, and ours faulted on the first. **Open Watcom takes a
function's attributes from its first declaration**: `valmode.h`
prototypes `ValidateMode` as plain `WINAPI`, so the definition's
`__loadds` was dropped and the export ran on the caller's DS (a thunk's
alias of the applet's stack), reading the driver's globals from there.
The driver hides the header's prototype with a `#define` around the
include, and `build-driver9x.sh` refuses a `.drv` any of whose exports
neither jumps straight to the DIB Engine nor loads DGROUP in its first
eight bytes. With the fix all eight 16/32 bpp modes validate and the
applet switches the adapter live.

Driving the applet headless on a Portuguese Windows: Run is "Executar"
(`e`; `d` is "Desligar"), `control desk.cpl,,3` loses its arguments, and
Ctrl+Tab does not switch property-sheet tabs — right-click the desktop
through the USB tablet, Up + Enter, and click the tab.

### 19. The split, as it landed

`d3dptdisp.c` went from 3 708 lines to 1 975 of NT plus ~1 940 of core
in five files, and the core includes no DDK header of either family.

**Two boundaries, not one.**

- The **DDI payloads** (`D3DDEVICEDESC_V1`, `D3DPRIMCAPS`, `D3DCAPS8`, the
  `DP2COMMAND` header, the `GetDriverInfo2` shapes) are identical on NT
  and 9x (§3), so they live once in `core/d3dpt_ddi.h`; the NT header
  keeps only the NT callback data and tables.
- The **surface objects** differ, so the core reads a flat
  `d3dpt_surf_desc` (handle, caps, caps2, flags, w, h, pitch, linear
  size, `fpVidMem`, resolved D3DFORMAT, pixel-format flags, source
  colour key) and **never keeps the OS's object past the call**: a key
  set before registration is taken off the record, a later one arrives
  through `SetColorKey` (keeping the object as a `void *` once outlived
  its surface: §36).

**Six hooks, nothing else**: `d3dpt_os_alloc` / `d3dpt_os_free`,
`d3dpt_os_ticks` (the flip timeout's clock), `d3dpt_os_surf` (fill a
descriptor), `d3dpt_os_attached` (attached surfaces that are not mip
levels) and `d3dpt_os_next_mip`. **`build-driver.sh` compiles the core
alone and asks `nm` what it still wants**: every undefined symbol must be
the core's own, a hook, or `memcpy` / `memset` — nothing in the source
stops an `Eng*` call being written into the core, and it would fault on
9x (`ERROR: the core calls out of itself: _EngDebugPrint`).

The callback *tables* name the layer's functions, so filling them is the
layer's; what they claim is `core_caps.c`'s. NT's compressed-surface
sizing dance, every `Drv*` entry, the mode list, palette, cursor and
mapping stayed in the layer. The public `ddraw.h` structures
(`DDPIXELFORMAT`, `DDSURFACEDESC`, `DDSCAPS`) are identical on both and
the core uses them directly.

**The constant trap.** mingw's public `ddraw.h` lacks
`DDSCAPS_EXECUTEBUFFER` (0x00800000, the DDKs' `DDSCAPS_RESERVED2`), and
the core once spelled it `0x00000800` — `DDRAWISURF_HASCKEYSRCBLT`. It
built, NT's layer was unaffected (its DDK definition won), and only the
core's vertex-buffer test was wrong: protocol v9's video-memory vertex
buffers stopped being recognised and one `SHTEST` case in nine drew the
previous case's colour. The core now defines its three
DirectDraw-internal bits under its own names (`DDSCAPS_EXECUTEBUFFER_`,
`DDRAWISURF_HASCKEYSRCBLT_`, `DDRAWISURF_HASPIXELFORMAT_`) and each layer
fails to compile if one disagrees with its DDK. This is why the whole
guest battery runs over any core change.

### 20. Publishing the HAL

The chain of §2 runs end to end:

```
d3dpt9x: QUERYESCSUPPORT(DCICOMMAND) -> 0x00000100
d3dpt9x: DCICOMMAND … command=0000000d   (DDVERSIONINFO)
d3dpt9x: DCICOMMAND … command=0000000b   (DDGET32BITDRIVERNAME)
d3dpthal: DriverInit, block at 0x8a7cf000 regs 0x80018000 vram 0x80019000
d3dpt9x: DCICOMMAND … command=0000000c   (DDNEWCALLBACKFNS)
d3dpt9x: DCICOMMAND … command=0000000a   (DDCREATEDRIVEROBJECT)
d3dpt9dd:   the DLL read magic=42463344 version=…
d3dpt9dd: DirectDraw took the HAL
```

— the DLL reads the adapter's `MAGIC` register straight through the
mini-VDD's linear mapping, from ring 3, with no ioctl. Three facts, each
of which looked like something else:

1. **`QUERYESCSUPPORT(DCICOMMAND)` must answer `DD_HAL_VERSION`, not 1.**
   Answering 1 tells DirectDraw it is a DCI driver: one
   `DCICREATEPRIMARYSURFACE` and never a DirectDraw question again, with
   no error anywhere.
2. **DPMI cannot give a GDT selector's base.** The mini-VDD builds its
   selectors in the GDT, and `int 31h AX=0006` answers junk for them
   (ignoring its own carry flag). The VxD returns both linear addresses
   (VRAM in `ESI`, the register page in `EDI`, `d3dpt9v.h`).
3. **The pointers in `DDHALINFO` are 16-bit far pointers** (callback
   tables, `lpModeInfo`, `vmiData.pvmList`): the 16-bit runtime walks
   them, and linear addresses make `DDHAL_SetInfo` refuse the HALINFO.

**The shared block** (`w9x/d3dpt9hal.h`) holds a magic and version, the
adapter's linear addresses and VRAM size, the current mode, the `cb32`
table the DLL fills, the DLL's module handle, the DirectDraw tables and
the command-window lock. The `.drv` allocates it through DPMI, which puts
it above 2 GiB. Both toolchains compile it, so every field is
fixed-width and the 16-bit side asserts each DDI structure fits its slot.

Nothing on a Win98 desktop calls `DirectDrawCreate`, so
`tools/win98-driver-test.sh` takes `PROG=<exe>` and names it in WIN.INI's
`run=`; `ddprobe.exe` is the program (caps, video memory, surfaces,
`WaitForVerticalBlank`, flips, a mode test — §30, §33).

**Build checks** from here: a freestanding DLL's entry point is named by
hand and `ld` only *warns* when it cannot find it, leaving
`AddressOfEntryPoint` zero — a silent reboot on 9x the moment DirectDraw
loads it. The build fails on a zero entry point, a missing `DriverInit`
export, an import from anything but `kernel32`, and an instruction past
the Pentium III floor.

### 21. The runtime read the HALINFO and threw it away

**`DDCAPS2_CERTIFIED` in `ddCaps.dwCaps2` makes DirectDraw refuse the
whole driver.** "Certified" is what the runtime says about a driver,
never what a driver claims.

It is invisible because the two halves of the runtime split the job: the
16-bit `DDHAL_SetInfo` only *stores* the HALINFO and returns TRUE; the
32-bit `ddraw.dll` validates it later while building the
`DDRAWI_DIRECTDRAW_GBL`, and on failure its caller silently builds an
emulation-only object. The driver logs "DirectDraw took the HAL" and
every application gets `DDCAPS_NOHARDWARE`. The validator, read out of
the guest's own `DDRAW.DLL` (DirectX 6.1; the rules still hold on 9.0c):

| rule | what the driver must do |
|---|---|
| `dwSize` is 0x130, 0x1cc or larger | the DX3 or DX5+ `DDHALINFO` (ours 0x1cc) |
| the structure is readable | `IsBadReadPtr` over `dwSize` |
| every heap has a non-zero `fpStart`, none `VIDMEM_ISNONLOCAL` | a `VIDMEM_ISHEAP` heap also needs `DDCAPS2_NONLOCALVIDMEM` |
| `vmiData.ddpfDisplay.dwSize == 32` | 8 bpp palettized needs `dwRGBBitCount == 8` |
| each callback table's `dwSize` a multiple of 4 and ≥ its minimum | 0x28 or ≥0x30 DD, ≥0x40 surface, ≥0x10 palette, ≥0x1c execute buffer |
| every callback a `dwFlags` bit claims passes `IsBadCodePtr` | bit index = entry index |
| no `DDSCAPS_OPTIMIZED` in `ddsCaps` | |
| **no `DDCAPS2_CERTIFIED`** | |
| `dwNumModes > 0` implies `lpModeInfo` | |

And from the object builder: **`DDCAPS_BLT` requires a `Blt` callback
and SRCCOPY (ROP 0xCC) in `dwRops`**, and `DDSCAPS_OFFSCREENPLAIN`,
`_OVERLAY`, `_TEXTURE`, `_ZBUFFER` each require a non-zero, even
`vmiData` alignment. A cap claimed without its conditions throws the HAL
away, as dxg does on NT.

Ruled out on the way, so nobody repeats them: `DDHALINFO_ISPRIMARYDISPLAY`
(set it; it was not the cause), `DDHALINFO_MODEXILLEGAL` (but see §30),
`dwHALVersion` either value, `dwVidMemTotal`/`Free`, `EmulationOnly`.

**The 9x `ddflags` bits live in the high half** of the adapter's DDFLAGS
register (`D9F_*` in `w9x/d3dpt9x.h`), so they never collide with the
core's; `tools/win98-driver-test.sh` takes `DDFLAGS=`. `D9F_CERTIFIED`
(0x01000000) puts the bad bit back as a repro; it is the same bit as the
core's `DDF_NO_VOLUME`, so setting one sets both.

### 22. Where the 32-bit HAL has to live

- **DirectDraw loads the HAL and calls `DriverInit` in `DDHELP.EXE`**,
  once per machine; **every application then validates the stored
  HALINFO in its own address space** (`IsBadCodePtr` per entry, the
  whole object discarded on one failure).
- A DLL in the private arena has a different address in every process,
  so DDHELP's callback addresses are bad pointers everywhere else. **The
  HAL must live in the shared arena above 2 GiB** (the reference bases
  `vmhal9x.dll` at `0xB00B0000`, as we do). §23 is how.
- Safety net: the `.drv` withholds any `cb32` entry below `0x80000000`
  (`the 32-bit HAL is at=…, below the shared arena: callbacks withheld`),
  because one unreachable callback costs the whole HAL, video memory and
  mode list included. The build fails a DLL based below `0x80000000`.
- A near-pointer trap from the same session: the `.drv` is small-model,
  and `*(DWORD *)&tbl.entry` into the far shared block truncated to an
  offset and stored into the driver's own DS. The flags went through the
  far struct, the validator skips zero entries, and a HAL with an empty
  callback table was "accepted".

`ddprobe` runs the runtime's own test where the runtime runs it:
`GetModuleHandle` / `LoadLibrary` / `IsBadCodePtr` on the HAL from the
application's process.

### 23. The shared arena: `IMAGE_SCN_MEM_SHARED`

**The 9x loader puts a PE DLL based above `0x80000000` into the shared
arena only if every section is marked `IMAGE_SCN_MEM_SHARED`**
(`0x10000000`); otherwise it relocates it into the private arena, and a
DLL with its relocations stripped fails `LoadLibrary`. GNU `ld` has no
switch for the flag (MSVC's `/SECTION:…,S`), so a post-link step in
`build-driver9x.sh` ORs it into every section of `d3dpt9hl.dll` and
recomputes the PE checksum. The HAL then loads at `0xB00B0000` in DDHELP
and in every application, `IsBadCodePtr` is 0 there, and DirectDraw calls
into it.

### 24. The DirectDraw DDI

- **The HAL links all five core files** with its own `memcpy`/`memset`
  (`kcrt.c`, `rep movsd`) and imports only kernel32. Its hooks: a shared
  heap (§32), `QueryPerformanceCounter`, and surface descriptors from
  `DDRAWI_DDRAWSURFACE_LCL` (`lpAttachList` for attachments,
  `DDSCAPS_MIPMAP` for mips).
- **VRAM addresses are linear on 9x.** NT's `fpPrimary` is 0 and heap
  offsets are zero-based; 9x's `vmiData.fpPrimary` is the linear VRAM
  address, so `d3dpt_os_surf` subtracts it to give the core a true VRAM
  offset.
- **Surface callbacks** (`d3dpthal.c`, structures in `w9x/ddhal32.h`):
  `Flip32` (waits on the frame counter with `DDFLIP_WAIT`, else
  `DDERR_WASSTILLDRAWING`; reads back live render targets; writes the
  target's VRAM offset to `D3DPT_FB_REG_OFFSET`), `GetFlipStatus32`,
  `GetBltStatus32`, `Lock32` (readback of live targets), `Unlock32`
  (`D3DPT_OP_VRAM_DIRTY`; Z buffers, §34), `DestroySurface32`,
  `SetColorKey32`. `ddprobe`'s five flips pace at ~16 ms with the scanout
  offset alternating in the QEMU log.
- **8 bpp**: `ModeOk` checks `D3DPT_FB_CAP_BPP8`; `Enable` sets up
  GDIINFO for a palette (`RC_PALETTE`, 20 reserved, 256 registers) and
  the DIB Engine's `PALETTIZED`; the `SetPalette` export (ordinal 22) is
  C, feeding `D3DPT_FB_REG_PALETTE`. The colour table is reserved in the
  PDEVICE at every depth (§30) and initialised at `Enable` (§26).

### 25. The Direct3D DDI on 9x

**Both halves pass to XP's bar**, with no change to `core/`: `EBTEST` 5
of 5 (the DirectX 3 face); `D3D7TEST`'s frame through `d3dim700.dll`
byte-identical to `d3dpt-dp2-test`'s golden (59 fps, the flip pacing
holding under load); `SHTEST` 9 of 9 with hardware vertex processing
(`vs 1.1, ps 1.4`); `CKTEST` 4 of 4; `DXTTEST` creating every format in
every pool. **A DDI 8 driver may leave out the pre-DP2 entries** (§4):
9x's `d3d8.dll` drives ours through `DrawPrimitives2` alone. Doc 15's
two `GetDriverInfo2` negotiation traps apply unchanged.

**The failure shape to suspect first.** Everything was black at first,
every call `S_OK`, and the host log had **no `ddi:` line**: the 9x layer
computed the command window as `vram_size - CURSOR_BYTES - SHM_SIZE`
while the device puts it at `vram_size - SHM_SIZE`, so batches went
16 KiB below the window the executor reads. The layer now reads
`D3DPT_FB_REG_CMD_OFFSET`, as NT always did. Together with §19's
constant and §26's heap end, this is the track's recurring bug: **a value
the layer re-derives instead of reading from the one authority fails
silently and looks like something else.** When "the calls all succeed
and nothing is drawn", diff the layer against `nt/`.

**A 2ksbox Win98 machine runs DirectX 9.0c** (decision). The in-box 6.1
(`DDRAW.DLL` 4.06.03.0518, no `DirectDrawCreateEx`) is an older DDI
generation with no XP precedent and no pixel oracle (`D3D7TEST`'s DX7
path cannot run on it). On 9.0c the runtime is the one XP has, and our
DX8 DDI serves DirectX 3 through 8 through the runtime's own translation
(DDI 9 would only add what doc 04 routes to M4's per-game `d3d9.dll`).
So stock Win98 gets the accelerated desktop and DirectDraw, and
**Direct3D wants DirectX 7 or later in the guest**. The DirectX 6
accommodations the step once grew (an `_INT`/`_LCL` pointer sniff, the
pre-`Clear2` `Clear` entry, `D3D7TEST`'s `IDirect3D3` path) were
instrumented, found never taken on 9.0c, and deleted.

**Trap when updating an image**: installing DirectX by hand means
booting it on `-vga cirrus`, which rebinds the display to the in-box
driver and drops `D3DPT9V.VXD` from `[386Enh]`. `SYSTEM.INI` still says
`pnpdrvr.drv`; `[boot.description]` says Cirrus. `win98-driver-test.sh
<image> install` puts it back; `boot` does not.

## Real titles and screen switches

### 26. Real titles, and a pointer that lived in the frame buffer

The first titles on the driver (the user's `claude98`: Monster Truck
Madness, NFS Porsche 2000, Total Annihilation, LEGO Island, Crimson
Skies, Blood) found what no probe could. `tools/win98-game-test.sh` is
the harness; it builds **the machine the player builds** (`launcherx
--print-args`: `-cpu pentium3`, the sound card, the disc on `ide.1`),
because Total Annihilation with no sound card prints "Sound system
initialization failed" and quits before a frame, which reads exactly like
the display driver failing. It re-stages the driver each run
(`NO_DRIVER=1` keeps the image's own).

#### The pointer must not live in the frame buffer

The DIB Engine draws its cursor **into VRAM** with a save-under, lifted
by `BeginAccess` / `EndAccess` around every GDI draw. **DirectDraw does
not go through GDI**: a game that locks the visible primary (Total
Annihilation, every frame) or flips writes over the cursor, and the next
mouse move stamps the stale save-under back — "the mouse is glitchy".
The flat 32-bit HAL cannot call the Engine's 16-bit exclusion pair.

So the pointer is the adapter's cursor sprite, as on XP (doc 15, "The
hardware cursor"): `SetCursor`, `MoveCursor` and `CheckCursor` are C in
`d3dpt9x.c`, a monochrome `CURSORSHAPE` becomes a8r8g8b8 below the
command window, and the host composites it. Details:

- **`C1_COLORCURSOR` is claimed only without a sprite**; a colour shape
  arrives in the screen's format. What the sprite cannot carry goes back
  to the Engine's software pointer, with the sprite taken off first, so
  there is always exactly one pointer.
- **`SetCursor` names two functions**: win16.h's API and the driver's
  ordinal 102. The API's declaration is hidden during the headers, as
  for `ValidateMode` (§18).
- **No CRT, so no 32-bit multiply**: `(DWORD)y * stride` links an
  undefined `__U4M`; use `MulW`.

A screendump shows no pointer on 9x as on XP (`d3dpt-vga: cursor 32x32
… defined` in the log is the sprite); the player composites it when the
pointer is grabbed.

#### The other five

- **NFS Porsche's silence and the `dxdiag` crash** are not the driver:
  the Portuguese SB16 wave-in name overflows DirectX 9's DSOUND (doc 20
  §5.3).
- **Crimson Skies** was first put down to a SafeDisc bug; it is ours
  (§27, §28, §34).
- **The DirectDraw heap ran into the command window**: `d3dpt9dd.c`
  published `fpEnd` at the top of VRAM less the cursor, but the command
  window occupies the top 64 MB of the 128 MB aperture (the cursor
  image sits below it). The `.drv` reads `D3DPT_FB_REG_CMD_OFFSET` now
  (the NT core's `dd_heap_end()`); `d3dpt9dd: heap=… ..=…` must end
  64 MB below the top. `dwVidMemTotal`/`Free` no longer count the window.
- **An 8 bpp mode came up with an uninitialised palette**: `Enable`
  programmed the device from the colour table past the PDEVICE before
  anything had filled it. It fills Windows' default first (the twenty
  static colours, a 6-6-6 cube, a grey ramp).
- **A full-screen DOS box (Blood)** needs the screen switch: while
  `D3DPT_FB_REG_ENABLE` is set the device scans out the linear frame
  buffer and the DOS program's VGA writes never show. The mini-VDD
  answers four dispatch entries — `PRE_HIRES_TO_VGA` clears `ENABLE`,
  `POST_VGA_TO_HIRES` sets it, the other two log — and a real switch
  calls all four in order with the driver's `RestoreDesktopMode`
  (registered through `VDD_DRIVER_REGISTER`) in the middle:

  ```
  d3dptvxd: hi-res -> VGA
  d3dptvxd: hi-res -> VGA done
  d3dptvxd: VGA -> hi-res
  d3dpt9x: RestoreDesktopMode
  d3dptvxd: VGA -> hi-res done
  ```

Two DOS-game failures that look like the driver: a DOS/4GW EXE started
from `C:\` cannot find `dos4gw.exe` (`GUEST_CMD` takes several CRLF
lines, so `cd` first), and an image with no `SET BLASTER=` fails every
DOS game's sound probe. And judge a run when it is over: an interim log
of a switch that has not come back reads as a one-way switch to black —
diff the screendumps.

### 27. Crimson Skies: two binaries, and eliminations on the wrong screen

The install has two executables: `CRIMSON2.EXE`, the SafeDisc loader,
and `CRIMSON.EXE`, a no-CD patch (dated 2025) the user runs. A SafeDisc
fix measured on the first said nothing about a rendering bug in the
second — check which binary a report is about.

Against the Cirrus control (`VGA=cirrus`) the menu's logo and buttons
were solid white silhouettes on our adapter. The eliminations made next —
no Direct3D, no DirectDraw blit, GDI through the DIB Engine correct
(`gdiprobe.exe`: 57 cases at 8/16/32 bpp, the 8 bpp failures being the
probe's own palette quantisation), the engine setup matching `vmdisp9x`'s
`enable.c` field by field — were made on the opening splash, not the
menu, and are moot for this bug (§28). What stays useful:

- `gdiprobe.exe` checks each GDI raster operation's pixels with
  `GetPixel`; `setbpp.exe` sets the desktop depth from a batch file.
- **The game sets its own mode** (1024×768×16 here) whatever the
  desktop's depth; a depth bisect via the desktop changes nothing.
- **Reinstalling the driver does not reset the desktop depth**:
  `install` on a machine already bound to this driver re-copies files
  without PnP reinstalling, and `CURRENT` survives. Change depth with
  `ChangeDisplaySettings` from inside the guest.

### 28. Crimson Skies: the legacy blend, and locked driver files

**The menu is Direct3D** (≈23 textured quads a frame through the DX7
HAL). The `D3DPT_DP2_TRACE` frame showed `COLOROP = SELECTARG2` — the
all-white diffuse — with the texture's alpha. The game sets the DirectX 5
`TEXTUREMAPBLEND` render state (`MODULATE`) **with no texture bound**,
then `COLORARG2`/`ALPHAARG2`, and binds textures per draw; the
executor's legacy-blend emulation decided "no texture: diffuse" and its
re-evaluate flag was cleared by *any* stage-0 state 1–6, the ARGs
included. It keeps one flag per op now, ended only by the app's own
`COLOROP` / `ALPHAOP` (host side, `d3dpt/exec/d3dpt_exec_ddi.cpp`,
shared with XP; doc 15, "The white menu text"; `d3dpt-dp2-test` has the
sequence). The QUIT button's missing bottom half on the same menu was
the depth bug of §34.

Driving it headless: `GUEST_CMD=$'cd C:\\ARQUIV~1\\MICROS~1\\CRIMSO~1\r\nCRIMSON.EXE'`,
`CDS=` the disc, the Select Video Device dialog's OK at 496,302 on an
800×600 desktop, clicked with `qmpc.py relclick` (it walks the PS/2
mouse, reading the position back from the adapter's cursor registers),
then Space to the menu.

#### Which driver files Win98 locks while running

| file | overwrite the running copy |
|---|---|
| `D3DPT9X.DRV` (held by GDI) | fails — locked |
| `D3DPT9V.VXD` (held by the VMM) | fails — locked |
| `D3DPT9HL.DLL` (loaded per game) | succeeds |

**And the one that succeeds is the dangerous one**: 9x does not hold the
file while its pages are still demand-paged from it (as a `.drv`'s
discardable segments are reloaded from theirs), so a module replaced
underneath runs the new bytes at the old addresses and blue-screens —
`SETUP /ALL` over an installed driver did, at the restart prompt. So
SETUP's 9x driver step never overwrites a driver file that is already
there: every one is staged as `NAME.EX_` and swapped by `WININIT.INI
[rename]` at the restart. The doubled `/ALL` in
`tools/setup-guest-test.sh` guards it.

### 29. Leaving a DOS box, the blue screen, and the pointer over both

#### The exit was not a hang: nobody repainted the desktop

After a full-screen DOS box (Blood) the mini-VDD put the *adapter* back
and the screen stayed on the game's last frame, tiled, with the VGA
planes as a band on top — while `info registers` twice showed an idle,
healthy guest (EIP moving, `HLT=1`). **What puts the *desktop* back is
INT 2Fh AX=4001h (`SCREEN_SWITCH_OUT`) / AX=4002h (`SCREEN_SWITCH_IN`)**,
which the main VDD raises in the Windows VM and every display driver
hooks in `Enable`:

- on the way out, set `BUSY` in the DIB Engine's PDEVICE so GDI stops
  drawing — here the desktop's frame and the DOS program's VGA memory
  are the same bytes from VRAM offset 0;
- on the way back, restore the mode if still `BUSY` and call **USER's
  repaint entry, ordinal 275** (undocumented, via `GetProcAddress`),
  where every window's WM_PAINT after a full-screen session comes from.
  `DISPLAY.500` (`UserRepaintDisable`) defers it.

The handler is `_SWHook` in `dibthunk.asm`, the callbacks in
`d3dpt9x.c`. The saved vector lives in the code segment (no DS needed),
written through an alias from KERNEL's `AllocCStoDSAlias` (KERNEL.170,
by ordinal) because code segments are read-only; the driver has one code
segment (`_TEXT`, `preload fixed`), which is how §13's empty-`_TEXT`
trap stays avoided. The log reads `switched out` just after `hi-res ->
VGA done` and `switched in` just after `VGA -> hi-res done`.

#### The big pointer: a sprite over a screen that has no cursor

On a machine without `seamless_mouse` the player composites the cursor
sprite into the guest frame, and during a DOS game that frame was the
game's VGA screen — hence a big pointer at desktop coordinates.
`d3dpt-vga` reports the sprite hidden whenever `ENABLE` is off
(`fb_cursor_move` gates on it; every `ENABLE` write re-runs it) and a
reset drops the shape, which covers XP's full-screen console and both
families' blue screens too. **Never `dpy_cursor_define(con, NULL)`**:
QEMU's console takes a reference unconditionally, so NULL is a SIGSEGV
in the player; "no cursor" is a hidden 1×1 transparent one
(`fb_cursor_clear`). A Windows restart on the device
(`GUEST_CMD='RUNDLL32 SHELL32.DLL,SHExitWindowsEx 2'`) is the check.

#### The blue screen is drawn by the VDD

A 9x blue screen (fatal exception, protection error, Ctrl+Alt+Del,
"It is now safe to turn off your computer") is *message mode*: the VMM
stops the world and the main VDD programs VGA text mode itself. The
adapter has to leave its linear mode for it, and three roads reach the
mini-VDD:

- **A fault in a VM's own execution** takes the ordinary switch —
  `switched out`, `hi-res -> VGA` — so `PRE_HIRES_TO_VGA` clears
  `ENABLE` (§26).
- **`SAVE_MESSAGE_MODE_STATE`** (function 45; `ddk9x/minivdd.h` has the
  numbering, which past 43 differs from older write-ups) is called once
  at boot; the mini-VDD clears `ENABLE` there too.
- **A fault outside any VM** (a timer or event callback) sends no switch
  at all — the corruption of patch 44's `uint16_t` slot list
  (`patches/qemu/README.md`, `44-tlb-retire`) put up invisible VTDAPI
  screens this way. What every message screen does send is the VMM's
  `Begin_Message_Mode` (0x10) / `End_Message_Mode` (0x11) to every VxD:
  the first clears `ENABLE` remembering whether it was set, the second
  sets it again only if it was.

`tools/win98-bsod-test.sh` blue-screens a copy on purpose: `BSOD.EXE`
loads `BSODVXD.VXD` (`w9x/bsodvxd.c`), whose init executes `ud2` in ring
0; `WHEN=event` uses `BSODTMR.VXD`, which faults from a one-second
time-out (the VxD holds a `DIOC_OPEN` handle so it is not unloaded
first). A trigger a test depends on has to be something we ship: `con\con`
is patched on the test image, and a fault in a DOS box only ends the DOS
box. The mini-VDD logs the first calls of every other dispatch entry
(`vdd fn=…`); around a switch the VDD also calls `RESTORE_REGISTERS`,
`ACCESS_VGA_MEMORY_MODE`, `ENABLE_TRAPS`, `MAKE_HARDWARE_NOT_BUSY` and
`DISABLE_TRAPS`, none of which needs an answer.

Total Annihilation's reported crash on exit did not reproduce from its
main menu; an exit from inside a skirmish is still unexercised (track
doc).

### 30. Carmageddon: a heap overrun at 16→8 bpp, and DirectDraw's own Mode X

**The PDEVICE is sized once, at boot.** `Enable` reserved the 8 bpp
colour table (256 `RGBQUAD`s) in `dpDEVICEsize` only when the *current*
mode was 8 bpp, but GDI allocates the display PDEVICE once from the boot
depth and `ReEnable` reuses it. A game switching a 16 bpp desktop to
8 bpp wrote 1 KiB past the block into whatever the GDI heap put next —
blue screens in a different VxD each run (VTDAPI, VSB16, KERNEL32), the
signature of shared memory scribbled on. The table is reserved at every
depth now. `ddprobe.exe <w> <h> <bpp> [sys] [modex] [vga] [hold]` is the
mode test that reproduced it without the game.

**A 320×200 game wants Mode X, not a driver mode.** Carmageddon asks
`SetCooperativeLevel(0x53)` (with `DDSCL_ALLOWMODEX`), `SetDisplayMode(320,
200, 8)` and a `PRIMARYSURFACE | FLIP | COMPLEX | SYSTEMMEMORY` chain —
the DirectX SDK's Mode X recipe. For a 320×200 request the driver does
not list, the runtime switches the display driver out (GDI `Disable`,
`VDD_DISPLAY_DRIVER_DISABLING`), programs the VGA and DAC itself and
copies the system-memory chain into planar VGA memory at every `Flip`.
Our driver listed 320×200 and 320×240 and set `DDHALINFO_MODEXILLEGAL`,
so the game got a linear mode with a system-memory primary that nothing
presents — black, palette correct. **The driver lists no 320-wide mode
and leaves the flag clear**, as the reference does; the VGA core then
reports Mode X (`cr1=4f cr7=1f cr9=41 cr12=8f sr4=06`) and the game
draws. On the Cirrus the same thing worked because a DCI primary *is*
the screen.

While the linear mode is off the device logs the VGA core's mode
registers once per change (`d3dpt-vga: vga core cr1=… sr4=…`), which is
how a headless run tells Mode X, 13h and a blue screen's text mode apart.

Also from this section:

- **The held frame after `ENABLE` goes 0** (so a mode switch's reset
  does not flash the VGA core) is `D3DPT_FB_VGA_GRACE_MS`, wall clock;
  counted in display refreshes it lasted 45 s on a headless console.
- **`CS=F000 EIP=D40F` in V86 mode is SeaBIOS's interrupt stub `iret`**,
  where an idle Windows 98 spends its time; a machine ignoring the power
  button because a full-screen DirectDraw process holds it is not dead.
- **Known limitation**: a system-memory *flipping* primary in exclusive
  full-screen on a *driver* mode blocks its process inside `Flip`'s
  `DDFLIP_WAIT` for a present the runtime never completes (`DDPROBE 640
  480 8 sys` is the repro); declining the flip only moved the block.
  No shipped title reaches it: 320×200 is Mode X and 640×480 titles use
  a video-memory primary.
- `ddprobe`'s log reopens after every line: a blue screen otherwise left
  a 0-byte `DDPROBE.LOG` (the FAT directory entry's size never written).

### 31. 3DMark 99: the emulator, not the driver

3DMark 99 on the driver scored 3334 under TCG with **DXVK, RADV and the
executor under 0.3 % of QEMU**: a Win98 3D title's time is the guest CPU.
The work that followed is TCG's and lives in `patches/qemu/README.md`
(patches 35–39, and 42–45 after them), doc 22 and the M9 track: the TB
code map (patch 35, 3334 → 5894, the race test to the 60 Hz flip cap),
SSE loads built in registers (36), x87 with a sticky inexact flag (37),
known TB flags in the inline lookup (38), `vec_allsign` (39). The one
host-side change was the executor's: a DX7 indexed draw passed
`MinVertexIndex` so DXVK copied every batch's unused prefix (32 GB a run),
now rebased to the touched range.

Two driver-side notes. The HAL's byte-loop `memcpy` (both families now
link `kcrt.c`'s `rep movsd`) changed nothing measurable — the samples in
it were the TB walks' cost landing on its stores; A/B before crediting a
profile. And the vertical blank is a real limit in benchmarks: with it
off (`ddflags=32768`) a title runs past 60 flips/s; the pacing stays on
for games (doc 15, "The flip chain's vertical blank").

### 32. A read of the whole driver

A review of all three 9x binaries and the shared core, each finding
checked against the other side of its boundary:

**The HAL (`w9x/d3dpthal.c`)**

- **The core's memory comes from a `HEAP_SHARED` heap** (9x kernel32
  only, `0x04000000`), created on first use. Every pointer to it lives
  in the DLL's shared data (§23), so a per-process heap left the next
  Direct3D process freeing another process's block. The log says so if
  the heap ever lands below 2 GiB.
- **`DDHAL_GETDRIVERSTATEDATA` has the DDK's layout** (it had four
  invented fields and wrote `ddRVal` 12 bytes past the structure).
- **The destroy callbacks release only a handle the surface has**; they
  used to invent one from the counter that numbers DX3-era surfaces from
  101 — the runtime's own range — and could drop a live texture.
- **The command-window lock is per thread**, and every surface
  registration takes it. A process that dies inside a callback dies
  holding it; `ContextDestroyAll32` breaks a lock its process left
  (`cmd_lock_break`). A fault that the process survives still leaks it
  (§36).
- `GetVerticalBlankStatus` now reports the blank (doc 15, "The flip
  chain's vertical blank").

**The core (both families)**

- **A blit after a draw goes in a record of its own.** TEXBLT, BUFFERBLT
  and VOLUMEBLT are guest-side copies made during the walk while the host
  reads resources when it runs a draw, so one record would give every
  draw the last blit's bytes. `dp2_run` cuts the stream before any blit
  that follows a draw. No runtime has been seen to need it — XP's and
  9x's d3d8.dll end the DP2 call before such a blit, and `MGDTEST` passes
  on the unfixed core too — it is what the protocol needs if one ever
  batches them.
- **Long non-indexed draws are cut** into pieces of at most 0x10000
  vertices (lists on a primitive, strips overlapping, triangle strips on
  an even triangle; a fan still skips): the caps allow 0xffff primitives
  and the host used to skip such a draw whole.
- Stream 0's stride is bounded at 1024; TEXBLT rectangles on mip levels
  round up and a DXT rectangle starts at its block; a DP2
  `SETRENDERTARGET` becomes the context's readback target.

**The mini-VDD**: selector limits were one page long; the way back from
a DOS box re-enables the linear mode only if the switch found it on
(not under the 16-colour drivers, not after `PhysicalDisable`); the
notification log's counter no longer wraps.

**The `.drv`**: two inline-asm VDD calls now declare the registers they
clobber (latent, one register-allocation change from corrupting a mode
set), and a refused `ReEnable` restores the mode globals.

**Checked and left alone**: GDIINFO's English/twips extents look wrong
but Windows 98's GDI builds mapping modes from `LOGPIXELS` (a probe got
all five right on the unfixed driver); the DX3 bounce's parser pointer is
machine-wide (right while system DLLs load at one base); two mini-VDD
failure paths leak arena pages; VOLUMEBLT refuses DXT volumes (never
seen sent).

### 33. Diablo II's sheared menu: one pitch rule for a mode

The shareware demo's menu at 800×600×8 was smeared into streaks.
DirectDraw sizes a flip chain's back buffers with the pitch rounded up to
the HAL's advertised alignment (64), so the back buffer's pitch was 832
while the mode was scanned at 800; each flipped line landed 32 bytes
further along. 800 is the one 8 bpp width in the table 64 does not
divide (and 400×300 at 8 and 16 bpp); NT never had it because its
miniport rounds every mode to 32 and advertises 32.

**`D3DPT9X_PITCH(w, bpp)`** (`w9x/d3dpt9x.h`) — bytes per line rounded up
to `D3DPT9X_PITCH_ALIGN` (64) — is now the only pitch: GDI's `dwPitch`
(and so `deWidthBytes`, the VDD registration, `lDisplayPitch`), the HAL
mode table `SetMode32` programs, both VRAM-fit checks, and
`dwOffscreenAlign`. 800×600×8 scans at 832. `ddprobe <w> <h> <bpp>`'s
`pitch check:` line compares the flipping primary's pitch with its back
buffer's and is the guard.

Why the game was on DirectDraw at all: the shareware's `D2VidTst` saves
the renderer under `...\Diablo II Shareware\VideoConfig` and the game
reads `...\Diablo II\VideoConfig` — a Blizzard bug. Setting that key's
`Render` to 3 puts it on Glide (the Voodoo 2, doc 21).

### 34. Crimson Skies in flight: a Z buffer reset through a Lock

In flight "almost everything is transparent, some pieces leave trails":
of 454 draws a frame only the HUD changed a pixel. The game uses reversed
depth (`ZFUNC GREATEREQUAL`, far = 0) and **resets its Z buffer by
Locking it (`DDLOCK_WRITEONLY`) and writing 0** every frame — no `Clear2`,
no depth-fill blit — so the host's depth buffer kept the previous frame's
and everything farther failed. (The Z surface read over QMP with
`pmemsave` at BAR0 + its offset showed the zeros; the `Lock32` log was
exhausted by the intro video.) The same bug cut §28's QUIT button in
half.

**The fix** is in the shared core (`core/core_ctx.c`, `d3d_z_written`):
each layer remembers a Z buffer locked for writing (`Unlock32` on 9x,
`DdUnlock` on XP), and on its Unlock, if the buffer holds one value,
that value — through `dwZBitMask`, converted to [0, 1] with integers
only because XP's driver is kernel code that may not touch the FPU —
becomes a Z-only clear on every context using it. A read-only lock and a
non-uniform buffer are left alone (there is no depth-image upload). The
scan samples every 7th row and the last, whole — ~15 % of the buffer,
top to bottom (user's call, 2026-09-14) — because it is guest CPU every
frame. Z locks are logged separately (the first 16, then every 1024th)
with the core's decision.

A DirectDraw depth fill (`Blt(DDBLT_DEPTHFILL)`) does not reach the
driver either: claiming `DDCAPS_BLTDEPTHFILL` alone leaves the runtime
filling through a Lock (which this now catches), and `DDCAPS_BLT` needs
SRCCOPY and a real blitter (§21). A fill of *part* of the buffer is
lost. XP had the same gap and has the same fix.

**The guard is `ZFILLTEST.EXE`** (`guest-tools/src/d3dptvid/zfilltest.c`,
built for both families): Z cleared to 1.0, then a depth fill of 0, a
fill of 0xffff and a Lock writing 0, each followed by a z 0.5 quad under
GREATEREQUAL read back. All pass with the fix; the old driver fails the
fill-0 and Lock cases on both families.

```sh
STAGE=guest-tools/out/driver9x/zfilltest.exe PULL=2KSBOX\\ZFILLTEST.LOG \
  GUEST_CMD=$'cd C:\\\r\nZFILLTEST.EXE' tools/win98-game-test.sh <image> zf
tools/xp-driver-test.sh <overlay> cmd 'D:\DRIVER\ZFILLTEST.EXE & copy C:\2KSBOX\ZFILLTEST.LOG E:\'
```

#### The menu text: a texture cap, read before any call

The Instant Action page drew every string as the engine's 8×8
"missing texture" pattern: the strings' textures were never created, and
nothing was refused — a decision the game made from the caps.
`DEVCAPS.EXE` (`w9x/devcaps.c`: every DirectDraw device including
non-display ones, its DDCAPS, available memory per kind, every
`D3DDEVICEDESC7`) diffed against the Voodoo 2's DirectX 7 driver found
it: 3dfx claims `D3DPTEXTURECAPS_POW2`, we claimed any size. With POW2
the game makes 3 814 power-of-two textures on that screen instead of 218
and the text draws.

The core's texture caps claim **`D3DPTEXTURECAPS_POW2 |
D3DPTEXTURECAPS_NONPOW2CONDITIONAL`** (what GeForce 2–4 and Radeons
claim) rather than POW2 alone, which would refuse every title a
non-power-of-two texture; the host has no such limit, the claim is for
the titles. `ddflags=0x2` (`DDF_TEX_ANYSIZE`) is the old answer and
`ddflags=0x20000000` (`DDF_TEX_256`) caps textures at 256×256 like a
Voodoo 2. The DX7/DX8 max texture aspect ratio is published as 4096
(it was 0; `D3D7TEST` refuses less than 8).

### 35. The shutdown screen's green band: the linear mode went off too late

"Windows is shutting down" sometimes showed the desktop with a bright
green band on top, for a quarter of a second — visible only to the
player (`PLAYER=1 PLAYER_SHOT_EVERY=6` on `tools/win98-game-test.sh`;
one-a-second screendumps never catch it). A 16 bpp desktop is shown
through the adapter's shadow copy, updated only while `ENABLE` is 1, and
the held frame (§30) is that copy. `PhysicalDisable` unregistered from
the VDD first and wrote `ENABLE = 0` after; the unregister is where the
VDD sends `DISPLAY_DRIVER_DISABLING` (function 26) and starts writing the
VGA planes from VRAM offset 0, and a refresh inside those 12 ms took them
into the held frame.

**The fix is the order, twice**: `PhysicalDisable` writes `ENABLE = 0`
before `VDD_DRIVER_UNREGISTER`, and the mini-VDD answers
`DISPLAY_DRIVER_DISABLING` itself (`driver_disabling_proc`) by turning
the linear mode off and forgetting any switch or message screen in
flight — the way back from a disable is the driver's own `Enable`. The
log reads `linear mode off`, then `d3dptvxd: display driver disabling`.

Driving a Portuguese Win98 shutdown by keys: `ctrl+esc`, `up`, `ret`,
`ret` (`d` selects "Documentos" first). A machine with the Voodoo 2 needs
`EXTRA="-device voodoo2"` in the harness, or Windows finds new hardware.

### 36. 3DMark2001 SE's demo: a freed surface, a swallowed fault, a lock left held

The demo froze on black after its loading screen. The frozen thread spun
in `DestroySurface32` → `cmd_lock_acquire` (`Sleep(0)`) on a lock held
by another thread that was **blocked**: it had taken a read fault in our
HAL inside `DrawPrimitives2`, reading a surface LCL the runtime had
freed and reused. The reader was `surf_colorkey_check`, which read the
colour key off a kept `SURF::lcl` pointer at every bind, and nothing
cleared that pointer when the runtime freed a surface. A handler in the
game's thread took the fault, and the lock was never released.

**The core keeps no pointer to an OS surface** (§19): registration takes
the key off the descriptor (`surf_key_snapshot`), a later key arrives
through `SetColorKey` → `surf_colorkey_set` (both layers claim the
colour-key caps), and the one thing lost is a key removed without a
`SetColorKey` call. `TextureGetSurf32` (DirectX 5 texture handles) keeps
its own list, filled by `TextureCreate32` and emptied by
`TextureDestroy32`. With it the demo plays to the end; XP's `CKTEST`
still passes 4 of 4.

**Still open, accepted for v1** (user decision, 2026-09-16): any fault
inside a callback leaks the command-window lock and freezes the session
until the process dies. The fix is an exception frame that releases the
lock on unwind, which would turn the next such bug into one failed call.

### 37. 3DMark2001 SE's white sky, ground and coat: no surface wider than the screen

Surfaces drew white where d3d8.dll bound **texture handle 0** — the
runtime had no video-memory copy of those managed textures. Every one of
them was 1024 wide, at a 640×480 desktop: **Windows 9x DirectDraw puts
no surface wider than the primary into video memory unless the driver
claims `DDCAPS2_WIDESURFACES`**. NT's driver always claimed it; the 9x
`d3dpt9dd.c` does now. A draw sampling a stage with handle 0 bound is
this symptom (§39 again).

The Lobby's frame rate under its debris physics was the guest's x87 at
PC=64, answered in doc 13 (patches 47–49).

### 38. 3DMark2001 SE's Nature: "device does not support bump normal maps"

3DMark's driver DLL probes six bump formats with `CheckDeviceFormat`,
and "normal" is **Q8W8V8U8** or W11V11U10. Q8W8V8U8 has no DDPIXELFORMAT,
so d3d8.dll creates it as a **FOURCC surface whose code is the
D3DFORMAT, 63**, and DirectDraw refuses a FOURCC not in the driver's list
before any callback — which was why XP's d3d8.dll made no video-memory
copy of it (doc 15). Both layers list FOURCC 63 now and size such a
surface (`fmt_fourcc_rows`); the host takes it as `R8G8B8A8_SNORM`.
BUMPTEST passes 10 of 10 on Win98 and the whole 3DMark2001 SE benchmark
runs to its score. **The NT half is not measured** (`xp-driver-test.sh
install` installed no driver on the `winxp-m7` overlay at the time; open
in `docs/00-status.md`).

### 39. 3DMark2001 SE's Pixel Shader ocean drew black: no cube map in video memory

The ocean's ps 1.1 reflection (`texm3x3vspec t3`) sampled stage 3 with
handle 0 bound (§37's symptom): the cube existed only as the runtime's
system-memory copy (`cube … not mirrored … (system memory)` in the log).
**9x DirectDraw creates a cube map in video memory only if the driver
claims `DDSCAPS2_CUBEMAP` in `DDMORESURFACECAPS.ddsCapsMore`**, answered
through `GetDriverInfo` for `GUID_DDMoreSurfaceCaps`, which NT's dxg
never asks and the 9x layer now answers. `ddflags=0x400000`
(`DDF_NO_CUBE`) withholds it with the cube caps. CUBETEST passes 9 of 9
on Win98.

## No executor, and WineD3D as the fallback

### 40. `no-exec` on 9x: the HAL claimed a Direct3D it did not have

With `-global d3dpt-vga.no-exec=on` (a host with no executor, doc 15)
`d3d_init` refuses, but `DriverInit` published the Direct3D globals and
callbacks anyway, so the `.drv` claimed `DDCAPS_3D` and the 3D surface
caps over a `D3DHAL_GLOBALDRIVERDATA` of zeros. The HAL now gates all of
it on `core.d3d`, as NT always did, and logs `d3dpthal: no Direct3D on
this host — DirectDraw only`. `NO_EXEC=1 tools/win98-driver-test.sh
<image> boot` checks that line, no `d3d global=`, and DDPROBE's
DirectDraw half intact. A DirectX 3–6 title then uses its software
renderer through the flip chain (Moto Racer: 299 flips in 5 s).

- **The crash that prompted this was Moto Racer without its disc**: it
  recurses until its stack is gone (a stack fault in KERNEL32 or
  `MOTO.EXE`) when the CD is not in the drive — with or without the
  executor. Check the disc before the driver.
- **`lpSetInfo` can go stale**: it points into DDRAW16, loaded only
  while some process has DirectDraw open, and the driver calls it from
  `Enable` at every mode set. `DDNEWCALLBACKFNS` with a null table takes
  it back, and the driver now clears it and checks the selector with
  `lar` before any call.

### 41. The monitor power-down: the screen is taken away, and the linear mode stayed on

Left idle, a machine showed a frozen desktop with §35's green band. The
log's last line was `d3dpt9x: switched out` with no mini-VDD call after
it, and the guest was idle and healthy (a key brought the desktop back).
**Windows' monitor power-down reaches the driver as a screen switch and
nothing else**: INT 2Fh AX=4001h, so `SwitchToBgnd` marks the PDEVICE
`BUSY` and the desktop freezes (§29), while the mini-VDD hears nothing
and `ENABLE` stayed on — scanning out a stale frame with the VGA planes
through its top 20 rows. `Control` logs each distinct escape once: a
power-down asks about escapes 0xc01 and 0x27 only, never
`SETPOWERMANAGEMENT` (0x1804), so there is no DPMS request to answer.

**The fix**: `SwitchToBgnd` writes `ENABLE = 0` — an unannounced switch
leaves the screen to the VGA core; in an announced one the mini-VDD has
already done it. `SwitchToFgnd` restores via `RestoreDesktopMode` as
before. `PWRPROBE.EXE` (`w9x/pwrprobe.c`) broadcasts `SC_MONITORPOWER`
on demand — an idle wait depends on the image's power scheme and proved
nothing in 12 minutes — and the pass is `switched out` followed by
`linear mode off` within a millisecond in the QEMU log, with
`NO_DRIVER=1` the control that must fail (`docs/testing.md` has the run
line).

**Still open**: an ACPI **standby** suspends the whole VM (QEMU
`SUSPEND`), and on wake nothing reprograms the adapter: a blank 720×400
text page, and the machine idles back into standby. The player does not
report `SUSPEND`/`WAKEUP` (`player/src/qmp.rs::is_notable`).

### 42. The WineD3D folder only works for the first DirectDraw program of the session

**On 9x the `DDRAW.DLL` beside a game is used only if that game is the
first program in the Windows session to touch DirectDraw.** Win9x keeps
one module per *name* machine-wide and `DDHELP.EXE` keeps DirectDraw
resident, so from the second DirectDraw program on the system
`ddraw.dll` is served whatever is in the folder. Measured with one
binary in one folder under `no-exec=on`: first in the session,
`D3D7TEST` gets `Wine D3D7 T&L HAL` and renders; after `DDPROBE` ran
from elsewhere, it gets this driver's `HAL caps 00000480 (no 3D)` and no
HAL device. A title's 3D setup offering no 3D adapter (FIFA 2000) is
this. XP resolves by full path per process and does not have it, which
is why `xp-fifa2000.bat` must rename those files *away*.

The stack itself is sound on 98 once reached: Wine's ddraw brings up a
HAL on the GL pass-through and `EBTEST` passes 5 of 5 through it.
Replacing `WINDOWS\SYSTEM\DDRAW.DLL` does not survive (System File
Protection objects at the next login — which also blocks `WIN.INI`'s
`run=` — and restores it from `SYSBCKUP`); preloading Wine's copy at
login does not either (an app-directory module never becomes the
machine's, even while resident). What works is redirecting the module
*name* — §43.

### 43. WineD3D as the machine's DirectDraw, decided at every login

`SETUP /I 7` on 9x ("WineD3D as this machine's DirectDraw") installs the
redirection, and `D3DPRE.EXE` in the Run key decides at every login
which way it points.

**The name.** `KnownDLLs` is read per `LoadLibrary`, so a value written
mid-session holds for every program started after it:

```
[HKLM\System\CurrentControlSet\Control\SessionManager\KnownDLLs]
"DDRAW"="ddrawme.dll"
```

**The files** (from `WINED3D\SYSTEM9X\` plus the per-game
`WINED3D\DDRAW\` set, one copy of each on the disc):

| file in `WINDOWS\SYSTEM` | what it is |
|---|---|
| `DDRAWME.DLL` | wine9x's `ddraw_98.dll`, a switcher choosing per caller between WineD3D and the real DirectDraw |
| `DDSYS.DLL` | this machine's own `DDRAW.DLL` with the `DDRAW.DLL` after `DDRAW16.DLL` inside it renamed (the `ddreplacer.c` edit, done by SETUP), so its 16-bit registration does not reach the switcher |
| `WINEDD.DLL`, `WINED3D.DLL` | WineD3D |
| `OPENGL32.DLL` | **our GL pass-through as the system OpenGL**; Microsoft's kept as `MSOGL32.DLL`, a spare of ours as `WGLPT32.DLL` for the helper to restore |

**OpenGL is replaced, not redirected**, and unconditionally. WineD3D
draws through the first `opengl32.dll` found and a machine-wide install
has no game folder to put ours first; redirecting `OPENGL32` through
`KnownDLLs` leaves WineD3D with no GL adapter (`HAL caps f5408668 (no
3D)`). The pass-through *is* the accelerated GL on these machines in
either mode, and our own Direct3D does not use GL. `GLPROBE.EXE`
confirms it (`GL_RENDERER` the host's card, never `GDI Generic`). The
file is staged and swapped at the restart since it may be loaded;
`opengl32.dll` has not been restored by SFP in any run.

**It depends on the device mapper**: the pass-through's `DllMain`
returns FALSE without `FXMEMMAP.VXD`, so a program importing opengl32
would not start. `SETUP /ALL` installs the mapper first (component 2); a
bare `/I 7` without it stops and says so.

**Which way it points is the host's, not the image's**: the same disk
runs on a host with the executor one day and without it the next.
`D3DPRE.EXE` asks the display driver through a private escape
(`D3DPT_ESC_HOSTINFO`, `d3dpt_esc.h`, answered from
`D3DPT_FB_REG_D3D_STATUS` — a program deciding about DirectDraw cannot
ask DirectDraw), writes the value when there is no executor and removes
it when there is, and logs to `WINDOWS\D3DPRE.LOG`. A driver that is not
ours answers nothing, which means no executor either.

**The check is `tools/wined3d-sys-test.sh`**: three boots on a raw copy
— install; `no-exec=on`, where a probe with no Wine DLLs of its own must
find `Wine D3D7 T&L HAL` *after* `DDPROBE` has loaded DirectDraw (545.5
fps); and the executor back, where the value must go and the probe be on
ours. The third boot matters most: a switch that never switched back
would leave every machine on WineD3D after one host without Vulkan. The
batch waits for `D3DPRE.LOG` with `WAITFILE.EXE`, because the Run key
and `WIN.INI`'s `run=` start together and under TCG the helper does not
always win.

All of this is WineD3D-in-guest, which ADR-018 retires in M15's last
step, once the host Wine executor has run real games — not before.

### 44. The fallback drew nothing on Linux: the frame a front-buffer flush presents

Under §43 FIFA 2000 (and §40's Moto Racer) played audio over a black
screen. **Wine's ddraw presents the primary by drawing into `GL_FRONT`
and flushing; it never swaps**, and the embed backend published frames on
a swap only on Linux and Windows (macOS had the hooks). Mesa accepts
`glDrawBuffer(GL_FRONT)` on a pbuffer with no error, so nothing
complained. The buffer hooks are shared in `embed/mglcntx_embed.c` now
(`install_buffer_hooks`: `fx_glDrawBuffer`, `fx_glReadBuffer`,
`fx_glFlush`, `fx_glFinish`), with only the mapping of the guest's
framebuffer 0 per OS; doc 12 has the design. A/B on the game: 206 frames
with the hooks, 29 ending black without. `embed-3d`
(`tools/embed-3d-test.c`) guards it with no guest: front buffer
selected, a magenta clear flushed, that frame required.

FIFA's pitch then draws black where the grass belongs — a WineD3D
rendering defect of the kind `tools/xp-wined3d-test.sh` records on XP
(`docs/testing.md`), not a presentation one.
