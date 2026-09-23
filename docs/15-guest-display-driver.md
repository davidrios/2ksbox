# 15. A real XP display driver (ADR-008, M7)

The XP guest side of doc 14: instead of DLL copies per game folder, a
Windows display driver pair that owns the adapter and speaks the
DirectDraw and Direct3D DDIs into the doc 14 transport and executor.
ADR-008 (doc 10) has the why; this doc is the how. The Win98 driver
shares the OS-independent `core/` described here (doc 19 §19; 9x
specifics are doc 19's), the protocol and the executor are doc 14, and
the track's loop and open work are `docs/tracks/m7-display-driver.md`.
Every test tool named here is described in `docs/testing.md`.

It came in three stages, which still name the parts: **M7a** the
framebuffer driver, **M7b** the DirectDraw DDI, **M7c** the Direct3D DDI
(a DirectX 7 HAL, grown into a DirectX 8 DDI with hardware T&L).
Today: register set **v5** (`D3DPT_FB_VERSION`), protocol **v13**
(`D3DPT_PROTO_VERSION`). FIFA 2000, Max Payne, Diablo, Moto Racer 1997,
GTA 2 and GTA Vice City run on it with no DLL in their folders.

## Shape (M7a)

```
guest (XP)                                          host (QEMU process)
 win32k.sys (GDI)                                    hw/d3dpt/d3dpt_vga.c  "d3dpt-vga"
   └ d3dptdisp.dll  display driver (winddi.h)          ├ PCI 1234:3d00, class VGA, stdvga ROM
       GDI draws into mapped VRAM                      ├ BAR 0 VRAM 128 MiB ─── DisplaySurface points here
       DirectDraw / Direct3D DDIs                      │   (top 64 MiB: the M7c command window)
 videoprt.sys                                          ├ BAR 1 registers (d3dpt/d3dpt_fb.h)
   └ d3dptvid.sys   video miniport (video.h)           │   mode table, WIDTH/HEIGHT/BPP/PITCH/OFFSET,
       finds the PCI device, maps the BARs,            │   ENABLE, FRAMES, DEBUG (char → QEMU log)
       reads the host's mode table, sets modes         └ VGA core (hw/display/vga.c) while ENABLE = 0:
 vga.sys / VGA BIOS until the driver is installed         BIOS text, vga.sys before install, BSODs
                                                     embed listener: same surface pointer → player
```

The sources are `guest-tools/src/d3dptvid/`: the NT layer `nt/`
(miniport `d3dptvid.c`, display DLL `d3dptdisp.c`, INF) over `core/`
(DP2 walker, surface table, caps, flip chain), shared with Win98.

- **The adapter is a real VGA.** QEMU's standard VGA core with the Bochs
  VBE ports, so SeaBIOS' `vgabios-stdvga.bin` boots it (the ROM takes
  BAR 0 as the LFB for any vendor id), XP's inbox `vga.sys` runs the
  desktop at 800×600×4 before our driver exists, and blue screens and
  shutdown text work because `HwResetHw` returns FALSE and videoprt's
  int10 puts the core back into mode 3. Vendor/device 1234:3d00 (the
  QEMU/Bochs pseudo vendor, our id), matched as `PCI\VEN_1234&DEV_3D00`.
- **The register BAR is the paravirtual part** (`d3dpt/d3dpt_fb.h`, one
  header for the device and both drivers). The host owns the **mode
  table**: the miniport reads MODE_COUNT and walks MODE_SEL → MODE_W/H/
  BPP/HZ, so what Display Properties offers is decided on the host —
  today a static list of 7 sizes (640×480 to 1600×1200) × {60, 75, 85} Hz
  × {8, 16, 32} bpp = 63 modes. A mode switch writes WIDTH/HEIGHT/BPP/
  PITCH/OFFSET and ENABLE = 1; ENABLE = 0 hands the console back to the
  VGA core. DEBUG takes one character per write and the device prints
  lines to the QEMU log (`d3dpt-vga: guest: …`): the only debugger kernel
  code has here, and enough.
- **No copy inside QEMU.** While ENABLE is set the device's
  `DisplaySurface` is created *over the VRAM bytes* at the guest's
  offset/pitch (`qemu_create_displaysurface_from`), dirty lines come from
  the memory dirty log (`DIRTY_MEMORY_VGA`, as `bochs-display` does), and
  the embed listener hands that pointer plus the dirty rectangles to the
  player. 8 and 16 bpp modes (and a 32 bpp one under a gamma ramp) are
  converted per dirty span into an x8r8g8b8 shadow, since the listener
  takes one format. The player's upload of the dirty rectangle is the one
  copy left: zero-copy to the GPU from guest RAM is impossible with a
  host-visible-only import (M3's dma-buf ring is for host-rendered 3D).
  `-device d3dpt-vga,full-frames=on` converts the whole frame every
  refresh instead, the A/B for a picture in stale bands (doc 21).
- **Mode switches do not flash.** A switch is RESET → SET_MODE a few ms
  apart; the device holds the last linear frame for 250 ms (wall clock —
  counted in refreshes it lasted 45 s headless, where an idle console
  refreshes every 3 s) after ENABLE goes 0 before showing the VGA core
  (`D3DPT_FB_VGA_GRACE_MS`), and the miniport zeroes the new mode's frame
  buffer unless `VIDEO_MODE_NO_ZERO_MEMORY`. A 16 bpp desktop's held
  frame is the shadow copy, so VGA writes to VRAM offset 0 before ENABLE
  goes 0 land in it (doc 19 §35). While the linear mode is off the device
  logs the VGA core's mode registers once per change (`d3dpt-vga: vga
  core cr1=… sr4=…`): how a headless run tells Mode X, 13h and a
  text-mode blue screen apart.
- **The display driver is the DDK "framebuf" shape.** `DrvEnablePDEV`
  picks the miniport mode matching the DEVMODE and fills GDIINFO/DEVINFO;
  `DrvEnableSurface` sets the mode, maps VRAM and gives GDI a surface over
  it with no drawing hooks: GDI draws every pixel in software, straight
  into guest VRAM. `DrvAssertMode(FALSE)` resets to VGA for full-screen
  consoles and the logon desktop switch. The register page reaches the
  display driver through `IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES`.
- **Install** is `DRIVER\DRVINST.EXE [-reboot]` on the guest-tools ISO
  (`SETUP` runs it): newdev's `UpdateDriverForPlugAndPlayDevices` with
  `INSTALLFLAG_FORCE` for the hardware id (what `devcon update` does),
  after setting the driver-signing policy to ignore. XP SP3 shows the
  "has not passed Windows Logo testing" dialog anyway — the stored policy
  is hash-protected and only the Control Panel changes it — **once per
  unsigned file** (the miniport, then the display DLL after its copy,
  minutes apart under TCG). A watcher thread in DRVINST presses each
  dialog's first button for as long as the install call runs (`alt+c` by
  hand). `SETMODE.EXE [w h bpp [hz]]` lists / switches modes for scripts.
  After the restart XP starts at 640×480×32@60 and remembers what is set.
- XP runs far faster under **KVM** (`-accel kvm -cpu pentium3`) than
  TCG; Linux hosts should use it, TCG is the Apple Silicon path.

### The register set and its versions

| Version | Adds |
|---|---|
| 1 | mode table, linear mode, ENABLE, FRAMES, DEBUG, DDFLAGS |
| 2 | the Direct3D command window: CMD_OFFSET, DOORBELL, D3D_STATUS |
| 3 | 8 bpp: the 256-entry PALETTE block (0x400), `CAP_BPP8` |
| 4 | the hardware cursor: CURSOR_* |
| 5 | gamma: the GAMMA block (0x800), GAMMA_ENABLE (0xb4), `CAP_GAMMA` |

The BAR is `valid.min_access_size = 4`: every access is 32-bit (which
matters to the 16-bit 9x driver, doc 19).

### Newer register sets are accepted

The miniport, the 9x display driver and the mini-VDD accept **any
version at or above their own**, and compare `D3DPT_FB_MAGIC` exactly.
The register set has only ever grown — each version new registers and a
CAP bit, nothing reinterpreted — so a newer adapter is the old one plus
registers the driver never touches. That is the rule in `d3dpt_fb.h`: a
bump only adds, and a change that must reinterpret a register is a new
MAGIC. It is what lets an installed guest survive a QEMU update.

Drivers built before 2026-09-12 want the version exactly. On XP such a
driver refuses the adapter and the desktop comes up on plain VGA; a
Windows 98 image whose `SYSTEM.INI` says `*DisplayFallback=0` dies at
boot with "Windows protection error" (claude98, a 2026-09-08 driver on
the v5 adapter). Boot such a machine on the Cirrus once and run the
ISO's `SETUP /ALL`. `-device d3dpt-vga,fb-version=N` makes the adapter
report another version, which is how the acceptance is checked.

### Device properties

| Property | Effect |
|---|---|
| `ddflags=N` | the DDFLAGS register: feature A/B bits for the driver, no reinstall ("The ddflags bits" below) |
| `no-exec=on` | D3D_STATUS answers `NO_EXEC`: a host with no executor at all |
| `d3d9=auto\|dxvk\|system` | the Direct3D 9 the executor runs on; `system` is Windows' own (ADR-007's second amendment) |
| `exec=wine` | the executor in a Wine process on the host (ADR-018, M15, doc 14) |
| `fb-version=N` | the register set version reported |
| `full-frames=on` | whole-frame conversion every refresh |

The executor properties model hosts, not devices, so the adapter only
hands them to the loader (`d3dpt/hw/d3dpt_exec_load.c`, shared with
doc 14's SysBus device, which then answers the same):

- **`no-exec=on`** (`-global d3dpt-vga.no-exec=on` in the machine form's
  Extra QEMU arguments; `NO_EXEC=1 tools/xp-driver-test.sh`) is a Linux
  or macOS host below the Vulkan 1.3 floor *with no Wine*. The driver
  keeps its whole DirectDraw half — modes, flip chain, cursor, gamma,
  palette — and offers no Direct3D, so a game falls back as it would on
  such a host: to the runtime's software device or to WineD3D staged next
  to it (`SETUP /GAME 4`, doc 04). The loader refuses **before the
  executor library is opened**, so `d3d9=` is never read and no backend is
  tried. The QEMU log says `d3dpt: no-exec=on: no Vulkan 1.3 device on
  this host` and the driver `d3dptdisp: no Direct3D executor on the host`.
  **Not `ddflags=0x20`** (`DDF_NO_D3D`): that makes the *driver* decide
  and never reads D3D_STATUS, so it proves nothing about the path a real
  below-floor user takes. (The 9x counterpart: doc 19 §40.)
- **`exec=wine`** is a Linux or macOS host below the floor that has a
  Wine: the guest sees READY as on any other host. On a host with Vulkan
  it is the A/B (`EXEC=wine tools/xp-driver-test.sh`).
- **`d3d9=`** is passed to the executor as `D3DPT_D3D9`; an explicit
  `D3DPT_D3D9` in the environment wins, which is how the two backends are
  compared on one Windows host. The machine form's **Direct3D** row
  writes it, resolving `auto` from the launcher's own Vulkan probe. The
  log names the library opened and the adapter that answered.

## Building kernel-mode PE files with GCC

`guest-tools/build-driver.sh` (also run by `build-wrappers.sh`, which
stages the result as `DRIVER\` on the ISO). mingw-w64 ships the DDK
headers and import libraries under its permissive licence; **nothing from
Microsoft's DDK is used**.

- `-nostdlib -shared -ffreestanding -fno-stack-protector
  -mno-stack-arg-probe -fno-asynchronous-unwind-tables`, subsystem native,
  image base 0x10000, OS/subsystem version 5.1, entry `_DriverEntry@8`
  (miniport) / `_DrvEnableDriver@12` (display DLL, exported undecorated
  through a .def with `--kill-at`). `-lgcc` for helpers.
- The miniport links `libvideoprt.a` plus the few ntoskrnl imports of the
  cached VRAM mappings (below), the display DLL **only** `libwin32k.a`;
  the build script fails on any other import.
- GCC emits `memcpy`/`memset` calls for struct copies even freestanding
  and neither port driver exports them: `kcrt.c` carries byte loops,
  compiled with `-fno-tree-loop-distribute-patterns` so they are not
  turned back into calls to themselves.
- Header sets are not mixable. The miniport takes `ntdef.h`, `dderror.h`,
  `devioctl.h`, `ddk/miniport.h`, `ntddvdeo.h`, `ddk/video.h` — **not**
  `ntddk.h`, which conflicts with `miniport.h` (and with
  `_WIN32_WINNT=0x0501` mingw's `wdm.h` does not compile; leave the
  default). The display DLL takes `windef.h`, `wingdi.h`, `winddi.h`,
  `devioctl.h`, `ntddvdeo.h`.
- mingw's `winddi.h` includes `ddrawint.h` / `d3dnthal.h`, which mingw
  does not ship: `guest-tools/src/d3dptvid/ddk/` vendors ReactOS'
  public-domain `ddrawint.h` (+ `dvp.h`) and a **self-contained
  `d3dnthal.h`** — the DDK's pulls in the user-mode `windows.h` through
  `d3dtypes.h`/`d3dcaps.h`, so the few Direct3D types the DDI structures
  use are spelled out with the DDK's layouts. Transcribing constants by
  hand is where this goes wrong silently: the `DDBD_*` bit-depth flags
  count *down* (`DDBD_16` 0x400, `DDBD_24` 0x200, `DDBD_32` 0x100), and
  having them the other way made `CreateDevice` fail with
  `DDERR_INVALIDPIXELFORMAT` at 32 bpp only (`dwDeviceRenderBitDepth` is
  what the runtime checks the target against). The DP2 token layouts only
  the host interprets (`d3dpt/exec/d3dpt_exec_ddi.cpp`).
- The same `-march=pentium3` floor and ISA/UCRT checks as the wrappers.

## M7b — the DirectDraw DDI

What `ddraw.dll` → `dxg.sys` sees behind the display driver:

- **The primary is a device surface GDI still draws on.**
  `EngCreateDeviceSurface` + `EngModifySurface(hsurf, hdev,
  HOOK_SYNCHRONIZE, MS_NOTSYSTEMMEMORY, dhsurf, pvScan0 = VRAM, pitch)`
  with a no-op `DrvSynchronizeSurface`. `HOOK_SYNCHRONIZE` is not
  optional: without it win32k refuses the `EngModifySurface` (the driver
  tries the variants in order and logs the one that took; an engine
  bitmap is the desktop-only fallback).
- **VRAM behind the primary is one linear heap** (`DrvGetDirectDrawInfo`:
  `VIDMEM_ISLINEAR`, `fpStart` = primary size rounded to 4 KiB, ending
  16 KiB below the command window, where the cursor image lives). dxg's
  heap manager places every DirectDraw surface; the driver never
  allocates.
- **Callbacks:** `DdMapMemory` (VRAM into the game's process through the
  miniport's `IOCTL_VIDEO_SHARE_VIDEO_MEMORY`), `DdCanCreateSurface`
  (the formats the host mirrors), `DdFlip` (one write of the target's
  VRAM offset into OFFSET: a real page flip, nothing copied),
  `DdWaitForVerticalBlank` / `DdGetFlipStatus` / `GetVerticalBlankStatus`
  on FRAMES (next section), `DdGetBltStatus` = always done, `DdLock` /
  `DdUnlock` for the Direct3D readback and dirty tracking, a
  `DdCreateSurface` that sizes compressed textures, volumes and buffers
  (M7c), and a never-called `DdBlt`. `GetDriverInfo` answers
  `GUID_NTCallbacks` (SetExclusiveMode, FlipToGDISurface) and the
  Direct3D GUIDs, and refuses the rest.
- **Mappings are cached, not write-combined.** videoprt's
  `VideoPortMapMemory` maps frame buffers uncached or write-combined —
  right for a PCI aperture, wrong for RAM: reads from such a mapping run
  at tens of MB/s, and GDI scrolls, the HEL's copies and every `Lock`
  read do exactly that. The miniport maps VRAM itself: `MmMapIoSpace
  (MmCached)` for the kernel view GDI draws through, and a cached view of
  `\Device\PhysicalMemory` (`ZwMapViewOfSection`, what videoprt does
  inside minus `PAGE_NOCACHE`) for DirectDraw's user-mode `Lock`. The
  register BAR stays an uncached videoprt mapping. QEMU reads guest RAM
  coherently, so a cached guest mapping is correct under KVM and TCG.
  DDTEST's windowed offscreen → primary `Blt` went from 29.9 to 305 fps
  with it, the 16 bpp flip chain from 31 fps (uncached) to thousands.
- **Test:** `DRIVER\DDTEST.EXE [w h bpp] [frames] [-windowed]`: caps,
  an exclusive flip chain (Lock/Unlock pattern + `Blt` colour fill +
  `Flip`) or a windowed offscreen surface blitted to the primary, fps,
  `ddtest.log` + `.bmp`; `tools/xp-driver-test.sh <image> ddtest` runs
  8, 16, 32 bpp and windowed. Every surface reports `VIDEOMEMORY |
  LOCALVIDMEM`, and the QEMU log shows `scanout offset 0 -> 614400 -> 0 …`
  per flip.

### dxg's caps rules

dxg validates the HAL after enabling it and, on any of these, drops the
**whole** HAL — `GetCaps` answers `DDCAPS_NOHARDWARE`, surfaces go to
system memory, no Direct3D — with no message anywhere. Each was found by
`ddflags` bisection (one boot per variant, DDTEST and the QEMU log tell)
and the disassembly of the image's `dxg.sys` / `ddraw.dll` (pulled out
with `qemu-img convert` + `7z`):

- `DDCAPS_GDI` in `dwCaps` (`ddflags=0x10` is the repro), as are
  `DDCAPS_BANKSWITCHED` and `DDSCAPS_MODEX`.
- `ddCaps.dwPalCaps != 0`, or palette callbacks ("8 bpp palettized
  modes").
- Colour-key caps without a `Blt` callback ("Palettized textures and
  colour keying"; `ddflags=0x20000`).
- Direct3D appearing or disappearing across a mode switch: the object
  rebuild after `SetDisplayMode` fails. Direct3D is therefore offered in
  every mode, 8 bpp included (the runtime refuses `CreateDevice` on a
  palettized primary by itself).
- A device claiming `DRAWPRIMITIVES2EX` or `HWTRANSFORMANDLIGHT` without
  a `GetDriverState` callback ("M7c — the Direct3D DDI").

And `dwCaps` claims **no blit caps** (`DDCAPS_BLT` and friends): the HEL
does every blit in user mode on the cached VRAM mapping ("Blit caps and
the HEL" for why that is not a choice).

### The flip chain's vertical blank

Titles of the era pace themselves by the flip chain: on a real card the
second `Flip` of a double-buffered chain cannot be set up until the first
has been scanned out, and that wait — not a timer in the game — holds a
1997 racer at 60 frames a second. Without it Moto Racer ran several times
too fast and DDTEST's chains at thousands of fps. **A game that runs far
too fast is a missing frame limiter, not a clock bug.**

- **The device (`d3dpt_vga.c`, `FRAMES`)** counts periods of the mode's
  `HZ` since `ENABLE` on the host clock (60 Hz if `HZ` is unset). It must
  not follow whoever pulls frames: a headless run has no display client,
  so `graphic_hw_update` is never called.
- **The driver (`core/core_flip.c`).** `DdFlip` remembers `FRAMES` at the
  flip; until it moves the flip is in the air, a second `DdFlip` waits
  (`DDFLIP_WAIT`) or returns `DDERR_WASSTILLDRAWING`, and
  `DdGetFlipStatus` answers `DDERR_WASSTILLDRAWING` for both
  `DDGFS_CANFLIP` and `DDGFS_ISFLIPDONE`. Every wait is bounded at 50 ms,
  so a device that stops counting cannot stop the guest, and pauses
  between polls: the register read and XP's performance counter are both
  exits.
- **`GetVerticalBlankStatus`** (`WaitForVerticalBlank(DDWAITVB_I_TESTVB)`)
  must sometimes say yes: a title's `while (!in_vb)
  GetVerticalBlankStatus(&in_vb);` never ends otherwise. The adapter has
  no beam position, so `vb_test` says yes once a frame, to the first
  question after `FRAMES` moved: `while (!in_vb)` ends at the next frame
  and `while (in_vb)` at the next question. DDTEST and `ddprobe` count
  the answers in a 500 ms loop (about 30 at 60 Hz).

With it, DDTEST's 8/16/32 bpp chains and `D3D7TEST` run at 60 fps (the
latter's frame still byte-identical to the host oracle). A windowed blit
to the primary is not a flip and is not throttled, as on real hardware.
`ddflags=0x8000` (`DDF_NO_VSYNC`) makes flips complete instantly: the A/B
for a suspect title and how throughput is measured (DDTEST 640×480: 3846
fps at 16 bpp, 4762 at 32, 1132 at 8 with a palette every frame, KVM).

The device prints the guest's real frame rate every 5 s while flips
happen (`d3dpt-vga: 299 page flips in 5.0 s (59.6/s)`), driven by the
flips themselves so headless and player runs agree. **No such line while
a game runs means it blits to the primary instead of flipping**: the
vertical blank cannot pace it, and if it is too fast the cause is the
guest CPU, not the display path. `FRAMES` is a clock, not the player's
present: a game gets the refresh it asked for, but not in phase with the
host's swapchain (open item).

### A DirectX 6 title's flip chain

What GTA 2 (1999, `IDirectDraw4` / `IDirect3D3`, 640×480×16) taught about
flip chains, all of which apply to every title:

- **A flip swaps roles, not memory.** `DdFlip` gets `lpSurfCurr` (front)
  and `lpSurfTarg` (back); the driver scans out `Targ`'s memory.
  Afterwards dxg does *not* exchange the two objects' `fpVidMem`: the
  handles keep their VRAM, `DDSCAPS_PRIMARYSURFACE` moves to the object
  now displayed, the application's `back` pointer means the other object,
  and dxg tells the driver with a `CreateSurfaceEx` pair (same offsets,
  swapped caps). The runtime's `SETRENDERTARGET` alternates the handle
  (2, 1, 2, 1 …) accordingly. **Never re-register the chain in
  `DdFlip`**: a first cut swapped the offsets there, so from the second
  frame the host rendered into the *displayed* buffer on alternate frames
  and a `Lock` of the back buffer found black or the previous frame —
  invisible to a golden compare of identical frames. `DdFlip` logs its
  first eight calls (`d3dptdisp: flip curr H at OFF targ H at OFF`).
  Should a runtime ever swap memory, `DdFlip` and a `DdLock` of a target
  re-register a surface the host knows at another offset
  (`d3d_register_moved`, silent), and the host drops a moved surface's
  shadow ("Untracked writes") so the first readback is whole.
- **The back buffer may never be announced.** A DirectX 7 interface's
  chain gets one `CreateSurfaceEx` per member; GTA 2's DX6 chain arrived
  as its root alone, so `SETRENDERTARGET 2` was unknown to the host and
  every other frame showed the buffer nobody drew. `DdCreateSurfaceEx`
  walks the surface's attach list (`d3d_register_chain`: the ring of a
  flip chain, an attached Z buffer; mip levels stay with their texture)
  and registers each member the host does not know, or knows at another
  offset; `D3dContextCreate` and `D3dSetRenderTarget` do the same for
  their targets. The DDK samples walk the list for the same reason.
- **Contexts outlive their PDEV.** A game's exclusive mode switch gives
  GDI a new PDEV, and its switch back another, *before* dxg's
  `ContextDestroyAll` arrives. With the context table in the PDEV no
  `CTX_DESTROY` reached the host, and the next run's `CTX_CREATE` of
  handle 1 came back `BAD_HANDLE` — `CreateDevice` failed on the game's
  second start. The table and its count are globals, like the surface
  table, and the host replaces a context re-created under an open handle
  (`ddi: context N still open, replaced`).
- **The white menu text: the legacy blend.** GTA 2 picks its blend with
  the DirectX 5 `TEXTUREMAPBLEND` render state (`MODULATEALPHA`) once,
  *before* any texture is bound, and the DX6 runtime passes it to a DX7
  driver as it is (it does not translate it into stage states). Mapped
  once, it meant "no texture: the diffuse alone", and the glyph textures
  bound later drew as white boxes. The executor keeps the legacy blend
  in effect as two flags, `legacy_cop` / `legacy_aop`:
  `TEXTUREMAPBLEND` / `TEXTUREHANDLE` set both, the app's own `COLOROP`
  ends the first and `ALPHAOP` the second, its ARGs end neither (Crimson
  Skies sets `COLORARG2 = DIFFUSE` after the blend, doc 19 §28), and every
  bind of stage 0's texture re-evaluates the halves still in effect.
  `d3dpt-dp2-test` covers both orders.
- Not ours: pressing Enter during GTA 2's Bink intro kills the game
  (`c0000095` in `binkw32!BinkGetSummary`); it does the same on XP's
  inbox Cirrus driver with no Direct3D. Skip the intro at the logos.

### 8 bpp palettized modes

The late-90s 2D titles (Diablo, StarCraft, Age of Empires, Caesar 3) set
640×480×8 through DirectDraw and animate the palette. Every size is
offered at 8 bpp, across four layers:

- **Device** (register set v3): `BPP = 8` and the 256-entry `PALETTE`
  block (x8r8g8b8, 0x400..0x7fc). The scanout is a pixman `c8` view of
  VRAM through a `pixman_indexed_t` from the registers, converted into
  the shadow per dirty span; a palette write repaints the whole frame at
  the next refresh. Each entry write is one MMIO exit: a full 256-entry
  animation costs ≈0.5 ms under KVM — fine for 20–30 palette updates a
  second; a RAM-backed palette page is the optimisation if a title needs
  more.
- **Miniport**: 8 bpp entries carry `VIDEO_MODE_PALETTE_DRIVEN |
  VIDEO_MODE_MANAGED_PALETTE`, 8 bits per gun;
  `IOCTL_VIDEO_SET_COLOR_REGISTERS` writes the block.
- **Display driver, GDI**: `GCAPS_PALMANAGED | GCAPS_COLOR_DITHER`, a
  `PAL_INDEXED` default palette (the 20 system colours at 0–9 and
  246–255, a 6×6×6 cube, 20 greys), `BMF_8BPP`, `ulNumColors = 20`,
  `ulNumPalReg = 256`, `HT_FORMAT_8BPP`, and `DrvSetPalette` writing the
  registers directly. XP's desktop at 800×600×8 comes up in the classic
  theme, the wallpaper dithered through the system palette.
- **Display driver, DirectDraw**: `ddpfDisplay = DDPF_RGB |
  DDPF_PALETTEINDEXED8`. **`dwPalCaps` stays 0 and there are no palette
  callbacks**: on NT the primary's palette is GDI's, so
  `IDirectDrawPalette::SetEntries` on it lands in `DrvSetPalette`, and
  the DDK header's `DdCreatePalette` / `DdSetEntries` / `DdSetPalette`
  are dead on XP (claiming them drops the HAL). `DDPCAPS_8BIT |
  DDPCAPS_ALLOW256` palettes work without any of it.
- **Test:** `DDTEST 640 480 8 300` rotates a four-ramp palette by one
  entry per frame; `ddtest.bmp` is written through the palette.
  `tools/xp-diablo.sh install|play` takes the retail Diablo from its
  installer to Tristram at 640×480×8, palette-cycled title flames
  included (the dungeon, sound and TCG timing are unrun).

A 320×200 game on Win98 wants DirectDraw's own Mode X, not a driver
mode, which is why no 320-wide mode is listed (doc 19 §30).

### The hardware cursor

Without pointer hooks GDI paints a software pointer into VRAM, erased and
redrawn around every drawing operation and always into the GDI primary —
under a flip chain one buffer of the two, so it blinks every other frame
in a full-screen title. Register set v4 is the sprite every card of the
era had:

- **Driver.** `DrvSetPointerShape` takes GDI's pointer (a 1 bpp AND/XOR
  mask, or a colour surface with a translation object; `SPS_ALPHA` marks
  32 bpp with alpha) and writes it as a8r8g8b8 into the 16 KiB above the
  DirectDraw heap, then CURSOR_ADDR / W / H / HOT_X / HOT_Y and
  CURSOR_DEFINE. Monochrome: AND 1 + XOR 0 is transparent, AND 0 black or
  white by XOR, AND 1 + XOR 1 ("invert") black — a sprite cannot invert.
  Colour pointers in another format go through a 32 bpp engine bitmap and
  `EngCopyBits`. Anything over `D3DPT_FB_CURSOR_MAX` (64) is
  `SPS_DECLINE`d and stays GDI's. `DrvMovePointer` writes CURSOR_X / Y and
  CURSOR_ENABLE (x = −1 hides); `GCAPS_ASYNCMOVE`; `DrvAssertMode(FALSE)`
  hides it before the VGA text.
- **Device.** CURSOR_DEFINE reads the image into a `QEMUCursor` and
  `dpy_cursor_define`s it; X / Y / ENABLE writes are `dpy_mouse_set`.
  Nothing is composited, so a QMP screendump shows no cursor, as with a
  real sprite; the log has the first defines and moves. The sprite is
  reported hidden whenever ENABLE is 0 (every ENABLE write re-runs
  `fb_cursor_move`) and a reset drops the shape: a VGA screen has no
  hardware cursor, and without this the player drew Windows' pointer over
  full-screen DOS games, XP's full-screen console and blue screens (doc 19
  §29). **Never `dpy_cursor_define(con, NULL)`**: QEMU's console takes a
  reference on the cursor unconditionally (`cursor_ref`), so NULL
  segfaults the player; "no cursor" is a hidden 1×1 transparent one
  (`fb_cursor_clear`).
- **Player.** With the USB tablet the host window's cursor *is* the
  guest's shape while over the image (no compositing, no latency), hidden
  when the guest hides it. With a relative mouse (PS/2, the player's
  grab) the host pointer is nowhere near the guest's, so the player
  composites the sprite into the frame, and a move alone republishes the
  frame. The headless dump takes that path, so a `PLAYER_DUMP_OUT` frame
  shows the cursor. A guest without a hardware cursor (Cirrus, vga.sys)
  keeps the software pointer in the frame.
- **Hidden behind a flip chain.** Windows keeps its pointer enabled
  behind an exclusive-mode game that never hides it (its arrow was drawn
  over Moto Racer's own). The adapter therefore hides the sprite from a
  flip chain's first page flip, whatever CURSOR_ENABLE says — a
  page-flipping game draws its own pointer, and GDI's would be wiped by
  the first flip anyway (`cur_flip_hidden`). It comes back at the next
  mode set, **or** after 2 s of guest time with no flip while the
  desktop's page is on screen: a game running at the desktop's own mode
  sets no mode on the way out (DirectDraw calls SetMode only for a
  change), which left the pointer hidden for good after 3DMark 99. A game
  idle on its other page stays hidden; one idle on the desktop's page (a
  loading screen) shows the pointer until its next flip. While a Voodoo 2
  has the monitor the cursor is hidden too (patch 66). The adapter does
  this, so both driver families get it; the `voodoo-guest-d3dpt` check
  guards it.

### Gamma ramps

A ramp where a RAMDAC would have one (register set v5):

- **The adapter**: a 256-entry x8r8g8b8 `GAMMA` block (0x800),
  `GAMMA_ENABLE` (0xb4), `D3DPT_FB_CAP_GAMMA`. The tables are made at the
  `GAMMA_ENABLE` write, not at the next refresh (which a headless run
  never makes). It is applied per dirty span to the shadow the 8/16 bpp
  modes convert into; a 32 bpp mode moves onto a shadow only while the
  ramp changes anything, so the identity ramp GDI loads at every mode set
  costs no copy. VRAM never holds ramped pixels: `GetFrontBuffer` and
  every readback see the pixels as drawn, a screendump and the player see
  the ramp. The log says `gamma ramp on` / `off`; a reset turns it off.
- **The XP driver** implements `DrvIcmSetDeviceGammaRamp` with
  `GCAPS2_CHANGEGAMMARAMP`: GDI's `SetDeviceGammaRamp`, DirectDraw's
  gamma control and Direct3D 8's `SetGammaRamp` all arrive there. It
  writes the high byte of each of the three 256-word ramps, then
  `GAMMA_ENABLE`, and claims `DDCAPS2_PRIMARYGAMMA`. The core claims
  `D3DCAPS2_FULLSCREENGAMMA` only when its layer loads ramps
  (`d3dpt_core.gamma`); Windows 98's layer has no ramp path yet.
  `ddflags=0x10000000` (`DDF_NO_GAMMA`) takes it out; the GDI cap stays
  and its entry refuses.
- **Test:** `GAMMATEST` holds a mild ramp (blue at 3/4 — GDI range-checks
  ramps) over a mid-grey frame; `tools/xp-driver-test.sh <image> gamma`
  waits for `gamma ramp on`, takes a screendump, waits for `off`, takes
  another: the centre pixel reads 80 80 60, then 80 80 80.

### Untracked writes — GDI on a DirectDraw surface

The contract between driver and executor is that the guest's writes to a
render target's VRAM are announced by `VRAM_DIRTY` (from `DdUnlock` and
the driver's own copies), the host uploads a dirty target before drawing,
and the host frame is read back into VRAM at EndScene / Lock / Flip. Two
ways a guest writes a surface never pass through `DdLock` / `DdUnlock`:

- **`GetDC` on a DirectDraw surface** is `NtGdiDdGetDC` → dxg, which
  builds a GDI surface over the DirectDraw surface's memory. A driver
  exporting `DrvDeriveSurface` is asked for that surface; ours does not,
  so GDI's `TextOut` / `BitBlt` write straight into VRAM with no driver
  callback on either side.
- **The engine's software cursor**, painted into the primary (576 pixels
  a move at 24×24) — in a flip chain the next frame's back buffer.

Moto Racer draws every 2D panel of its bike-selection screen and its
HUD's text the first way; the readback of the host frame overwrote them
every frame (the screen showed the bike and nothing else, while a click
on the invisible Start button still worked). `DrvDeriveSurface` would
not fix it — GDI does not say when it is done with a derived surface, so
a readback could overtake a `TextOut` in progress — so the executor keeps
a **shadow of each render target's VRAM** as of the last moment host and
VRAM agreed (after every upload and readback) and uses it twice a frame:

1. Before the frame's first draw (`bind_ctx`), a target that is not
   dirty is compared with its shadow; a difference is an unannounced
   guest write and the target is uploaded as if it had been.
2. At the readback, pixels that differ from the shadow are the guest's
   writes since the draws began: they are **kept over the host frame**,
   and the target is marked dirty so the next frame starts from them.

A full-target `Clear` skips the check as it skips the upload. The cost is
one `memcmp` of the target per frame (per row first, per pixel only on
rows that differ) and, while a title writes after its scene, one upload
per frame; Moto Racer stays at 120 frames/s under KVM. The device log
counts them (`ddi: … N untracked guest pixels in 5.0 s`, and the first
eight events). `DrvDeriveSurface` stays on the list as an optimisation,
not a correctness item.

### Blit caps and the HEL

**On XP a declined `DdBlt` is not a HEL fallback.** With blit caps
claimed (`DDCAPS_BLT | BLTSTRETCH | BLTCOLORFILL …` in any of the caps
sets), dxg routes every blit to the driver and a `DDHAL_DRIVER_NOTHANDLED`
reaches the application as `E_NOTIMPL` (DDTEST's windowed
`Blt(DDBLT_COLORFILL)` failed with 0x80004001). Whatever the 9x DDK
says, claiming blit caps on NT means writing the blitter — copy, colour
fill, stretch, source colour key, ROP — in the display DLL or on the
host. The HEL's user-mode copies on cached VRAM are fast enough for the
2D titles seen so far, so the driver claims none. The `Blt` callback is
still registered (colour-key caps need one, and without `DDCAPS_BLT`
nothing reaches it; `DDF_CKEY_NOBLTCB` leaves it out) and logs any call
with its rectangles. Windowed multisampling is the one feature that
waits on a blitter ("Multisampling").

FIFA 2000's intro videos play at 320×240 in the middle of the 640×480
mode; they were not a caps question — with the caps claimed nothing
blitted at all (the game writes decoded frames through `Lock`). Open
until someone sees them full-screen on a known configuration.

## M7c — the Direct3D DDI

The DX7 HAL behind the display driver, on the doc 14 protocol and
executor. The adapter's top 64 MiB of VRAM is a command window in exactly
the SysBus device's layout (`d3dpt_proto.h`: header page, records, return
area), so the guest encoder `d3dpt_enc.h` and `d3dpt_exec_submit` work
unchanged: DOORBELL submits the window and the host runs the batch
synchronously inside the write, D3D_STATUS says whether the host has an
executor (reading it loads the library), CMD_OFFSET where the window is.
The executor library is loaded once per process; each device has its own
instance.

```
guest (XP)                                      host
 ddraw.dll / d3dim.dll / d3d8.dll                d3dpt-vga: VRAM [heap | 64 MiB window]
   └ dxg.sys ── DDI ──> d3dptdisp.dll              DOORBELL ──> libd3dpt_exec (d3dpt_exec_ddi.cpp)
        CreateSurfaceEx → VRAM_SURFACE record        surface handle → DXVK texture / render target
        ContextCreate  → CTX_CREATE                  d3d9 device, SetRenderTarget + depth
        DrawPrimitives2 → DP2 record (tokens+verts)  token interpreter → Draw*PrimitiveUP etc.
        SceneCapture END / Lock / Flip → READBACK    GetRenderTargetData → memcpy into VRAM
        Unlock → VRAM_DIRTY                          texels re-read from VRAM before the next use
```

- **Surfaces stay in guest VRAM.** dxg's heap allocates every surface
  below the window; `DdCreateSurfaceEx` (`GUID_Miscellaneous2Callbacks`)
  registers each by `dwSurfaceHandle` with VRAM offset, size, pitch,
  D3DFORMAT and caps (mip chains send every level's offset). The host
  creates the DXVK object lazily: a MANAGED texture filled from the VRAM
  pointer, a render target, or a depth surface (D16/D24X8/D24S8, with
  fallbacks). `DdUnlock` of a texture or target sends `VRAM_DIRTY`;
  `DdDestroySurface` (`NOTHANDLED`, dxg frees the block) sends
  `VRAM_RELEASE`. The driver logs every surface it registers
  (`d3dptdisp: surface <handle> caps … w h fmt at …`) so an unknown
  handle in the executor's log can be looked up.
- **Rendering goes to a host render target; READBACK brings it back.** A
  context (`D3dContextCreate`) is a render-target + Z pair; the d3d9
  device is created on the first. The frame is copied into the target's
  VRAM at `SceneCapture END`, at `DdLock` of a target and in `DdFlip`
  before the OFFSET write, so flips, HEL blits, GDI and screenshots see
  it; skipped when nothing was drawn since. A full `Clear` of the target
  skips the upload of stale VRAM; a partial one, or a draw onto a
  HEL-blitted background, uploads it first. Per frame that is one
  `GetRenderTargetData` + memcpy (1.2 MB at 640×480×32).
- **DrawPrimitives2** copies the runtime's command buffer and vertices
  into one `DP2` record (`D3DHALDP2_USERMEMVERTICES` is a user pointer,
  read in the caller's context) and rings the doorbell; the driver mirrors
  RENDERSTATE tokens into the runtime's `lpdwRStates`. The host interprets
  the tokens on `IDirect3DDevice9`: render and stage states (DX7→d3d9
  filter renumbering, address / filter / LOD states → sampler states),
  VIEWPORTINFO + ZRANGE (re-applied after every target change: d3d9 resets
  it), SETRENDERTARGET, CLEAR, the draw tokens → `DrawPrimitiveUP` /
  `DrawIndexedPrimitiveUP` over the touched range, SETMATERIAL / SETLIGHT
  / SETTRANSFORM (WORLD renumbered), STATESET, TEXBLT, palettes. Malformed
  streams answer `D3DERR_COMMAND_UNPARSED` with `dwErrorOffset`;
  out-of-range vertex references skip the primitive. The DX7-only states
  without a d3d9 twin (4, 10, 30, 33, 40, 47: TEXTUREPERSPECTIVE,
  LINEPATTERN, ZVISIBLE, STIPPLEDALPHA, EDGEANTIALIAS…) are logged once
  and dropped. ZBIAS (47) is mapped: each of its 0..16 steps is DEPTHBIAS
  −1/65535, the scale DXVK's own d3d8 layer uses (`d8caps::ZBIAS_SCALE`).
- **Caps:** `lpD3DGlobalDriverData` (FLOATTLVERTEX, DRAWPRIMITIVES2 +
  2EX, HWRASTERIZATION, TEXTUREVIDEOMEMORY; Z, all blends and compares,
  Gouraud + specular, fog, point/linear/mip filters, every address mode;
  the texture formats the host mirrors), `lpD3DHALCallbacks`
  (ContextCreate/Destroy/DestroyAll, SceneCapture), `GUID_D3DCallbacks3`
  (Clear2, ValidateTextureStageState, DrawPrimitives2),
  `GUID_D3DCallbacks2` (SetRenderTarget), `GUID_D3DExtendedCaps` (4096²
  textures, 8 stages, all texture ops, stencil, `dwMaxTextureAspectRatio`
  4096), `GUID_ZPixelFormats` (D16, D24X8, D24S8), `DDCAPS_3D` +
  `DDSCAPS_3DDEVICE|TEXTURE|ZBUFFER|MIPMAP`. `DdCanCreateSurface` accepts
  exactly the formats the host mirrors.
- **`GetDriverState` is mandatory.** After validating the
  `GUID_Miscellaneous2Callbacks` table, `ddraw.dll` checks
  `hwCaps.dwDevCaps & (D3DDEVCAPS_DRAWPRIMITIVES2EX |
  D3DDEVCAPS_HWTRANSFORMANDLIGHT)` and, if set, requires `GetDriverState`
  to be non-NULL, else builds the HEL-only object — the whole HAL gone.
  Neither the DDK docs nor the samples say so. `DdGetDriverState`
  answers DD_OK with the buffer untouched. The same disassembly showed
  that user-mode ddraw probes a mangled `GUID_DDStereoMode` the driver
  must *refuse*, that `dxg.sys` drops Callbacks3 when the
  ParseUnknownCommand query is refused, and that no answer may exceed
  `dwExpectedSize` (guard words after the buffer are checked).
- **Texture sizes.** `dwTextureCaps` claims `D3DPTEXTURECAPS_POW2 |
  NONPOW2CONDITIONAL` (and D3DCAPS8 with it), a GeForce's answer: Crimson
  Skies branches on `POW2` and with it absent never made its menu's
  string textures (doc 19 §34). `ddflags=0x2` (`DDF_TEX_ANYSIZE`) is the
  A/B.
- **A Z buffer written through a Lock.** A title that resets depth by
  writing the Z buffer, and the HEL doing an application's depth fill
  (also through `DdLock`, since the driver claims no blits), write VRAM
  the host's depth buffer never reads. `DdUnlock` hands a Z buffer locked
  for writing to the core (`d3d_z_written`), which samples a seventh of
  its rows and turns a buffer of one value into a host Z clear
  (`ZFILLTEST.EXE`, doc 19 §34).
- **Tests.** `tools/d3dpt-dp2-test.cpp` (the `d3dpt-dp2` host check)
  registers a target, a Z buffer and a texture in malloc'ed VRAM, sends
  the D3D7TEST scene as the DP2 tokens the runtime would emit, checks
  pixels, and feeds hostile records that must be refused without killing
  the executor; every executor fix since has a case there.
  `DRIVER\D3D7TEST.EXE` draws the same scene through `IDirect3DDevice7`
  and `tools/xp-driver-test.sh <image> d3d7` diffs its BMP against the
  host test's: 0 of 307200 pixels differ. The runtime batches a whole
  frame into one DP2 call.

### When a title falls back to its software renderer

A 1997 title asks for what a Voodoo of the day had — 8-bit palettized
textures (what fits in 4 MB) and colour keying — and with either missing
takes its software rasterizer without a word. `DdCanCreateSurface`
prints the first eight formats it refuses:

    d3dpt-vga: guest: d3dptdisp: refused pixel format, flags 0x00000020 fourcc 0x00000000 bits 0x00000008 …

(`flags` bit 5 with 8 bits is `DDPF_PALETTEINDEXED8`). Read it with the
context line: `d3dptdisp: d3d context 1 …` means the game took the HAL,
no context line means it never got that far.

### Palettized textures and colour keying

Protocol v8. What a 1997 title gets:

- **The caps.** The DX7 texture list carries `DDPF_RGB |
  DDPF_PALETTEINDEXED8` at 8 bits, the DX8 list `D3DFMT_P8`;
  `dwTextureCaps` carries `D3DPTEXTURECAPS_TRANSPARENCY` and
  `ALPHAPALETTE` (masked out of `D3DCAPS8.TextureCaps`, where bit 3 means
  nothing); DirectDraw carries `DDCAPS_COLORKEY` with `dwCKeyCaps =
  DDCKEYCAPS_SRCBLT`, a `SetColorKey` callback **and a `Blt` callback**.
  CKTEST settled the shape in four runs:
  1. caps + `SetColorKey`, no `Blt`: dxg drops the whole HAL
     (`ddflags=0x20000` is the repro);
  2. no caps: `SetColorKey(DDCKEY_SRCBLT)` succeeds but user-mode ddraw
     keeps the key to itself — `DdSetColorKey` is never called;
  3. caps + `DDCAPS_BLT` + a declining `DdBlt`: works, but claims blits;
  4. caps + the `DdBlt` callback **without** `DDCAPS_BLT`: works, and
     nothing can ever be routed to `DdBlt` — the driver's shape.

  `ddflags=0x10000` (`DDF_NO_CKEY`) withdraws the D3D caps, the
  DirectDraw caps, the P8 format and the key check together.
- **The key arrives two ways**, both kept: `DdSetColorKey` sends
  `D3DPT_OP_VRAM_COLORKEY` (handle, low, high, on/off) as the app sets
  it, and the DP2 walk, on every `TEXTURESTAGESTATE` that binds a
  texture, reads the surface's `DDRAWISURF_HASCKEYSRCBLT` / `ddckCKSrcBlt`
  off the `DD_SURFACE_LOCAL` the surface table remembers and sends the
  record ahead of the DP2 record when it differs from what the host was
  told — covering a key set before the surface was mirrored. Key values
  are the surface's own pixel values (0xf81f for magenta in R5G6B5, an
  index for P8), an inclusive range.
- **Palettes never touch the driver.** A texture's palette reaches the
  host inside the DP2 stream: `SETPALETTE` (palette handle, flags with
  `DDRAWIPAL_ALPHA` 0x2000 when `peFlags` are alpha, surface handle) and
  `UPDATEPALETTE` (handle, start, count, entries) — the same two tokens
  for DX7 and DX8.
- **The host expands both to A8R8G8B8.** DXVK has no P8 and a key needs
  alpha, so a P8 or keyed texture's host object is A8R8G8B8 and
  `upload_texture` converts texel by texel (palette colour, alpha from
  `peFlags` for an alpha palette, a grey ramp with no palette set; alpha
  0 inside the key range). A palette update dirties every texture using
  it; a key change releases the host object when its format changes.
  Since a bound texture can change under the runtime (a palette edit, a
  `Lock`), every draw first re-uploads dirty bound stages (`pre_draw`) —
  the runtime re-sends `TEXTUREMAP` only on a `SetTexture`.
- **Keying is alpha 0 plus the alpha test, with one override.** While
  `COLORKEYENABLE` (41) is on and stage 0's texture has a key, the alpha
  test is forced on (`GREATEREQUAL 1`) unless the app runs its own, and
  stage 0's alpha op becomes `SELECTARG1 TEXTURE` when the app's alpha
  pipeline does not read the texture: the DX7 runtime's
  `TEXTUREMAPBLEND` emulation sets `ALPHAOP = SELECTARG2 DIFFUSE` for any
  format without alpha — every keyed R5G6B5 / P8 texture — and an alpha
  test cannot see a key the pipeline threw away. The app's states come
  back the moment the key stops applying. The cost: a title that keys
  *and* fades by diffuse alpha in one draw loses the fade (rare under the
  DX7 SDK's semantics).
- **Tests.** `d3dpt-dp2-test` (a P8 texture through a palette, an entry
  changed under a bound texture, a keyed checker with 41 on and off, the
  app's own alpha test winning, hostile palettes);
  `DRIVER\CKTEST.EXE` through the DX7 API (`xp-driver-test.sh <image>
  cktest`).

### Execute buffers — the DirectX 3 path

Moto Racer (1997) took the HAL with v8's caps and drew nothing through it:
**0 draws** over minutes of play. It ships with DirectX 3 and draws
through **`IDirect3DDevice::Execute`**: execute buffers of `D3DOP_*`
instructions (`STATERENDER`, `PROCESSVERTICES`, `TRIANGLE`, `EXIT`),
textures bound by `D3DRENDERSTATE_TEXTUREHANDLE`, the viewport cleared
through a background material. XP's `d3dim.dll` (the DX3–6 runtime;
`d3dim700.dll` is only `IDirect3D7`) runs that on a DrawPrimitives2
driver. `DRIVER\EBTEST.EXE` does what such a title does and logs every
HRESULT; `-rgb` runs it on the runtime's RGB software device as the
control. What the path needs (found in `d3dim.dll`'s disassembly, XP SP3):

- **`dwMaxVertexCount` must be 4096.** The Execute core sizes its TL
  vertex buffer as `max(dwVertexCount clamped to 4096, dwMaxVertexCount)
  × 32` bytes + 0x400, and the vertex buffer constructor refuses more than
  0xffff vertices; every failure there is reported as `E_OUTOFMEMORY`. A
  cap of 65535 is one page over, so every `Execute` failed with
  0x8007000E before a token reached the driver. 4096 is the runtime's own
  clamp; `dwMaxBufferSize` stays 0 (unlimited). `ddflags=0x40000`
  (`DDF_EB_MAXVERT_65535`) is the repro. The DX5+ interfaces never
  consult the field.
- **The path is a pass-through with a bounce, not a translation.** In the
  UNCLIPPED mode the core hands every run of driver instructions to
  `DrawPrimitives2` **as they are**: `dwFlags` = `D3DHALDP2_EXECUTEBUFFER`
  (0x2), `lpDDCommands` = the app's execute buffer, `dwCommandOffset` the
  current instruction, `lpDDVertex` the runtime's TL buffer. The
  `D3DOP_*` opcodes share the DP2 numbering where the payloads match
  (`POINT` / `LINE` / `TRIANGLE` / `STATERENDER` = 1 / 2 / 3 / 8 =
  `POINTS` / `INDEXEDLINELIST` / the legacy 8-byte `INDEXEDTRIANGLELIST`
  / `RENDERSTATE` — which is why the DP2 enumeration skips 4–7 and 9–14);
  the driver consumes those, `SPAN` (13, skipped) and `EXIT` (11). Every
  other opcode — `PROCESSVERTICES` (9) first of all, the matrix / light
  opcodes 4–7, `TEXTURELOAD`, `BRANCHFORWARD`, `SETSTATUS` — is the
  runtime's: the driver ends the call before it with
  `D3DERR_COMMAND_UNPARSED` (0x88760BB8) and its offset in
  `dwErrorOffset`; the runtime catches up its state mirror, executes the
  instruction itself (a `PROCESSVERTICES` fills the TL buffer) and calls
  again from the next one. **Skipping** opcode 9 instead makes `Execute`
  succeed with an all-zero TL buffer. `walk` bounces (`d3dptdisp: execute
  buffer: opcode 9 x1 bounced to the runtime at 0x34`, the first eight);
  a call starting on a runtime instruction bounces with no host round
  trip. The runtime's `D3DParseUnknownCommand` answers
  `COMMAND_UNPARSED` for these too — it is not the mechanism
  (`ddflags=0x80000` never calls it). No caps steer any of this, and a
  CLIPPED `Execute` never reaches the pass-through: the runtime
  transforms and emits ordinary DP2 draws.
- **The DirectX 5 texture render states** arrive as the app wrote them,
  where the DX6+ runtimes turn them into stage states. The executor maps
  them (`legacy_render_state`): `TEXTUREHANDLE` (1, the surface handle —
  dxg's, i.e. ours) binds stage 0 and applies the blend;
  `TEXTUREMAPBLEND` (21) sets stage 0's ops as the old fixed function did
  (no texture: the diffuse; `DECAL` / `COPY`: the texels; `MODULATE`:
  texels × diffuse, alpha from the texture when its format has one — the
  colour-key expansion counts — else the diffuse; `DECALALPHA`,
  `MODULATEALPHA`, `ADD`); `TEXTUREADDRESS` (3), `TEXTUREMAG` / `MIN`
  (17 / 18) and `WRAPU` / `WRAPV` (5 / 6) go to stage 0's sampler and
  `WRAP0`. The legacy-blend flags of "A DirectX 6 title's flip chain"
  apply.

EBTEST passes its five cases (Clear, flat, textured, keyed, CLIPPED), and
**Moto Racer plays**: name screen, showroom and the race with its
colour-keyed palms and buildings at 120 frames/s under KVM (four DP2
calls and ~175 draws a frame), fast under TCG too — a DX3 title's guest
side is only execute-buffer building. Its one-triangle draws cannot be
batched: it sorts polygons back to front and switches texture per
polygon, and consecutive triangles under one texture already arrive as
one `D3DOP_TRIANGLE` entry; the 356 draws a frame cost DXVK nothing
measurable. Each `Execute` is one DP2 call per run of driver
instructions plus one bounce per runtime instruction, each a doorbell
round trip; batching is for a title that shows the cost.
`tools/xp-motoracer.sh` drives it (the game insists on a 16 bpp desktop).

### FIFA 2000 on the HAL

The first DX7-era title, formerly parked on WineD3D: with no DLL in the
folder, its own DirectX renderer (`THRASH\dx6z.dll`) runs on the HAL
unmodified — the EA intro, the title, the attract-mode match at
800×600×16. The match does not page-flip: it blits its back buffer to the
primary, so every frame is READBACK + a HEL blit. `tools/xp-fifa2000.bat`
and `tools/xp-fifa-match.sh kvm|tcg <image>` drive it.

**The match ignores the keyboard under TCG.** FIFA's keyboard is a
`DISCL_NONEXCLUSIVE | DISCL_FOREGROUND` DirectInput device polled with
`GetDeviceState`. On XP such a device is fed by a low-level hook that
runs on the thread which created it, only while that thread services its
message queue — and the match loop pumps rarely. A fast guest (KVM)
keeps up; a TCG guest stretches the gaps and the hook falls behind, so
the device reports no key while `GetAsyncKeyState` in the same process
sees every one. Ruled out: the emulator's input path (`DRIVER\DITEST.EXE`
sees every key under TCG; the embed library's `qemu-embed: input:`
statistics are clean), `LowLevelHooksTimeout`, the frame rate.

The fix is `D3DPT\DINPUT.DLL` next to the EXE (`SETUP /GAME 2`): a
forwarding shim that sets in the returned keyboard state every key
`GetAsyncKeyState` reports pressed. User-confirmed 2026-09-05 by A/B on a
TCG run: keys with the DLL, none without. On the user's everyday KVM host
nothing is needed. It is silent by default; `D3DPT_DINPUT_LOG=1` adds
`dinput_log.txt` (devices, cooperative level, poll rate, every key)
through a sampler thread that costs real time under TCG
(`tools/xp-fifa2000.bat` turns it on when `E:\DILOG` is staged).

**The shim goes next to the game, never system-wide** (user decision,
2026-09-05). Replacing `system32\dinput.dll` fights Windows File
Protection, and a forwarding shim cannot share its target's name in one
directory; `AppInit_DLLs` would load it into every GUI process. And the
merge is a lie: `GetAsyncKeyState` is system-wide, so a foreground
device that has correctly gone quiet would report keys again — one worth
telling FIFA's match loop, not every process.

FIFA's own quirks: its front-end menus need a mouse button held ≈1 s,
Esc skips the intro, the kickoff starts itself after ≈1 minute; F1–F4
cameras, Esc pause, F12 exit in the match.

### Max Payne on the HAL: XP's own d3d8.dll on a DX7 driver

XP's d3d8.dll treats a driver without a `D3DCAPS8` answer as a "DirectX
7 driver": it does the vertex processing and feeds the DX7 token set
through DrawPrimitives2. Max Payne runs that way with no wrapper DLL
(`ddflags=0x2000` gives d3d8.dll that face today; `tools/xp-maxpayne.bat`,
~290 frames/s at 800×600×16 under KVM `-cpu pentium3`). What it taught:

- **`…_IMM` tokens are DWORD-aligned, before and after.** After the
  `D3DHAL_DP2COMMAND` of `TRIANGLEFAN_IMM` / `LINELIST_IMM`, round the
  offset up to 4, then the token's header and the vertices; round up
  again for the next command (the DDK's perm3 sample does the same). The
  DX8 runtime's legacy path starts such tokens at 2 mod 4 every frame
  (after an `INDEXEDTRIANGLELIST2` with an even count). Aligning only the
  end parsed but read vertices two bytes early — one garbage fan per
  frame, clipped to the screen as **black bands across the alley**.
  `d3dpt-dp2-test` sends such a fan with the runtime's 0xcc padding. The
  executor logs the token history with every first failure of a kind
  (`ddi: dp2: tokens before it (offset:op x count): …`).
- **DXVK's exceptions abort QEMU; validate before calling.** A garbage
  `LightEnable` index made DXVK grow its light array to ~2³²:
  `std::bad_alloc`, which cannot be caught — DXVK carries its own
  statically linked unwinder, and the system `__gxx_personality_v0`
  handed its context `abort()`s. The interpreter drops light indices ≥
  1024 and transform ids outside VIEW / PROJECTION / TEXTURE0–7 /
  WORLD0–3; `d3dpt_exec_submit` still wraps each record in a `try` for
  the executor's own allocations.

## M7c — the DirectX 8 DDI

To d3d8.dll the driver is a DirectX 8 driver: `D3DCAPS8` with hardware
T&L, the DX8 token stream, render-to-texture, state sets. D3DGAME8 (doc
14's DX8 reference scene) runs through XP's own d3d8.dll with **hardware
vertex processing** and no wrapper DLL (`tools/xp-driver-test.sh <image>
d3dgame8`, diffed against the native oracle). The pieces:

- **`GetDriverInfo2`.** With `DDHALINFO_GETDRIVERINFO2` in the HAL info
  the runtime sends `GUID_DDStereoMode` queries whose data starts with a
  `DD_GETDRIVERINFO2DATA` header (`dwMagic` = `D3DGDI2_MAGIC`); the driver
  answers `DXVERSION` (0x802), `GETD3DCAPS8` (212 bytes), `GETFORMATCOUNT`
  / `GETFORMAT` (`DDPF_D3DFORMAT` entries, the D3DFORMAT in `dwFourCC`
  and the `D3DFORMAT_OP_*` in the `dwRBitMask` slot) and refuses the rest.
  A real stereo query, without the magic, is refused. Two traps from
  `d3d8.dll`'s disassembly: without the HAL-info flag the runtime never
  asks and stays on the DX7 path; and it checks `dwActualSize` against
  the size *inside* the GDI2 header while leaving the outer
  `dwExpectedSize` at the previous query's 24 bytes — an answer clamped to
  the outer size makes it drop the driver (`CreateDevice` answers
  `D3DERR_NOTAVAILABLE`). `ddflags=0x2000` (`DDF_NO_DX8`) keeps the DX7
  face.
- **The caps.** `D3DCAPS8` is the DX7 caps in DX8 form plus
  `HWTRANSFORMANDLIGHT` (also in the DX7 `D3DDEVICEDESC`, with the
  transform / lighting caps, lights, clip planes, blend matrices: the
  executor maps SETTRANSFORM / MULTIPLYTRANSFORM / SETLIGHT / SETMATERIAL
  onto DXVK's fixed function; `ddflags=0x1000` withdraws it),
  `PUREDEVICE`, 16-bit indices, 4096² textures, 8 stages, and the
  features of the sections below. **Never `D3DPMISCCAPS_CLIPTLVERTS`**:
  with it the runtime stops clipping pre-transformed vertices and hands
  the driver polygons crossing the camera plane, which the host
  rasterizes as garbage (Max Payne transforms on the CPU even on a T&L
  device; its alley walls came out as flat panels at wrong depths).
- **The tokens.** The DX8 draws name vertex and index buffers by surface
  handle, possibly in guest system memory the host cannot see. The driver
  keeps a table of every surface dxg reports (VRAM and system memory,
  each level's address and pitch, buffers with their size) and walks the
  stream twice in `D3dDrawPrimitives2` (`core/core_dp2.c`): SETVERTEXSHADER
  (an FVF, or a shader handle with bit 0 set), SETSTREAMSOURCE(UM) and
  SETINDICES become context state, each DRAWPRIMITIVE(2) /
  DRAWINDEXEDPRIMITIVE(2) / CLIPPEDTRIANGLEFAN becomes a self-contained
  `D3DPT_DP2_DRAW8` (protocol v6) carrying the primitive, the vertex
  format, the vertex range and the indices (relative to MinIndex), TEXBLT
  is done in the driver (system-memory texture → VRAM, then VRAM_DIRTY),
  patches and dirty rects are dropped by size, and the DX7 tokens pass
  through (inline-vertex ones re-padded for their new offset). Pass 1
  measures and blits, pass 2 writes. **The state persists between
  calls** — the runtime sends bindings only on change — and is kept as
  handles, resolved at every call, because a `Lock` with DISCARD gives a
  buffer new memory and dxg reports it with another `CreateSurfaceEx`. A
  user-memory stream holds `dwVertexLength` vertices of the token's
  stride, not of `dwVertexSize`. The driver's `dx8 draws skipped … why`
  line names skipped draws (bits: 1 shader, 2 no FVF, 4 no stream, 8
  stride < FVF, 16 vertex range, 32 index range, 64 primitive).
- **State sets** (`STATESET`) record into d3d9 state blocks.
  **Render-to-texture:** a texture with 3DDEVICE caps is a default-pool
  render-target texture whose level 0 is the target. The d3d8-only render
  states 153, 164, 172, 173 are dropped; the rest share d3d9's numbering.
- **Compressed textures need a `DdCreateSurface` that sizes them.** dxg
  sizes a video-memory surface from its bit count, which a FOURCC format
  lacks, so a DXT texture asked for zero bytes: `D3DPOOL_DEFAULT` failed
  with `D3DERR_OUTOFVIDEOMEMORY`, and a MANAGED one's video copy failed
  silently at the first draw, leaving the previous texture bound. For a
  `DDPF_FOURCC` DXT surface the callback sets `dwBlockSizeX` = the linear
  size, `dwBlockSizeY` = 1, `fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE` and
  `dwLinearSize` (which is why dxg's "pitch" of a DXT surface is its
  linear size) and returns `NOTHANDLED` for everything else. DirectDraw
  creates a FOURCC surface only when the code is in the driver's FOURCC
  list (`DrvGetDirectDrawInfo`'s `pdwFourCC`; the kernel reads it, not
  d3d8.dll), and a FOURCC-style entry in the GDI2 format list makes
  `CreateTexture` fail: d3d8.dll matches `dwFourCC` under
  `DDPF_D3DFORMAT` only. `DRIVER\DXTTEST.EXE` tries every format × pool.
- **The runtime's clipped fans are stream-0 draws; the DP2 vertex
  buffer is a dummy under d3d8.dll.** With `CLIPTLVERTS` withdrawn the
  runtime clips pre-transformed triangles and emits `CLIPPEDTRIANGLEFAN`
  (58: FirstVertexOffset, dwEdgeFlags, PrimitiveCount). d3d8.dll keeps
  its own clip stream (a `D3DPOOL_DEFAULT`, `D3DUSAGE_DYNAMIC` vertex
  buffer), copies the fan in at the current FVF's stride, and first emits
  `SETSTREAMSOURCE` for stream 0 with that buffer; so `FirstVertexOffset`
  is a byte offset into stream 0 as bound. The DP2 call's own vertex
  pointer is a 10 × 32-byte dummy on the DX8 path (only
  `DrawPrimitiveUP` swaps a user pointer in). Reading fans from
  `lpVertices` — the DX7 layout — drew Max Payne's nearest walls black.
- **Filter numbering.** d3d8.dll hands a DirectX 8 driver its own
  `D3DTEXF_*` values for `MAGFILTER` / `MIPFILTER` (NONE 0, POINT 1,
  LINEAR 2, ANISOTROPIC 3, the cubics 4 and 5), where the DX7 runtime
  sends `D3DTFG_*` (ANISOTROPIC 5) and `D3DTFP_*` (NONE 1, POINT 2,
  LINEAR 3); `MINFILTER` agrees. The executor reads the DX7 numbering, so
  the driver rewrites a d3d8.dll context's two states as it copies them
  (`tss_dx8_filter`). It tells runtimes apart by `ContextCreate`'s
  `dwhContext` *on input*, the interface version: **4** d3d8.dll, **3**
  DirectX 7, **0** the execute-buffer path (`iface` on the `d3d context`
  line). Before it every DX8 trilinear filter drew point-mipped
  (D3DGAME8: 11163 pixels off the oracle, 618 after).

The protocol features below each have a `ddflags` A/B bit, a
`d3dpt-dp2-test` section (including hostile records) and a guest probe.

### Vertex and pixel shaders 1.x on the DX8 DDI

Protocol v7. The caps say `D3DVS_VERSION(1,1)` / `D3DPS_VERSION(1,4)`
(96 vertex constants, `MaxPixelShaderValue` 8; `ddflags=0x4000`
withdraws both), so d3d8.dll validates each shader against the caps and
emits `CREATEVERTEXSHADER` (handle, declaration bytes, function bytes),
`SETVERTEXSHADER`, `SETVERTEXSHADERCONST`, `DELETEVERTEXSHADER` and the
pixel-shader four.

- **The driver stays out of it.** The shader tokens pass through the
  walk; a DRAW8 under a shader carries the handle in its `fvf` field with
  the stream's stride, and only the host knows what a vertex is.
- **The executor** keeps shaders per context by handle.
  `CREATEVERTEXSHADER` converts the `D3DVSD_*` declaration to
  `D3DVERTEXELEMENT9`s (`REG`'s register names the usage by the DX8
  convention — 0 position, 3 normal, 5 diffuse, 7.. texcoords — `SKIP`,
  `CONST` runs remembered, tessellator and `EXT` tokens skipped), records
  the bytes it reads of each stream, and puts one `dcl_usage vN` per input
  register in front of the function (d3d9 wants them; DX8 shaders have
  none). A declaration without a function is the fixed function on that
  layout. `SETVERTEXSHADER` applies the declaration, the function and the
  `D3DVSD_CONST` runs (DX8 loads them when the shader is set); constants
  go to `Set*ShaderConstantF` with the register range checked. A DRAW8
  under an unknown shader, or whose declaration reads a stream or bytes
  the draw did not carry, is skipped with one log line.
- **The bytecode is validated before DXVK sees it.** On an unknown
  opcode DXVK's compiler logs `No layout known for opcode` and then
  *asserts* — an abort, QEMU dies with it. `sm1_valid` walks the tokens
  against a table of the vs 1.x / ps 1.x opcodes with their operand
  counts (`DEF` carries four raw floats, `COMMENT` its length, `PHASE`
  only in ps 1.4) and checks every register against the stage's file
  sizes; anything else is refused and the handle stays unknown. The DX8
  runtime validates shaders itself, so a real guest never gets there.
- **Test:** `DRIVER\SHTEST.EXE` through XP's d3d8.dll with hand-assembled
  shaders (no D3DX in mingw), every draw read back (`xp-driver-test.sh
  <image> shtest`). A declaration-only shader must list its registers in
  FVF order: d3d8.dll refuses a colour-before-position layout itself.

### Vertex and index buffers in video memory

Protocol v9. With `D3DDEVCAPS_HWVERTEXBUFFER` / `HWINDEXBUFFER` (DDI-only
bits of `D3DCAPS8.DevCaps`) the runtime asks for its `D3DPOOL_DEFAULT`
buffers in video memory, keeps a MANAGED buffer's video copy in step by
`BUFFERBLT` tokens, and a draw can *name* the buffer instead of copying
it through the window. `ddflags=0x100000` (`DDF_NO_HWVB`) keeps every
buffer in system memory, the A/B.

- **Allocation.** The buffer callbacks see `DDSCAPS_EXECUTEBUFFER |
  DDSCAPS_VIDEOMEMORY` with `DDSCAPS2_VERTEXBUFFER` / `INDEXBUFFER`; as
  for a DXT texture the driver asks dxg for one block of the size, and
  the `CreateSurfaceEx` that follows registers a `D3DPT_VS_BUFFER`
  surface (no host object: the host reads it from VRAM). Command buffers
  stay in system memory. `DDSCAPS_EXECUTEBUFFER` is 0x00800000 (the DDKs'
  `DDSCAPS_RESERVED2`); a transcription as 0x800 once cost a case.
- **Filling.** `D3dUnlockD3DBuffer` reports the locked range with
  `VRAM_DIRTY_RANGE`; the driver does `BUFFERBLT` itself in pass 1 and
  reports the range the same way. **The `BUFFERBLT` token is 24 bytes**,
  not the 20 its field list suggests (a sixth dword follows the
  `D3DRANGE`); sized at 20 the stream desynchronised after the first
  blit. `DISCARD` / `NOOVERWRITE` need nothing: every earlier draw ran
  inside its doorbell write. The host keeps no copy, so the ranges are
  only counted (`N buffer writes of M KiB` in the 5 s line).
- **Drawing.** `walk_draw` sets `D3DPT_DRAW8_VRAM_VB` / `VRAM_IB` and
  writes a `{handle, byte offset}` pair in place of the bytes; the host
  checks the range against the buffer and draws from the VRAM pointer.
  Inline and VRAM sources mix in one draw (the clip stream is a
  default-pool buffer, so clipped fans come from VRAM too).
- **The numbers** (GTA Vice City, `tools/xp-vicecity.sh play`, ~480 draws
  a frame at 800×600×32, the game's limiter and the vertical blank off):
  **360–375 frames/s** under KVM against **265–285** with every draw's
  vertices copied, and **62–75** against **51–55** under TCG — the
  guest's memcpys are the cost removed. RenderWare rewrites 330–430 MiB of
  buffers per 5 s at that rate, which is why the host reads them at each
  draw rather than mirroring them.

### More than one vertex stream

Protocol v10. `MaxStreams` is 16 (what the era's T&L parts claim);
`ddflags=0x200000` (`DDF_ONE_STREAM`) is 1 again, the A/B.

- **Driver.** The context keeps all sixteen bindings. A draw under a
  shader handle carries, after stream 0 and the indices, **every other
  bound stream whose range is there** — the driver does not parse
  declarations; a stale binding costs an 8-byte reference when it is a
  VRAM buffer and a copy when not. One vertex number indexes every
  stream, so stream *n*'s range starts at stream 0's first vertex at
  stream *n*'s own stride. A stream running past its buffer is left out;
  an FVF draw carries stream 0 alone.
- **Record.** `D3DPT_DRAW8_STREAMS`; after the indices a `{count, 0}` pair
  and `count` `d3dpt_dp2_draw8_stream` entries (number 1..15 increasing,
  stride, its own VRAM flag), each followed by bytes or `{handle, offset}`.
- **Executor.** Every entry is parsed before anything can skip the draw.
  A declaration reading more than stream 0 is drawn **interleaved**: the
  streams it reads are copied into one vertex and a copy of the
  declaration with every element moved into stream 0 at its stream's base
  is bound (cached per shader per stride set, eight at most). So the host
  holds no vertex buffers and every DRAW8 takes the same `…UP` path.
  `apply_vs`'s early return stays: it keeps a `D3DVSD_CONST` run from
  clobbering constants set after the shader.
- **Tests:** SHTEST's two-stream cases and STRMTEST (three streams from
  a StartVertex, BaseVertexIndex + MinIndex, a system-memory stream, a
  gap, stale streams under an FVF draw), every buffer's first vertices a
  decoy.

### Cube textures

Protocol v11. `D3DPTEXTURECAPS_CUBEMAP | MIPCUBEMAP`, cube filter caps
= the 2D ones, and `D3DFORMAT_OP_CUBETEXTURE` on every RGB and DXT
format (not P8); a render-target format is a render-target cube too.
`ddflags=0x400000` (`DDF_NO_CUBE`). **The DX8 face only:** a DirectX 7
cube is created by user-mode DirectDraw against `GUID_DDMoreSurfaceCaps`,
which this driver does not answer.

- **What the runtime builds.** Six surfaces: the root is +X's level 0
  (`DDSCAPS2_CUBEMAP | CUBEMAP_POSITIVEX`), the other faces hang off its
  attach list, each with its own mip chain. The old attach walk left out
  mip-mapped faces and would have registered a one-level cube's faces as
  a flip chain.
- **Driver** (`core/core_surf.c`). `d3dpt_os_attached_all` (both layers)
  lists every attachment; `cube_faces` finds the six from the root, and
  `d3d_register_cube` puts every face in the table under its own handle
  and sends one `VRAM_SURFACE` with `D3DPT_VS_CUBE` (face 0's level 0,
  then `6 × levels − 1` `{offset, pitch}` pairs face-major), then a
  `VRAM_CUBE_FACE` per face. A TEXBLT into a cube root copies every face
  and level (`blt_levels`).
- **Executor.** A plain cube is a managed `IDirect3DCubeTexture9`; a
  render-target cube is a one-level default-pool one whose faces are
  each entry's `rt`, so targeting a face, clearing, the untracked-write
  shadow and READBACK into the face's VRAM are the 2D path unchanged.
  `VRAM_DIRTY` of a face means the cube; a render-target cube uploads only
  its dirty faces (`faces_dirty`).
- **How the runtime fills a cube:** the default-pool cube's faces are
  written through Lock / Unlock under each face's handle; no cube `TEXBLT`
  has been seen, so that path is kept but unexercised. System-memory
  copies reach `CreateSurfaceEx` with odd shapes and are logged `not
  mirrored … (system memory)`.
- **Test:** CUBETEST (managed, Lock, `UpdateTexture` into a cube the host
  already uploaded, DXT1, a render-target cube, `TCI_CAMERASPACENORMAL`,
  a vs 1.1 writing `oT0`).

### Volume textures

Protocol v12. `D3DPTEXTURECAPS_VOLUMEMAP | MIPVOLUMEMAP`, volume filter
and address caps = the 2D ones, `MaxVolumeExtent` 256, and
`D3DFORMAT_OP_VOLUMETEXTURE` on the six RGB formats and DXT1/3/5, not P8.
`ddflags=0x1000000` (`DDF_NO_VOLUME`). The DX8 face only.

- **What the runtime builds.** One DirectDraw surface per level,
  `DDSCAPS2_VOLUME` with the depth in `dwCaps4`'s low word, each through
  its own `DdCreateSurface`. It fills a video-memory volume by locking
  **the whole level** and writing slice n at n × the slice pitch (upload,
  `LockBox`, `UpdateTexture` alike); no `VOLUMEBLT` has been seen, though
  the driver does one like a TEXBLT.
- **The slice pitch trap.** `DD_SURFACE_GLOBAL.dwBlockSizeY` is a union
  with `lSlicePitch`. Asked for as one block of `size × 1` (the DXT
  recipe) the slice pitch came out 1 and the runtime wrote every slice a
  byte apart; setting it later in `CreateSurfaceEx` reaches only the
  kernel's copy. The block is asked for as **`depth` × `pitch × rows`**,
  whose height is the slice pitch the runtime then uses. `DdCreateSurface`
  sizes rows by format (`fmt_row_bytes`, `surf_rows`: block rows for
  DXT), on 9x as on NT.
- **Driver / executor.** `VRAM_SURFACE` with `D3DPT_VS_VOLUME`: level 0's
  first slice, the levels' pairs, then `{depth, slice pitch}`. The host
  makes a managed `IDirect3DVolumeTexture9` uploaded slice by slice;
  refused: depth 0 or over 256, a short slice pitch, anything past VRAM,
  a volume that is also a cube, target, primary or Z buffer.
- **Test:** VOLTEST (managed and minified, `LockBox`, `UpdateTexture`, a
  DXT1 volume). DXT3/5 share the arithmetic and are unexercised.

### Anisotropic filtering

Caps only — the executor already mapped the filters and passed
`MAXANISOTROPY`. Both faces claim `MINFANISOTROPIC | MAGFANISOTROPIC`
(cube and volume caps copy them), `D3DPRASTERCAPS_ANISOTROPY` and a
maximum of 16; `ddflags=0x2000000` (`DDF_NO_ANISO`). ANISTEST's
receding stripes: far-row contrast 0 under trilinear, 252 under ×16.

### The rest of DX8's texture formats

L8, A8L8, A4L4, A8, X4R4G4B4, R3G3B2, A8R3G3B2, DXT2 and DXT4 are 2D
textures in the DX8 list (32 slots); `ddflags=0x4000000`
(`DDF_NO_MORE_FMTS`). The DX7 list is unchanged.

- **Driver.** `pf_format` knows d3d8.dll's pixel formats for them
  (`DDPF_LUMINANCE` by mask, `DDPF_ALPHA` alone at 8 bits, the 3-3-2
  masks), read through the RGB names of the union slots because the 9x
  DDK has no luminance names. DXT2/DXT4 are in both families' FOURCC
  lists, since DirectDraw checks that list first. `fmt_row_bytes` sizes
  all of them — and V8U8, whose managed `TEXBLT` copied rows of zero
  bytes before.
- **Host.** At device creation the executor asks `CheckDeviceFormat`
  about each format once and expands any refused one to A8R8G8B8 at
  upload, as P8 is (`host_lacks`, `texel_argb`); the log says which
  (`ddi: no host texture format 52 27 29: expanded to A8R8G8B8 at upload`
  under KosmicKrisp). **X4R4G4B4 is always expanded**: DXVK creates it as
  `VK_FORMAT_A4R4G4B4_UNORM_PACK16` with no swizzle, so the X nibble
  samples as alpha and a texture written with it 0 draws transparent —
  DX7's X4R4G4B4 surfaces included.
- **Bump maps.** V8U8 is in both lists (the DX8 one as `TEXTURE |
  BUMPMAP`, the DX7 one as a `DDPF_BUMPDUDV` format for DX6/7 EMBM
  titles); L6V5U5 and X8L8V8U8 in the DX8 one, mapped from `DDPF_BUMPDUDV
  | DDPF_BUMPLUMINANCE` by mask (the luminance mask sits in the
  `dwBBitMask` slot); `ddflags=0x800000` (`DDF_NO_BUMP`) takes them all
  out. The host needs nothing but sizes: DXVK does the op, and the bump
  matrix is an ordinary stage state. DXVK's fixed function never applied
  the luminance (a shadowed variable in `sampleTexture`, and the
  luminance read from the wrong texel): `patches/dxvk/07`, with a
  `d3dpt-dp2-test` case that fails on unpatched DXVK.
- **Q8W8V8U8 is a FOURCC surface.** It has no DDPIXELFORMAT, so d3d8.dll
  creates it as a FOURCC surface whose code is the D3DFORMAT (63), and
  DirectDraw refuses a FOURCC missing from the driver's list before any
  callback — the texture silently kept only its system-memory copy. Both
  layers list 63 and size it; 3DMark2001 SE refuses Nature without it
  (doc 19 §38).
- **Point sprites** with a per-vertex size: `D3DFVFCAPS_PSIZE` is
  claimed (the driver's `fvf_stride` and the host already carried it).

### Multisampling

Protocol v13, full-screen only. The format list gives the render-target
formats (X8R8G8B8, A8R8G8B8, R5G6B5, X1R5G5B5) and D16 / D24X8 / D24S8
2 and 4 samples in `MultiSampleCaps` (the green-mask slot: `wFlipMSTypes`
low word, `wBltMSTypes` high, bit n − 1 for n samples).
`ddflags=0x8000000` (`DDF_NO_MSAA`).

- **The driver** reads the count off `ddsCapsEx.dwCaps3`
  (`DDSCAPS3_MULTISAMPLE_MASK`) and puts it in the `VRAM_SURFACE` caps
  (`D3DPT_VS_SAMPLES`, bits 8..12) of a target, depth buffer or
  flip-chain member. VRAM holds only the resolved image.
- **The host** creates the target and its depth buffer multisampled when
  DXVK has the count (`CheckDeviceMultiSampleType`, else plainly, logged
  once) and resolves (`StretchRect`, `resolved()`) before every readback.
  Nothing is uploaded into one: Direct3D 8 locks no multisampled surface.
- **Full screen only.** With blt types claimed d3d8.dll made a windowed
  multisampled device whose `Present` drew nothing: a windowed `Present`
  of a multisampled back buffer is a driver blt, and there is no blitter
  ("Blit caps and the HEL"). A flip needs nothing new.
- **Test:** MSAATEST, a full-screen 640×480×16 4-sample device, the edge
  read from the front buffer after the flip.

### The DX8 feature probes

One program per Direct3D 8 feature in `DRIVER\`, each through XP's own
d3d8.dll with every draw read back in the guest, over
`guest-tools/src/d3dptvid/d3d8probe.h`. A probe first logs the caps and
`CheckDeviceFormat` answers for its feature; **when the caps say the
driver has no such feature it ends `(not offered: <why>)` and runs
nothing**, so the same program becomes the feature's check the day the
caps claim it. `tools/xp-driver-test.sh <image> probe <NAME>` runs one,
`probes` all ten in one boot; the verdict is PASS, NOT OFFERED or FAIL.

| Probe | Feature | On XP |
|---|---|---|
| `CUBETEST` | cube textures (v11) | PASS, 9 cases |
| `STRMTEST` | several vertex streams (v10) | PASS, 6 |
| `VOLTEST` | volume textures (v12), DXT1 included | PASS, 5 |
| `FMTTEST` | the nine formats above, colour and alpha each | PASS, 9 |
| `BUMPTEST` | EMBM on each bump format, luminance variants, DOT3 | PASS, 8 (Q8W8V8U8's cases pass on Win98; XP not re-run) |
| `SPRTEST` | point sprites, per-vertex size | PASS, 4 |
| `ANISTEST` | anisotropic filtering | PASS, 1 |
| `PATCHTST` | RT- and N-patches | NOT OFFERED |
| `MSAATEST` | full-screen multisampling on and off | PASS, 2 |
| `MGDTEST` | `UpdateTexture`, a managed texture and a managed vertex buffer changed between two draws | PASS, 3 (doc 19 §32) |

## The ddflags bits

`-device d3dpt-vga,ddflags=N` (the adapter's DDFLAGS register; `DDFLAGS=`
in `tools/xp-driver-test.sh`) switches driver behaviour with no
reinstall. The bits are `DDF_*` in `core/d3dpt_core.h`; the 9x layer's
own are `D9F_*` (doc 19).

| Bit | Name | Effect |
|---|---|---|
| 0x1 | `DDF_NO_GETDRIVERINFO` | no GetDriverInfo |
| 0x2 | `DDF_TEX_ANYSIZE` | textures of any size (no `POW2`) |
| 0x4 | `DDF_NO_SURFACE_CB` | only MapMemory + CanCreateSurface |
| 0x8 | `DDF_ENGINE_BITMAP` | engine-bitmap primary |
| 0x10 | `DDF_GDI_CAP` | add `DDCAPS_GDI`: the HAL-drop repro |
| 0x20 | `DDF_NO_D3D` | DirectDraw only (the driver decides; not `no-exec`) |
| 0x40–0x800 | `DDF_NO_3D_CAP` … `DDF_NO_PARSEUNKNOWN` | one Direct3D answer each: 3D caps, D3D GetDriverInfo, buffer callbacks, Callbacks3, Misc2, ParseUnknownCommand |
| 0x1000 | `DDF_NO_TNL` | no hardware T&L |
| 0x2000 | `DDF_NO_DX8` | a DirectX 7 driver to d3d8.dll |
| 0x4000 | `DDF_NO_SHADERS` | no shader versions |
| 0x8000 | `DDF_NO_VSYNC` | flips complete instantly |
| 0x10000 | `DDF_NO_CKEY` | no colour keying, no P8 |
| 0x20000 | `DDF_CKEY_NOBLTCB` | key caps without a Blt callback: HAL-drop repro |
| 0x40000 | `DDF_EB_MAXVERT_65535` | the DX3 `E_OUTOFMEMORY` repro |
| 0x80000 | `DDF_NO_PARSEUNKNOWN_CALL` | never call the runtime's `D3DParseUnknownCommand` |
| 0x100000 | `DDF_NO_HWVB` | buffers in system memory (pre-v9) |
| 0x200000 | `DDF_ONE_STREAM` | one stream (pre-v10) |
| 0x400000 | `DDF_NO_CUBE` | no cube textures |
| 0x800000 | `DDF_NO_BUMP` | no bump formats |
| 0x1000000 | `DDF_NO_VOLUME` | no volume textures |
| 0x2000000 | `DDF_NO_ANISO` | no anisotropic filtering |
| 0x4000000 | `DDF_NO_MORE_FMTS` | none of the nine extra formats |
| 0x8000000 | `DDF_NO_MSAA` | no multisampling |
| 0x10000000 | `DDF_NO_GAMMA` | no gamma ramp |
| 0x20000000 | `DDF_TEX_256` | textures of 256² at most, as a Voodoo 2's (doc 19) |

## Debugging the driver

- **The DEBUG register** → the QEMU log (`d3dpt-vga: guest: d3dptdisp:
  …`) is the driver's only output; no kernel debugger is used.
- **`D3DPT_DP2_TRACE=<flag file>`** in QEMU's environment: `touch` the
  file when the screen shows the scene in question, and the executor logs
  one whole frame, from the next readback to the one after — a snapshot
  of every render / stage state so far, every token with its arguments
  (dropped states marked), each bound texture's format and the mean of
  its VRAM texels, every draw's first three vertices — and writes every
  bound texture's levels (`tex-<handle>-l<n>.ppm`, `-a.pgm`) and the
  target after every draw (`draw-<n>.ppm`) next to the flag file, then
  removes it. Counting pixels per `draw-<n>.ppm` names the draw that
  paints an artefact. A readback with no draw before it does not end the
  frame (`readback of N with no draw, the frame goes on`): Crimson Skies
  reads its target back after every target switch.
- **`D3DPT_DDI_REREAD=1`** re-reads every texture from VRAM at every bind
  (a stale host copy vs VRAM the guest never wrote);
  **`D3DPT_DDI_NOFOG=1`** forces fog off.
- The image's `ddraw.dll`, `d3dim.dll`, `d3d8.dll` and `dxg.sys` can be
  pulled out (`qemu-img convert` + `7z x`) and disassembled with
  `i686-w64-mingw32-objdump` — where most rules above came from, since
  the DDK documentation stops short of them.

## Open items

- **Present the host frame through the player's 3D path** instead of the
  per-frame readback into VRAM; and a vertical blank in phase with the
  player's swapchain rather than the `FRAMES` clock.
- **A blitter** behind `DDCAPS_BLT` (needed for windowed multisampling),
  and `DrvDeriveSurface` (an optimisation now that the target shadow
  exists).
- **The mode table from the player** (M2): a `modes=` property or an embed
  API call replacing the static table, pixel-aspect flags per entry.
- **RT- and N-patches** (PATCHTST is the one probe not offered); cube,
  volume and DX7-list entries for the extra formats when a title asks.
- **BUMPTEST's Q8W8V8U8 cases on XP**: unmeasured, because the install on
  a `winxp-m7` overlay failed when they were new (`docs/00-status.md`).
- FIFA 2000's videos at 320×240 ("Blit caps and the HEL"); a RAM-backed
  palette page if a title animates faster than MMIO allows; more 8 bpp
  titles (StarCraft, Age of Empires, Caesar 3).
- The two `640×480×4 / 800×600×4 @ 1 Hz` entries in the mode list are
  vga.sys' (VgaSave stays registered as the VGA-compatible device); a
  `VgaCompatible = 1` INF variant would hide them but makes our driver the
  one bootvid uses. Left alone.
- Not planned: multi-monitor, DPMS, hot-unplug. Windows 2000 is untested
  (same DDI version).
