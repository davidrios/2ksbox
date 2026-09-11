# 15. A real XP display driver (ADR-008, M7)

The long-term guest side of doc 14: instead of DLL copies per game
folder, a Windows display driver pair that owns the adapter, and later
speaks the DirectDraw and Direct3D DDIs into the same transport and
executor. Staged: **M7a framebuffer (landed 2026-09-04) → M7b DirectDraw
DDI (first cut landed the same day: VRAM surfaces, page flips, cached
mappings) → M7c Direct3D DDI (first cut 2026-09-04: the DX7 non-T&L HAL
on the doc 14 executor, D3D7TEST's frame identical to the host-side
test's)**. ADR-008 (doc 10) has the why; this doc is the how and the state.

## Shape (M7a)

```
guest (XP)                                          host (QEMU process)
 win32k.sys (GDI)                                    hw/d3dpt/d3dpt_vga.c  "d3dpt-vga"
   └ d3dptdisp.dll  display driver (winddi.h)          ├ PCI 1234:3d00, class VGA, stdvga ROM
       EngCreateBitmap over the mapped VRAM            ├ BAR 0 VRAM 128 MiB ─── DisplaySurface points here
                                                       │   (top 64 MiB: the M7c command window)
       GDI draws every pixel itself                    ├ BAR 1 registers (d3dpt/d3dpt_fb.h)
 videoprt.sys                                          │   mode table, WIDTH/HEIGHT/BPP/PITCH/OFFSET,
   └ d3dptvid.sys   video miniport (video.h)           │   ENABLE, FRAMES, DEBUG (char → QEMU log)
       finds the PCI device, maps BAR 1,               └ VGA core (hw/display/vga.c) while ENABLE = 0:
       reads the host's mode table, sets modes            BIOS text, vga.sys before install, BSODs
 vga.sys / VGA BIOS until the driver is installed    embed listener: same surface pointer → player
```

- **The adapter is a real VGA.** QEMU's standard VGA core with the Bochs
  VBE ports, so SeaBIOS' `vgabios-stdvga.bin` boots it (the ROM's default
  path takes BAR 0 as the LFB for any vendor id), XP's inbox `vga.sys`
  runs the desktop at 640×480 or 800×600 4 bpp before our driver exists,
  and the blue screen / shutdown text still work because `HwResetHw`
  returns FALSE and videoprt's int10 puts the core back into mode 3.
  Class code 03.00, vendor/device 1234:3d00 (the QEMU/Bochs pseudo vendor,
  our id), matched by the INF as `PCI\VEN_1234&DEV_3D00`.
- **The register BAR is the paravirtual part** (`d3dpt/d3dpt_fb.h`, one
  header for the QEMU device and the miniport). The host owns the **mode
  table**: the miniport reads MODE_COUNT and walks MODE_SEL → MODE_W/H/
  BPP/HZ, so what XP's Display Properties offers is decided in the player
  (M2's mode table and pixel aspect plug in here; today a static list of
  7 sizes × {60, 75, 85} Hz × {16, 32} bpp = 42 modes). A mode switch
  writes WIDTH/HEIGHT/BPP/PITCH/OFFSET and ENABLE = 1; ENABLE = 0 hands
  the console back to the VGA core. DEBUG takes one character per write
  and the device prints lines to the QEMU log (`d3dpt-vga: guest: …`):
  the only "debugger" we have for kernel code, and enough so far.
- **No copy inside QEMU.** While ENABLE is set the device's
  `DisplaySurface` is created *over the VRAM bytes* at the guest's
  offset/pitch (`qemu_create_displaysurface_from`), dirty lines come from
  the memory dirty log (`DIRTY_MEMORY_VGA`, the same mechanism
  `bochs-display` uses), and the embed listener hands that pointer plus
  the dirty rectangles to the player. 16 bpp modes are converted per dirty
  span into an x8r8g8b8 shadow (the embed listener takes one format);
  32 bpp is the untouched framebuffer. The player's upload of the dirty
  rectangle into its texture is the one copy left (zero-copy to the GPU
  from guest RAM is not possible with a host-visible-only import; M3's
  dma-buf ring is for 3D frames the host renders).
- **The display driver is the DDK "framebuf" shape.** `DrvEnablePDEV`
  picks the miniport mode matching the DEVMODE, fills GDIINFO/DEVINFO for
  16/32 bpp bitfield surfaces, `DrvEnableSurface` sets the mode, maps
  VRAM (`IOCTL_VIDEO_MAP_VIDEO_MEMORY`) and hands GDI an engine bitmap over
  it with no hooks: GDI draws every pixel in software, straight into
  guest VRAM, and simulates the pointer. `DrvAssertMode(FALSE)` resets to
  VGA for full-screen consoles and the logon desktop switch. The register
  page reaches the display driver through
  `IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES` (debug now; M7b/c records later).
- **Install** is `DRIVER\DRVINST.EXE [-reboot]` on the guest-tools ISO:
  newdev's `UpdateDriverForPlugAndPlayDevices` with `INSTALLFLAG_FORCE` for
  the hardware id (what `devcon update` does), after setting the
  driver-signing policy to ignore. No DDK, no devcon. Or Device Manager →
  Video Controller (VGA Compatible) → Update Driver → the folder.
  `SETMODE.EXE [w h bpp [hz]]` lists / switches modes for scripts.

## Building kernel-mode PE files with GCC

`guest-tools/build-driver.sh` (also run by `build-wrappers.sh`, which
stages the result as `DRIVER\` on the ISO). mingw-w64 ships the DDK headers
and import libraries under its permissive licence; nothing from Microsoft's
DDK is used. What it took:

- `-nostdlib -shared -ffreestanding -fno-stack-protector
  -mno-stack-arg-probe -fno-asynchronous-unwind-tables`, subsystem native,
  image base 0x10000, OS/subsystem version 5.1, entry `_DriverEntry@8`
  (miniport) / `_DrvEnableDriver@12` (display DLL; exported undecorated
  through a .def with `--kill-at`). `-lgcc` for helpers.
- The miniport links **only** `libvideoprt.a`, the display DLL **only**
  `libwin32k.a`; the build script fails if any other import appears.
- GCC emits `memcpy`/`memset` calls for struct copies even freestanding
  and neither port driver exports them: `kcrt.c` carries byte loops,
  compiled with `-fno-tree-loop-distribute-patterns` so they are not
  turned back into calls to themselves.
- Header sets are not mixable: the miniport takes `ntdef.h`, `dderror.h`,
  `devioctl.h`, `ddk/miniport.h`, `ntddvdeo.h`, `ddk/video.h` (**not**
  `ntddk.h`, which conflicts with `miniport.h`; with `_WIN32_WINNT=0x0501`
  mingw's `wdm.h` does not even compile, so leave the default). The display
  DLL takes `windef.h`, `wingdi.h`, `winddi.h`, `devioctl.h`, `ntddvdeo.h`;
  mingw 14's `winddi.h` includes `ddrawint.h`/`d3dnthal.h` which mingw does
  not ship, so `guest-tools/src/d3dptvid/ddk/` vendors ReactOS' public-domain
  `ddrawint.h` (+ `dvp.h`) and a **self-contained `d3dnthal.h`**: the DDK's
  version includes `d3dtypes.h`/`d3dcaps.h` and through them the user-mode
  `windows.h`, so the few Direct3D types the DDI structures use (caps
  structs, D3DRECT, the DP2 command header) are spelled out in it with the
  DDK's layouts. The DP2 token layouts themselves only the host interprets
  (`d3dpt/exec/d3dpt_exec_ddi.cpp`).
- The same `-march=pentium3` floor and ISA/UCRT checks as the wrappers.

## State (2026-09-04, Linux host, XP SP3 guest)

- `-vga none -device d3dpt-vga`: SeaBIOS text, XP boots to the desktop on
  `vga.sys` (800×600×4, Found New Hardware wizard for the unknown VGA).
- `DRVINST.EXE`: the INF matches, XP's Logo-test dialog appears once
  (unsigned driver; "Continue anyway"; the installer sets the policy
  values but SP3 still asked — see below), "installed", the miniport is
  loaded immediately (`adapter found`, 42 modes in the QEMU log). After a
  reboot the desktop is ours: XP picks 640×480×32@60 first (no saved
  mode), `SETMODE 1024 768 32 85` switches (`ChangeDisplaySettings = 0`),
  800×600×16@75 too (the 16 bpp shadow path), `SETMODE` lists 42 modes
  plus XP's two 4 bpp VgaSave entries. Every switch is `reset device` →
  `set mode N` → `linear mode on (WxHxBPP pitch P …)` in the log.
  Clean power-down (QMP `system_powerdown`) and the desktop come back at
  the saved mode.
- Player (Linux, RADV): the same image boots in `target/release/player`
  with `-vga none -device d3dpt-vga`; the log shows the listener following
  the driver (`[display] switch 1024x768 stride 4096` right after
  `linear mode on`), `shutdown -r` writes ENABLE = 0 (`linear mode off`)
  before the guest reset so the BIOS text and the XP boot logo show on the
  VGA core, the second boot comes back at the saved 1024×768×32@85, and
  QMP `system_powerdown` ends the run with the player exiting cleanly.

- **KVM** (2026-09-04, the user's run and mine): `-accel kvm -cpu host`
  with the same device and driver works and is far faster than TCG; the
  x87 patches are TCG-only and irrelevant there. Linux hosts should run XP
  under KVM; TCG stays the Apple Silicon path.
- **Mode-switch flash fixed** (2026-09-04): a switch is RESET → SET_MODE a
  few ms apart, and the VGA core was shown in between (text mode / stale
  VGA memory in the player), then the new mode came up with the old
  desktop bytes at the new pitch. Now the device holds the last linear
  frame for 15 refreshes after ENABLE goes 0 before handing the console to
  the VGA core (a real return to VGA is delayed by ~250 ms, nothing else),
  and the miniport zeroes the new mode's frame buffer unless
  `VIDEO_MODE_NO_ZERO_MEMORY` (it maps all of VRAM in kernel space for
  that). Player log of a switch is now one `[display] switch` line.

## M7b — the DirectDraw DDI (2026-09-04)

What Microsoft's `ddraw.dll` → `dxg.sys` sees behind the display driver
(`nt/d3dptdisp.c` over `core/`, doc 19 §19; the DDI header is ReactOS' public-domain
`ddrawint.h`, vendored in `guest-tools/src/d3dptvid/ddk/`):

- **The primary is a device surface GDI still draws on.** `EngCreateDeviceSurface`
  + `EngModifySurface(hsurf, hdev, HOOK_SYNCHRONIZE, MS_NOTSYSTEMMEMORY,
  dhsurf, pvScan0 = VRAM, pitch)` with a no-op `DrvSynchronizeSurface`.
  `HOOK_SYNCHRONIZE` is not optional: without it win32k refuses the
  `EngModifySurface` (the driver tries the variants in order and logs the
  one that took; the M7a engine bitmap is the fallback, desktop only).
- **VRAM behind the primary is one linear heap** (`DrvGetDirectDrawInfo`:
  `VIDMEM_ISLINEAR`, `fpStart` = primary size rounded to 4 KiB, `fpEnd` =
  end of BAR 0, offsets relative to the frame buffer). dxg's heap manager
  places every DirectDraw surface in it; the driver never allocates.
- **The caps dxg accepts:** `dwCaps = 0` (no blit caps: DirectDraw's HEL
  blits on the mapped VRAM, which is RAM here), `dwCaps2 = WIDESURFACES`,
  `ddsCaps = PRIMARYSURFACE | OFFSCREENPLAIN | FLIP | FRONTBUFFER | BACKBUFFER`,
  `DDHALINFO_GETDRIVERINFOSET` + a `GetDriverInfo` that answers
  `GUID_NTCallbacks` (SetExclusiveMode, FlipToGDISurface) and refuses the
  rest. **`DDCAPS_GDI` in `dwCaps` makes dxg drop the HAL** (enable →
  immediate disable, `DDCAPS_NOHARDWARE`, system-memory surfaces); found
  by bisection with the `ddflags` knob below, kept as `DDF_GDI_CAP` for
  the record.
- **Callbacks:** `DdMapMemory` (VRAM into the game's process through the
  miniport's `IOCTL_VIDEO_SHARE_VIDEO_MEMORY`), `DdCanCreateSurface`
  (display format only), `DdFlip` = one write of the target surface's
  VRAM offset into the device's OFFSET register (a real page flip: the
  console surface moves to the other buffer, nothing is copied),
  `DdWaitForVerticalBlank` = wait for the device's FRAMES counter to
  move (bounded at 50 ms), `DdGetBltStatus` = always done,
  `DdGetFlipStatus` = the vertical blank below. Not hooked: Lock, Unlock,
  Blt, CreateSurface, DestroySurface.
- **Mappings are cached, not write-combined.** videoprt's
  `VideoPortMapMemory` maps frame buffers uncached or write-combined
  (right for a PCI aperture, wrong for RAM): reads from such a mapping run
  at tens of MB/s, and GDI scrolls, DirectDraw's HEL copies and every
  `Lock` read do exactly that. The miniport therefore maps VRAM itself:
  `MmMapIoSpace(MmCached)` for the kernel view GDI draws through, and a
  cached view of `\Device\PhysicalMemory` (`ZwMapViewOfSection`, what
  videoprt does inside minus `PAGE_NOCACHE`) for DirectDraw's user-mode
  `Lock`. Those are the miniport's only ntoskrnl imports; the register BAR
  stays a videoprt (uncached) mapping. QEMU reads guest RAM coherently, so
  a cached guest mapping is correct under KVM and TCG alike.
- **`-device d3dpt-vga,ddflags=N`** (register `DDFLAGS`, read by the
  display driver) switches behaviours without a driver reinstall:
  1 = no GetDriverInfo, 4 = no surface callbacks, 8 = engine-bitmap
  primary, 0x10 = add `DDCAPS_GDI`, 0x8000 = no vertical blank (flips
  complete instantly, the M7b behaviour: throughput runs). That is how the caps were bisected
  in one afternoon: one boot per variant, `DDTEST` and the QEMU log tell.
- **Test:** `DRIVER\DDTEST.EXE [w h bpp] [frames] [-windowed]` (guest-tools
  ISO): caps, exclusive flip chain (Lock/Unlock pattern + `Blt` colour fill
  + `Flip`) or a windowed offscreen surface blitted to the primary, fps,
  `ddtest.log` + `ddtest.bmp`. `tools/xp-driver-test.sh <image> ddtest`
  runs the set headless and pulls the logs out. Results 2026-09-04 (KVM,
  RADV host):

  | case | before (write-combined) | cached mappings |
  |---|---|---|
  | 640×480×16 exclusive, 300 flips | 3846 fps | 4762 fps |
  | 640×480×32 exclusive, 300 flips | 6383 fps | 6383 fps |
  | 640×480×32 windowed, offscreen VRAM → primary Blt (HEL) | 29.9 fps | 305 fps |

  Those are throughput numbers, from before the flip chain had a vertical
  blank: since 2026-09-05 a flip chain runs at the mode's refresh rate and
  `DDTEST` reports 60 fps for all three exclusive cases. Add
  `DDFLAGS=32768` to measure throughput again (the numbers above come back
  to the frame).

  Every surface reports `VIDEOMEMORY | LOCALVIDMEM`, the QEMU log shows
  `scanout offset 0 -> 614400 -> 0 …` per flip, the picture (checkerboard
  + moving bar) is right in the screendump. The uncached numbers before
  write-combining were 31 fps for the 16 bpp chain.

## Open items

- The Logo dialog: none of the registry policy values silence it on XP
  SP3 (the stored policy is hash-protected; only the Control Panel can
  change it). `DRVINST.EXE` therefore runs a watcher thread that finds
  the dialog (created by setupapi inside DRVINST's own process) and presses
  its first push button, "Continue Anyway". `alt+c` is the manual fallback.
- The two `640×480×4 / 800×600×4 @ 1 Hz` entries in the mode list are
  vga.sys' (VgaSave stays registered as the VGA-compatible device); harmless,
  a `VgaCompatible = 1` INF variant would hide them but also makes our
  driver the one bootvid uses. Leave.
- A hardware cursor since register set v4 (2026-09-05 evening, "The
  hardware cursor" below): `DrvSetPointerShape` / `DrvMovePointer` feed
  the CURSOR registers, the player shows the guest's shape as the host
  window's cursor. Pointers over 64×64 fall back to GDI's software
  pointer in the framebuffer.
- `FRAMES` is a clock, not the player's present: it counts periods of the
  mode's `HZ` since `ENABLE`. A game therefore gets the refresh rate it
  asked for whatever the player is doing, but the two are not in phase —
  a present signal from the player's own swapchain is the follow-up, and
  the only way to make the guest's frames and the host's line up exactly.
- DirectDraw: `DdBlt` is not hooked, so blits are the HEL's user-mode
  copies on cached VRAM (fast enough for 2D); a host-side blit through
  the executor only makes sense once surfaces can live on the host GPU
  (M7c). Overlays, FourCC and different-format surfaces are refused
  (8 bpp palettized modes: see the section below). `GetDC` on a
  DirectDraw surface (`DrvDeriveSurface` absent): GDI writes the VRAM
  behind the driver's back; the executor's shadow of each render target
  catches those writes ("Untracked writes" below).
- Mode table from the player (M2): a `modes=` device property or an embed
  API call that replaces the static table, pixel-aspect flags per entry.
- Multi-monitor, DPMS/power states, hot-unplug: not planned.
- Win2000 untested (same DDI version; should work).

## The flip chain's vertical blank (2026-09-05)

Moto Racer 1997 on the driver played at several times its intended speed.
It is not a timing bug in the guest: the game paces itself by its flip
chain, as nearly every title of the era does, and our chain had no pace.
`DdFlip` wrote the OFFSET register and returned, `DdGetFlipStatus` said
"done" without looking, so `Flip` never blocked and `DDTEST`'s 640×480×16
chain ran at 4762 fps. On a real card the second `Flip` of a
double-buffered chain cannot be set up until the first has been scanned
out, and that wait — not a timer in the game — is what holds a 1997 racer
at 60 frames a second.

Two halves:

- **The device (`d3dpt_vga.c`, `FRAMES`).** The counter used to be
  incremented by `gfx_update`, i.e. by whoever was pulling frames: the
  player at `PLAYER_REFRESH_MS` (16 ms), a headless run never (there is no
  display client, so `graphic_hw_update` is never called — a game under
  `xp-game-test.sh` would have been capped at the 50 ms bail-out, 20 fps).
  It is now derived from the host clock: periods of the mode's `HZ` since
  `ENABLE`, 60 Hz if the guest left `HZ` unset. Same register, same
  contract (a monotonic counter ticking at the display's rate), so no
  `D3DPT_FB_VERSION` bump and an older driver still works against it.
- **The driver (`nt/d3dptdisp.c`).** `DdFlip` remembers `FRAMES` at the flip;
  until it moves the flip is in the air. A second `DdFlip` in that window
  waits (`DDFLIP_WAIT`) or returns `DDERR_WASSTILLDRAWING`, and
  `DdGetFlipStatus` answers `DDERR_WASSTILLDRAWING` for both `DDGFS_CANFLIP`
  and `DDGFS_ISFLIPDONE` — the runtime spins there for `DDFLIP_WAIT`.
  Bounded at 50 ms like `wait_frame`, so a device that stops counting
  cannot stop the guest with it. `wait_frame` now pauses between two polls:
  the register read and XP's performance counter are both exits, and a
  16 ms wait used to cost tens of thousands of them.

Measured (KVM, RADV host, `tools/xp-driver-test.sh <image> ddtest`):

| case | before | with the vertical blank |
|---|---|---|
| 640×480×8 exclusive flip chain (palette every frame) | 1132 fps | 60.0 fps |
| 640×480×16 exclusive flip chain | 3846 fps | 60.0 fps |
| 640×480×32 exclusive flip chain | 4762 fps | 60.4 fps |
| 640×480×32 windowed, offscreen → primary `Blt` (HEL) | 346 fps | 346 fps |
| `D3D7TEST` 640×480×32, 300 frames | 2400–2700 fps | 60.2 fps, frame still byte-identical to `d3dpt-dp2-test` |

The windowed case is unchanged on purpose: a blit to the primary is not a
flip and a real card does not throttle it either — a game that presents
that way and never calls `WaitForVerticalBlank` runs as fast as the CPU
allows on real hardware too. `DDFLAGS=32768` (`DDF_NO_VSYNC`) restores the
old behaviour to the frame, which is both the A/B for a suspect title and
how the throughput numbers above are still measured.

The device prints the guest's real frame rate every 5 s while flips are
happening (`d3dpt-vga: 299 page flips in 5.0 s (59.6/s)`), driven by the
flips themselves so a headless run reports the same as the player. **No
line at all while a game runs means it blits to the primary instead of
flipping** — the vertical blank cannot pace that game, and if it is too
fast the cause is the guest CPU, not the display path.

## A DirectX 6 title's flip chain (2026-09-05, GTA 2)

GTA 2 (1999, `IDirectDraw4` / `IDirect3D3`, 640×480×16) glitched on its
first run after a boot and crashed on the second, both on the driver
(`/tmp/player3.log`, TCG, `winxp-m7g`). Three faults, two in the driver
and one in the executor, all found from that log and one traced frame:

- **The glitch: the back buffer was never registered.** dxg's
  `CreateSurfaceEx` came for the primary (handle 1) only — a DirectX 7
  interface's chain gets one call per member, D3D7TEST and CKTEST never
  missed one, but this DX6 chain arrived as its root alone. The runtime's
  `SETRENDERTARGET 2` was `render target handle 2 unknown` to the host,
  every frame was rendered and read back into handle 1's VRAM (offset 0)
  while the flips alternated the scanout between 0 and 614400, so every
  other frame showed the buffer nobody drew. `DdCreateSurfaceEx` now
  walks the surface's attach list (`d3d_register_chain`: the ring of a
  flip chain, an attached Z buffer; mip levels stay with their texture)
  and registers each member the host does not know, or knows at another
  offset; `D3dContextCreate` and `D3dSetRenderTarget` do the same for
  the target they are handed. The DDK's sample drivers walk the list in
  their `CreateSurfaceEx` for the same reason.
- **The crash: the context table lived in the PDEV.** A game's exclusive
  mode switch gives GDI a new PDEV (the Direct3D context is created on
  that one) and its switch back at exit another, *before* dxg's
  `ContextDestroyAll` for the process arrives. The table in the new PDEV
  was empty, no `CTX_DESTROY` ever reached the host (the log has no
  `d3d context gone` line), and the next run's `CTX_CREATE` of handle 1
  came back `batch error 3` (`BAD_HANDLE`: the handle was still open) —
  `E_FAIL` from `CreateDevice`, the game dereferenced nothing. The table
  and its live count are globals now, like the surface table; and the
  host replaces a context re-created under an open handle instead of
  refusing it (`ddi: context N still open, replaced`), so a driver that
  lost one cannot wedge the next process. `tools/d3dpt-dp2-test.cpp`
  checks the re-creation.
- **The white menu text: the legacy blend was decided before the
  texture.** With the chain right, the menu drew — the photo, the logos,
  and solid white boxes where its text belongs. The traced frame
  (`D3DPT_DP2_TRACE`) showed 16×16 and 32×32 A4R4G4B4 glyph textures
  bound per draw as a stage state (`tss 0.0`), `ALPHABLENDENABLE` on with
  `SRCALPHA` / `INVSRCALPHA`, and stage 0's colour *and* alpha op at
  `SELECTARG2`: the diffuse, `ffffffff`. GTA 2 picks its blend with the
  DirectX 5 `TEXTUREMAPBLEND` render state (4, `MODULATEALPHA`) once,
  before any texture is bound, and the DX6 runtime passes that state to
  a DX7 driver as it is (it does *not* translate it into stage states,
  contrary to what "Execute buffers" assumed) — the executor's
  `apply_mapblend` mapped it at that moment ("no texture: the diffuse
  alone") and never again. The executor now keeps a `legacy_blend` flag
  (set by `TEXTUREMAPBLEND`, cleared by the app's own explicit stage-0
  op) and re-evaluates the blend whenever stage 0's texture changes
  while it is set. `d3dpt-dp2-test` covers the order (blend first, the
  texture as a stage state) and fails without the fix.
  **And an argument is not an op (Crimson Skies, 2026-09-09, doc 19
  §28).** That flag was cleared by *any* stage-0 state 1–6, the ARGs
  included. Crimson Skies' menu sets `TEXTUREMAPBLEND` `MODULATE` with no
  texture bound (the executor's answer: `SELECTARG2`, the diffuse), then
  its own `COLORARG2` / `ALPHARG2` = `DIFFUSE` — which ended the legacy
  blend — and binds eight A4R4G4B4 textures per draw as `tss 0.0`; the
  colour op stayed at the diffuse, `ffffffff`, and the logo, emblem and
  buttons drew as white silhouettes with the texture's alpha. The flag is
  two now, one per op (`legacy_cop`, `legacy_aop`): `TEXTUREMAPBLEND` /
  `TEXTUREHANDLE` set both, the app's own `COLOROP` ends the first and its
  own `ALPHAOP` the second, its ARGs end neither, and a bind re-evaluates
  the halves still in effect (ops only: the ARGs the app set stay). The
  `D3DPT_DP2_TRACE` snapshot is what gave it away — `tss 0: 1=0x3` at the
  frame start while every draw expected `MODULATE` — after two sessions
  had it as texture staleness, the upload, and the bind. `d3dpt-dp2-test`
  has the sequence, and the app's own op over a later bind.
- **The flip model, measured.** `DdFlip` logs its first eight calls
  (`d3dptdisp: flip curr H at OFF targ H at OFF`). For this DX6 chain
  they read `curr 1 at 0 targ 2 at 0x96000`, then `curr 2 at 0x96000
  targ 1 at 0`, and so on: the handles keep their VRAM and the roles
  alternate, exactly dxg's DX7 model above — only the `CreateSurfaceEx`
  notifications are missing. Whatever the model, `DdFlip` and a
  `DdLock` of a target now re-register a surface the host knows at
  another offset (`d3d_register_moved`, silent) before the readback: a
  no-op here, the fix should a runtime ever swap memory; the host drops
  a moved surface's shadow (the untracked-writes compare, "Untracked
  writes" below) so the first readback into new memory is whole
  (`d3dpt-dp2-test` checks that too).
- **Not ours: Enter during the intro movie.** Headless, a press of
  Enter while the Bink intro movie plays kills the game with `c0000095`
  in `binkw32!BinkGetSummary` (`div ecx`: a 33-bit dividend over a
  playback time of 1 ms; Dr. Watson's stack, pulled out of the overlay
  with `7z` after `qemu-img convert`). The same press at the logos a few
  seconds earlier reaches the menu. It is not the display path: the
  control run on XP's inbox cirrus driver (`VGA=cirrus`, no Direct3D at
  all) crashes the same way, with or without an AC97 card. A GTA 2 /
  11.44-build matter; let the intro play, or skip it at the logos.
- **Verified** with `tools/xp-driver-test.sh <overlay> bat` (three
  launches in one boot, `tskill` between them, the driver reinstalled
  from the ISO first): every launch creates its context, the flip lines
  above, `ddi: 91 frames/s`, the menu with its text in the screendumps.
  The driver ISO must be reinstalled in a guest for any of this: the old
  DLL keeps the old behaviour.

## When a title falls back to its software renderer

Moto Racer draws through its software rasterizer on the driver while FIFA
2000 gets the HAL. The two differ by two years: FIFA's 1999 DX6 renderer
asks for RGB textures and alpha blending, which the HAL offers; a 1997
title asks for what a Voodoo of the day had. Two gaps stand out in
`d3d_caps_init`:

- **No palettized textures.** `d3d_texformats` offers 32/16-bit RGB and
  DXT1/3/5 — no `DDPF_PALETTEINDEXED8`. 8-bit palettized textures with a
  palette per texture are how nearly every 1997 3D title stores its art
  (it is what fits in 4 MB of texture memory), and `DdCanCreateSurface`
  refuses the format outright, so the game cannot make its textures and
  drops to software.
- **No colour keying.** `dpcTriCaps.dwTextureCaps` has no
  `D3DPTEXTURECAPS_TRANSPARENCY`, and no surface-level colour key reaches
  the executor. 1997 titles cut out sprites and foliage with a colour key
  rather than an alpha channel; a game that requires it will not take a
  HAL that does not claim it.

Both landed the same night (the next section, protocol v8): the HAL offers
palettized textures and colour keying now, and Moto Racer takes it with
them (2026-09-05, headless) — and then drew nothing through it, because it
is a DirectX 3 title: "Execute buffers — the DirectX 3 path" below. The
diagnostic stays: `DdCanCreateSurface` prints the first eight formats it refuses —

    d3dpt-vga: guest: d3dptdisp: refused pixel format, flags 0x00000020 fourcc 0x00000000 bits 0x00000008 …

`flags` bit 5 (`DDPF_PALETTEINDEXED8`) with 8 bits is the palettized-texture
case. Read it together with the context line: `d3dptdisp: d3d context 1 …`
means the game did take the HAL, and no context line at all means it never
got that far.

## Palettized textures and colour keying (2026-09-05, protocol v8)

Both gaps above are closed. What a 1997 title now gets from the HAL:

- **The caps.** The DX7 texture format list has a tenth entry,
  `DDPF_RGB | DDPF_PALETTEINDEXED8` at 8 bits, and the DX8 list
  `D3DFMT_P8`; `dpcTriCaps.dwTextureCaps` carries
  `D3DPTEXTURECAPS_TRANSPARENCY` and `ALPHAPALETTE` (masked out of
  `D3DCAPS8.TextureCaps`, where bit 3 means nothing); the DirectDraw caps
  carry `DDCAPS_COLORKEY` with `dwCKeyCaps = DDCKEYCAPS_SRCBLT`, and the
  surface callbacks a `SetColorKey` **and a `Blt`** — the four
  CKTEST runs that settled the shape (2026-09-05):
  1. caps + `SetColorKey` callback, no `Blt`: dxg drops the whole HAL
     (`GetCaps` answers `DDCAPS_NOHARDWARE`, no Direct3D device — the same
     post-enable validation as `DDCAPS_GDI` and palette caps;
     `ddflags=0x20000` is the repro);
  2. no caps: the HAL stays and `SetColorKey(DDCKEY_SRCBLT)` succeeds,
     but user-mode ddraw keeps the key to itself — dxg's `DD_SURFACE_LOCAL`
     shows no `DDRAWISURF_HASCKEYSRCBLT`, `DdSetColorKey` is never called;
  3. caps + `DDCAPS_BLT` + a `DdBlt` that declines: the HAL stays, dxg
     calls `DdSetColorKey` *and* records the key in the surface, and
     DDTEST's chains and windowed blits are unchanged with `DdBlt` never
     called;
  4. caps + the `DdBlt` callback **without** `DDCAPS_BLT`: the same, and
     nothing can ever be routed to `DdBlt` — this is the driver's shape.
  So dxg's rule is "colour-key caps need a Blt callback"; the HEL keeps
  doing every blit, keyed ones included. `ddflags=0x10000`
  (`DDF_NO_CKEY`) withdraws the D3D caps, the DirectDraw caps, the P8
  format and the key check together.
- **Two ways the key arrives**, both kept: `DdSetColorKey` sends
  `D3DPT_OP_VRAM_COLORKEY` (handle, low, high, on/off) as the app sets
  it, and the DP2 walk, on every `TEXTURESTAGESTATE` that binds a
  texture (state 0, pass 1), reads the surface's `DDRAWISURF_HASCKEYSRCBLT`
  / `ddckCKSrcBlt` off the `DD_SURFACE_LOCAL` the surface table remembers
  (valid until `DestroySurface`) and sends the record when it differs
  from what the host was told — as the DDK's sample drivers read it —
  which covers a key set before the surface was mirrored. The record goes
  into the batch ahead of the DP2 record, so the host has the key before
  the draw.
- **A flip does not move memory — found on the way.** The first case
  passed and the second read back black: CKTEST's per-case frame dumps
  and a flip / lock trace showed dxg's flip model on NT. `DdFlip` gets
  `lpSurfCurr` (front) and `lpSurfTarg` (back); the driver scans out
  `Targ`'s memory. Afterwards dxg does *not* exchange the two objects'
  `fpVidMem`: the handles keep their VRAM and the *roles* move — the
  `DDSCAPS_PRIMARYSURFACE` bit goes to the object now displayed, the
  application's `back` pointer means the other object from then on, and
  dxg tells the driver with a `CreateSurfaceEx` pair (same offsets,
  swapped caps). The M7c first cut re-registered `Targ` at `Curr`'s
  offset and vice versa in `DdFlip`, so from the second frame of every
  flip chain the host rendered and read back into the *displayed*
  buffer on alternate frames; the runtime's `SETRENDERTARGET` alternates
  the handle (2, 1, 2, 1 …) as the roles alternate, and with the stale
  offsets the app's `Lock` of its back buffer found nothing (black), or
  the previous frame. `D3D7TEST`'s golden compare never caught it (every
  frame identical, the last one read back before its flip), and a
  spinning scene looks the same either way. `DdFlip` re-registers
  nothing now.
- **Palettes never touch the driver.** A texture's palette reaches the
  host inside the DP2 stream: `SETPALETTE` (palette handle, flags with
  `DDRAWIPAL_ALPHA` 0x2000 when the entries' `peFlags` are alpha, surface
  handle) binds a palette to a surface and `UPDATEPALETTE` (palette
  handle, start, count, `PALETTEENTRY`s) fills it — the same two tokens
  DX7 and DX8 use (`SetPaletteEntries` / `SetCurrentTexturePalette` in
  DX8), and the executor answered "palettes are not supported" to them
  until now. `dwPalCaps` stays 0 and there are still no palette
  callbacks (the 8 bpp section: dxg drops the HAL otherwise).
- **The colour key goes by a record**, `D3DPT_OP_VRAM_COLORKEY` (handle,
  low, high, flags), sent by the texture-bind check above. The key values
  are the surface's own pixel values (0xf81f for magenta in R5G6B5, an
  index for P8), a range inclusive.
- **The host expands both to A8R8G8B8.** DXVK has no P8, and a key needs
  an alpha channel, so a P8 or keyed texture's host object is created in
  A8R8G8B8 (`host_format`) and `upload_texture` converts texel by texel:
  the palette's colour (alpha from `peFlags` when the palette is an alpha
  one, a grey ramp when no palette was set), the 16-bit formats decoded,
  alpha 0 for a texel whose raw value is inside the key range. A palette
  update marks every texture using it dirty; a key set or cleared
  releases the host object when its format changes (`refresh_object`).
  Since a bound texture can change under the runtime's nose (a palette
  edit, a `Lock` of a bound texture), every draw first re-uploads the
  bound stages whose surface is dirty (`pre_draw`) — the runtime re-sends
  `TEXTUREMAP` only on a `SetTexture`.
- **Keying is alpha 0 plus the alpha test, with one override.** Render
  state 41 (`COLORKEYENABLE`) is a host state now. While it is on and the
  texture at stage 0 has a key, the alpha test is forced on
  (`GREATEREQUAL 1`) unless the app runs its own (`ALPHATESTENABLE`),
  and — the part that matters for 1997 titles — stage 0's alpha op is
  made `SELECTARG1 TEXTURE` when the app's alpha pipeline does not read
  the texture alpha: the DX7 runtime's `TEXTUREMAPBLEND` emulation sets
  `ALPHAOP = SELECTARG2 DIFFUSE` for any texture format without alpha,
  which is every keyed R5G6B5 / P8 texture, and an alpha test cannot see
  a key the alpha pipeline threw away. The app's alpha test states and
  stage-0 alpha op come back the moment the key stops applying (state 41
  off, another texture bound, the key removed); an app change to those
  states while overridden is recorded and the override re-applied. The
  cost of the override: a title that colour-keys *and* blends by the
  diffuse alpha in the same draw loses the fade (it blends by the
  texture's alpha, 255 everywhere but the key) — the DX7 SDK's own
  `TEXTUREMAPBLEND` semantics make that combination rare.
- Tests: `tools/d3dpt-dp2-test.cpp` — a P8 texture through a palette
  (four entries, four cells), `UPDATEPALETTE` changing an entry under a
  bound texture, a keyed R5G6B5 checker with state 41 on (the keyed cell
  is the clear colour) and off (blue again), the app's own alpha test
  winning over the key, the key removed, `UPDATEPALETTE` beyond 256
  entries refused, `SETPALETTE` on an unknown surface ignored.
  `DRIVER\CKTEST.EXE` (`guest-tools/src/d3dptvid/cktest.c`) does it on XP
  through the DX7 API: a `DDPF_PALETTEINDEXED8` texture with an
  `IDirectDrawPalette`, `SetEntries` turning an entry blue, a R5G6B5
  texture with `SetColorKey(DDCKEY_SRCBLT, magenta)` drawn with
  `COLORKEYENABLE` on and off, every draw read back from the back buffer;
  `tools/xp-driver-test.sh <image> cktest` runs it and greps `cktest.log`
  for "0 failed".

## Execute buffers — the DirectX 3 path (2026-09-05)

Moto Racer (1997) takes the HAL with protocol v8's caps: the log shows
`d3dptdisp: d3d context 1 rt 2`, 256×256 textures with `setcolorkey`
lines, palettes through `SETPALETTE` / `UPDATEPALETTE`, and the flip
chain at 60/s — but the title screen, the menus and the race showed only
what the game draws itself (the 2D panels, the sky panorama, the HUD): the
executor counted **0 draws** over minutes of play, and the 160 DP2 calls it
did make in that time were state flushes (`tools/xp-motoracer.sh` boots
the disc, an Alcohol MDS/MDF through the cdimage driver; the game insists
on a 16 bpp desktop — `SETMODE 800 600 16` first).

The reason is the API generation. Moto Racer ships with DirectX 3 and
draws through **`IDirect3DDevice::Execute`**: execute buffers filled with
`D3DOP_*` instructions (`STATERENDER`, `PROCESSVERTICES`, `TRIANGLE`,
`EXIT`), textures bound by `D3DRENDERSTATE_TEXTUREHANDLE`, the viewport
cleared through a background material. XP's `d3dim.dll` (the DX3–6
runtime; `d3dim700.dll` is only `IDirect3D7`) emulates all that on a
DrawPrimitives2 driver, and two things in our HAL broke it. Both were
found with a new probe, `DRIVER\EBTEST.EXE`
(`guest-tools/src/d3dptvid/ebtest.c`), which does exactly what such a
title does — `IDirect3D` v1 by `QueryInterface` on the DirectDraw object,
the HAL device by `QueryInterface(IID_IDirect3DHALDevice)` on the back
buffer, `CreateViewport` / `SetViewport`, a material as the background,
`CreateExecuteBuffer` + `Lock` + `SetExecuteData`, `Execute`, textures
loaded with `IDirect3DTexture::Load` from a system-memory surface into an
`ALLOCONLOAD` video one and bound by handle — and logs every HRESULT:

- **Every `Execute` returned `E_OUTOFMEMORY` (0x8007000E), before a single
  token reached the driver.** `-rgb` runs the same program on the
  runtime's RGB software device, where it draws: the probe was right and
  the HAL path was wrong. The kernel log showed the runtime destroying its
  64 KiB DP2 vertex buffer inside the failing `Execute` and never getting a
  new one (the later, state-only DP2 calls came with a stale
  `lpDDVertex` that dxg resolved to a texture). The disassembly of
  `d3dim.dll` (XP SP3, base 0x6de70000) gave the mechanism: the Execute
  core (`0x6de7dc06`) first sizes its TL vertex buffer
  (`0x6de7dbb9`) as `max(D3DEXECUTEDATA.dwVertexCount clamped to 4096,
  D3DDEVICEDESC.dwMaxVertexCount) × 32` bytes, grows the buffer to that
  plus 0x400 (`0x6de73450`: release the old one, create a new
  `EXECUTEBUFFER | SYSTEMMEMORY` surface in user mode), and the vertex
  buffer constructor (`0x6de79a03`) refuses more than 0xffff vertices —
  and every failure of that creation is reported as `E_OUTOFMEMORY`
  (`0x6de73527`). Our `hwCaps.dwMaxVertexCount` was 65535: 65535 × 32 +
  0x400 bytes is 65567 vertices, one page over the limit, so the legacy
  path could never draw. **The cap is 4096 now** (the runtime's own clamp
  on what one Execute processes; `dwMaxBufferSize` stays 0 = unlimited);
  `ddflags=0x40000` puts 65535 back as the repro. `dwMaxVertexCount` is
  not consulted by the DX5+ interfaces, which is why D3D7TEST, FIFA and
  everything else never noticed. Withdrawing T&L (`0x1000`), the DX8 face
  (`0x2000`), adding the `*VIDEOMEMORY` device caps or a non-zero
  `dwMaxBufferSize` all changed nothing (all tried before the
  disassembly).
- **The stream then carried an unknown token 9 — and the vertices were
  zero.** The legacy path is a *pass-through*, not a translation: the
  Execute core walks the execute buffer's instructions and, in the
  UNCLIPPED mode, hands every run of driver instructions to
  `DrawPrimitives2` **as they are**, with `dwFlags` =
  `D3DHALDP2_EXECUTEBUFFER` (0x2), `lpDDCommands` = the app's execute
  buffer surface, `dwCommandOffset` = the current instruction, and
  `lpDDVertex` = the runtime's TL vertex buffer (`dwVertexLength` its
  whole capacity). The `D3DOP_*` opcodes share the DP2 numbering where
  the payloads match (`POINT` / `LINE` / `TRIANGLE` / `STATERENDER` are
  1 / 2 / 3 / 8 = `POINTS` / `INDEXEDLINELIST` / the legacy 8-byte
  `INDEXEDTRIANGLELIST` / `RENDERSTATE` — which is why the DP2
  enumeration skips 4–7 and 9–14), and the driver must consume those,
  plus `SPAN` (13, skipped) and `EXIT` (11, the end). Everything else is
  the runtime's — `PROCESSVERTICES` (9) first of all, also the matrix /
  light opcodes 4–7, `TEXTURELOAD`, `BRANCHFORWARD`, `SETSTATUS` — and
  the protocol for it is the *bounce*: the driver ends the call before
  such an instruction with `D3DERR_COMMAND_UNPARSED` (0x88760BB8) and
  the instruction's offset in `dwErrorOffset` (from the buffer's start,
  like `dwCommandOffset`); the runtime catches its state mirror up over
  what the driver consumed (`0x6de7d5e8`), executes the instruction
  itself (a `PROCESSVERTICES` COPY / TRANSFORM fills the TL buffer,
  `0x6de7da50`) and calls again from the next one. Our first attempt
  *skipped* opcode 9 instead: `Execute` then succeeded and the
  triangles arrived — with an all-zero TL buffer, because nobody had
  copied the vertices (the runtime's `D3DParseUnknownCommand`, which we
  now store — its `lpvData` *is* the function, expected size 0 — answers
  `D3DERR_COMMAND_UNPARSED` for these opcodes too; it is not the
  mechanism). `walk` does the bounce now (`d3dptdisp: execute buffer:
  opcode 9 x1 bounced to the runtime at 0x34`, the first eight); a call
  that starts on a runtime instruction bounces without a host round trip.
  No caps steer any of this (T&L, transform caps, `bClipping` are not
  consulted), and a CLIPPED `Execute` never reaches the pass-through:
  the runtime transforms and emits DP2 draws itself, which is why the
  probe's CLIPPED case worked from the first run.
- **The DirectX 5 texture render states.** In the pass-through the
  render states come as the app wrote them, and a DX3 title binds its
  texture with `D3DRENDERSTATE_TEXTUREHANDLE` (1, the surface handle
  `IDirect3DTexture::GetHandle` returned — dxg's, i.e. ours) and picks
  its blend with `TEXTUREMAPBLEND` (21); the DX6+ runtimes turn these into
  stage states before the driver sees them, so the executor had always
  dropped them. It maps them now (`legacy_render_state`): 1 binds stage 0
  and applies the blend, 21 sets stage 0's colour / alpha ops the way
  the old fixed function did (no texture: the diffuse alone; `DECAL` /
  `COPY`: the texels; `MODULATE`: texels × diffuse with the alpha from the
  texture when its format has one — the colour-key expansion counts,
  and `apply_ckey` still overrides a keyed texture — else from the
  diffuse; `DECALALPHA`, `MODULATEALPHA`, `ADD`), `TEXTUREADDRESS` (3)
  and `TEXTUREMAG` / `TEXTUREMIN` (17 / 18, the six DX5 filters onto
  MIN / MIP) go to stage 0's sampler, `WRAPU` / `WRAPV` (5 / 6) to
  `WRAP0`. `tools/d3dpt-dp2-test.cpp` covers the legacy triangle list
  with `TEXTUREHANDLE` + `MODULATE`, `DECAL`, and handle 0.
- **Result:** `EBTEST` passes its five cases through XP's own
  `d3dim.dll` on the HAL (`xp-driver-test.sh <image> ebtest`: the Clear,
  the flat quad, the textured quad, the keyed quad with the key cut out,
  the CLIPPED transformed quad — 0 failed on `winxp-m7g`, 2026-09-05),
  and the RGB control still draws. Per `Execute` the runtime makes one
  DP2 call per run of driver instructions plus one bounce per runtime
  instruction, each a synchronous doorbell round trip, and the TL
  buffer's 2048 × 32 bytes ride along with every call that draws — a
  DX3 title with many small execute buffers pays for that; batching is a
  follow-up if one shows it.

**Moto Racer plays on the HAL** (2026-09-05, `tools/xp-motoracer.sh play`,
`build/xp-driver-test/moto4/`): the name screen's 3D letters, the showroom
bike under its spotlights, and the Speed Bay race — track, kerbs, the
bike and rider, the palms and buildings cut out by colour key, the HUD —
at 120 frames/s under KVM (`ddi: 120.0 frames/s (600 readbacks, 2400 dp2
calls, 106573 draws in 5.0 s)`: four DP2 calls and ~175 draws per frame).
Played by hand by the user the same evening (the player on `winxp-m7g`,
after the untracked-writes fix below): works great, and fast under TCG
as well — a DirectX 3 title's guest side is only execute-buffer
building, the rasterization is the host GPU's.

**The one-triangle draws cannot be batched (2026-09-05, later).** A traced
race frame (`D3DPT_DP2_TRACE`, the attract demo on the canyon track) has
8 DP2 calls (6 `Execute`s of ~3.5 KB each plus the scene start and end),
356 `D3DOP_TRIANGLE` draws — and 356 `TEXTUREHANDLE` render states, one
before every draw, alternating between a handful of textures (0x8, 0xb,
0xd, 0x1d, 0x21 …), with Z off (`rs 7 = 0`): the game sorts its polygons
back to front and emits them in that order, switching texture per
polygon. Consecutive triangles under one texture are already one
`D3DOP_TRIANGLE` entry with `wCount` > 1 (the name screen draws its 3D
letters as a single 185-triangle entry, the showroom's bike in entries of
up to 14), so there is no run of same-state draws left to merge, and
merging across the texture switches would break the painter's order.
The 356 draws a frame cost DXVK nothing measurable (120 frames/s, paced
by the flip chain); the item is closed.

**The showroom's 2D panels (2026-09-05, later): untracked guest writes.**
They were not a timing accident: in every run the bike-selection screen
showed the bike under its spotlights and nothing else — no "CHOOSE BIKE"
header, no arrows, no features panel, no Start / Back — while a click on
the invisible Start button still started the race. The traced showroom
frame is only the bike (79 draws, no clear, the first draw's snapshot
already carrying the spotlight backdrop, so the backdrop's blit is a
tracked write), and the 2D panels are never in VRAM when the host reads
the frame back: they are written by the guest *without* a `DdLock` /
`DdUnlock` pair — GDI through `GetDC` on the DirectDraw surface, which
dxg handles with no driver callback since the driver has no
`DrvDeriveSurface` — so the driver never sends `VRAM_DIRTY`, and the
executor's readback of the host frame overwrote them every frame. Fixed
in the executor by comparing the target's VRAM with a shadow of it (the
section "Untracked writes" below): the panels are there
(`build/xp-driver-test/moto9/bike2.png`), the device logs ~52 k
untracked pixels a frame in the showroom and ~36 k in the race (the
HUD's text and dials), plus a 576-pixel one now and then that is the
24×24 software mouse cursor GDI paints into the primary.

## Untracked writes — GDI on a DirectDraw surface (2026-09-05)

The M7c contract between the driver and the executor was: the guest's
writes to a render target's VRAM are announced by `VRAM_DIRTY` (sent from
`DdUnlock`, and by the driver's own TEXBLT / system-memory copies), the
host uploads the target before drawing when it is dirty, and the host
frame is read back into VRAM at EndScene / Lock / Flip. Two ways a guest
writes a surface never pass through `DdLock` / `DdUnlock`:

- **`GetDC` on a DirectDraw surface.** ddraw's `IDirectDrawSurface::GetDC`
  is `NtGdiDdGetDC` → dxg, which builds a GDI surface over the
  DirectDraw surface's memory. A driver that exports `DrvDeriveSurface`
  gets asked for that surface; ours does not, so dxg wraps the VRAM
  itself and GDI's `TextOut` / `BitBlt` / `Rectangle` write straight
  into it. There is no driver callback on either side of it.
- **The engine's software cursor.** Without a hardware pointer GDI paints
  the mouse cursor into the primary surface directly (24×24 = 576
  pixels a move); in a flip chain the primary is the next frame's back
  buffer.

Moto Racer 1997 draws every 2D panel of its bike-selection screen and
the race HUD's text through the first: the panels were missing from
every showroom screendump (the section above) while the backdrop and
the 3D bike were fine. Rather than add `DrvDeriveSurface` (which would
give the driver a hook to send `VRAM_DIRTY` — but only on the way in;
GDI does not tell the driver when it is done with a derived surface,
so a readback could still overtake a `TextOut` in progress), the
executor now keeps a **shadow of each render target's VRAM** as of the
last moment the host and VRAM agreed — taken after every upload and
every readback — and uses it twice a frame:

1. Before the frame's first draw (once per readback cycle, in
   `bind_ctx`), a target that is not dirty is compared with its shadow;
   any difference is a guest write the driver never announced, and the
   target is uploaded as if it had been (the draws then land on top, as
   they would on real hardware where the write preceded them).
2. At the readback, pixels that differ from the shadow are the guest's
   writes since the frame's draws began (the HUD after the scene, GDI
   during it): they are **kept over the host frame**, and the target is
   marked dirty so the next frame's first draw refreshes the host
   target from VRAM (the kept pixels persist where nothing draws over
   them, exactly as on hardware).

A full-target `Clear` skips the check as it skips the upload. The cost
is one `memcmp` of the target (600 KB at 640×480×16) per frame, per row
first and per pixel only on rows that differ, and, while a title does
untracked writes after its scene, one upload of the target per frame;
Moto Racer's showroom and race both stay at 120 frames/s under KVM. The
device log counts the pixels (`ddi: … N untracked guest pixels in 5.0 s`,
and the first eight events with the target's handle and which of the
two paths took them); the DP2 trace notes them at the readback.
`tools/d3dpt-dp2-test.cpp` covers both paths and the persistence: a
block poked into the target's VRAM with no `VRAM_DIRTY` before a draw
is drawn over where the quad is and read back as itself elsewhere, one
poked after the draws survives the readback, and in the next frame it
is drawn over where the quad is and persists elsewhere.

`DrvDeriveSurface` stays on the list as an optimisation (GDI could then
draw on a host-side copy) rather than a correctness item.

## The hardware cursor (2026-09-05 evening, register set v4)

The user's report after playing Moto Racer by hand: the mouse cursor
flickers. The driver had no pointer hooks, so GDI painted a software
pointer into VRAM — erased and redrawn around every drawing operation
(the scanout catches it mid-erase on the desktop) and always into the
GDI primary at offset 0, which under a flip chain is one of the two
buffers: in a full-screen title that flips at 60 Hz and leaves the
Windows cursor showing, it is visible every other frame.

The fix is the sprite every card of the era had, in three parts:

- **Driver.** `DrvSetPointerShape` takes GDI's pointer (a 1 bpp mask
  surface, AND rows over XOR rows, plus a colour surface with a
  translation object for colour pointers; `SPS_ALPHA` marks a 32 bpp
  one with its own alpha) and writes it as a8r8g8b8 into the 16 KiB
  above the DirectDraw heap (`dd_heap_end`, the heap ends there now),
  then CURSOR_ADDR / W / H / HOT_X / HOT_Y and CURSOR_DEFINE.
  Monochrome pointers: AND 1 + XOR 0 is transparent, AND 0 is black or
  white by XOR, AND 1 + XOR 1 ("invert the screen") is black — a sprite
  cannot invert. Colour pointers in another format go through a 32 bpp
  engine bitmap and `EngCopyBits` with the translation. Anything over
  `D3DPT_FB_CURSOR_MAX` (64) is `SPS_DECLINE`d and GDI keeps its
  software pointer for it. `DrvMovePointer` writes CURSOR_X / Y and
  CURSOR_ENABLE (x = −1 hides); `GCAPS_ASYNCMOVE` lets GDI move it from
  any context. `DrvAssertMode(FALSE)` hides it before the VGA text.
- **Device.** CURSOR_DEFINE reads the image out of VRAM into a
  `QEMUCursor` and `dpy_cursor_define`s it on the console; X / Y /
  ENABLE writes are `dpy_mouse_set`. Nothing is composited into the
  frame: a headless screendump shows no cursor, as with a real sprite,
  and the log has the first defines and moves (`cursor 32x32 hot 0,0
  defined`, `cursor shown at x,y`).
- **Player.** The embed library already forwarded QEMU's cursor
  callbacks (`on_cursor`, `on_mouse_set`); the player now takes them:
  the shape becomes a winit custom cursor and, while the pointer is
  over the image and the guest shows its cursor, the host window's
  cursor *is* the guest's shape. With the USB tablet the host pointer
  is exactly where the guest's cursor is, so there is no compositing
  and no added latency; when the guest hides its cursor (a game's
  `ShowCursor(FALSE)`) the host cursor is hidden over the image, and a
  guest without a hardware cursor (cirrus, vga.sys, an older driver)
  keeps the old behaviour — hidden over the image, the software pointer
  in the frame. The state is applied only on change (wakes come every
  frame). **That is the tablet path only.** With a relative mouse (no
  `usb-tablet`: the PS/2 mouse, the player's grab) the host pointer is
  nowhere near the guest's cursor, so the player composites the sprite
  into the frame at the position the guest reports, alpha-blended, and a
  move alone republishes the frame so the sprite follows between guest
  redraws (the first cut had only the host-cursor path and the user saw
  no cursor at all in Moto Racer without the tablet). The headless dump
  takes the sprite path, so a `PLAYER_DUMP_OUT` frame shows the cursor.
  User-confirmed 2026-09-05 evening: steady on the desktop, present in
  Moto Racer's menus, no blink.

`D3DPT_FB_VERSION` is 4: an installed v3 driver refuses the device, so
every image needs a reinstall from the ISO, and the QEMU rebuild goes
with it (prepare → ninja). Verified headless by the device log lines at
the desktop after the install; the flicker itself is a player-window
observation for the user.

## Blit caps and the HEL (2026-09-05, evening; nothing landed)

FIFA 2000's videos (EA's `.mad` files, decoded by the game) play at
320×240 in the middle of the 640×480 mode instead of filling it. The
first guess was the blit caps: the driver claims none (`dwCaps` 0, the
HEL does every blit in user mode on the mapped VRAM), and a 1999 title
that asks `DDCAPS_BLTSTRETCH` before stretching would settle for 1:1.
Tried: `DDCAPS_BLT | BLTSTRETCH | BLTCOLORFILL | BLTQUEUE | CANBLTSYSMEM`
with the `DDFXCAPS_BLT*` stretch / shrink bits, in `dwCaps` and the
SVB / VSB / SSB sets alike, with `DdBlt` returning
`DDHAL_DRIVER_NOTHANDLED` for everything so that the HEL would still do
the work. Two findings, both against:

- **On XP a declined `DdBlt` is not a HEL fallback.** With the caps
  claimed, dxg routes every blit to the driver and `NOTHANDLED` comes
  back to the application as `E_NOTIMPL` (DDTEST's windowed
  `Blt(DDBLT_COLORFILL)` failed with 0x80004001 at 8, 16 and 32 bpp).
  Whatever the Windows 9x DDK said about the HEL taking over, the NT
  runtime does not: claiming blit caps means writing the blitter (copy,
  colour fill, stretch, source colour key, ROP) in the display DLL on
  the cached VRAM mapping, or on the host once plain surfaces have a
  host copy. Not worth it for now — the HEL's user-mode copies are
  fast enough for the 2D titles seen so far.
- **The videos never blit.** With the caps in place FIFA 2000 played
  the intro at the same 320×240, and the driver logged no `DdBlt` at
  all through the whole intro: the game writes the decoded frames into
  the surface itself (Lock) and chooses the size by something else
  (its own CPU or resolution heuristics, or the movies simply are
  320×240 and were on the hardware of the day). Open until someone
  sees them full-screen on a known configuration.

Kept from the experiment: `DdBlt` logs its first calls with source and
destination rectangles, and the `Blt` callback is registered whenever
`DDF_CKEY_NOBLTCB` is not set (it has to exist for the colour-key
caps, and it is never called without `DDCAPS_BLT`).

## 8 bpp palettized modes (2026-09-04, register set v3)

The late-90s 2D titles (Diablo, StarCraft, Age of Empires, Caesar 3) set
640×480×8 through DirectDraw and animate the palette; with only 16/32 bpp
modes in the table `SetDisplayMode` failed and each stopped at its "could
not set display mode" box. Now every size is also offered at 8 bpp (63
modes), across the four layers:

- **Device** (`d3dpt_fb.h` v3, `d3dpt_vga.c`): `BPP = 8`, a 256-entry
  `PALETTE` register block (x8r8g8b8 per entry, 0x400..0x7fc of the
  register page, `CAP_BPP8`). The scanout is a pixman `c8` view of VRAM
  through a `pixman_indexed_t` filled from the registers, converted into
  the x8r8g8b8 shadow per dirty span exactly like the 16 bpp path; a
  palette write marks the palette dirty and the next refresh repaints the
  whole frame (a palette change recolours every pixel). Each entry write
  is one MMIO exit: a full 256-entry animation costs ≈0.5 ms under KVM
  (DDTEST's 8 bpp chain with a `SetEntries` every frame runs at 1200 fps
  against 3800–6400 for the RGB chains) — fine for a game's 20–30 palette
  updates a second; a RAM-backed palette page is the optimisation if a
  title ever needs more.
- **Miniport**: 8 bpp mode entries carry `VIDEO_MODE_PALETTE_DRIVEN |
  VIDEO_MODE_MANAGED_PALETTE`, 8 bits per gun, no masks;
  `IOCTL_VIDEO_SET_COLOR_REGISTERS` writes the CLUT into the block.
- **Display driver, GDI:** `GCAPS_PALMANAGED | GCAPS_COLOR_DITHER`, a
  `PAL_INDEXED` 256-entry default palette (the 20 system colours at 0–9
  and 246–255, a 6×6×6 cube, 20 greys), `BMF_8BPP` surfaces, `ulNumColors
  = 20`, `ulNumPalReg = 256`, `HT_FORMAT_8BPP`, and `DrvSetPalette`
  (`PALOBJ_cGetColors` → the PALETTE registers directly, the IOCTL only
  while the register page is unmapped). XP's desktop at 800×600×8 comes up
  in the classic theme with the wallpaper dithered through the system
  palette, as on any palette-managed adapter.
- **Display driver, DirectDraw:** `ddpfDisplay = DDPF_RGB |
  DDPF_PALETTEINDEXED8`, 8 bits, no masks. Two runtime findings, both
  from the disassembly of the image's `dxg.sys` / `ddraw.dll`
  (`build/xp-driver-test/pal-*`, DDTEST at 8 bpp answering
  `DDERR_UNSUPPORTEDMODE` from `CreateSurface` until both were in):
  1. **`dwPalCaps` must stay 0 and there must be no palette callbacks.**
     dxg's post-enable validation (the function that also rejects
     `DDCAPS_GDI`, `DDCAPS_BANKSWITCHED`, `DDSCAPS_MODEX`…) fails the HAL
     outright when `ddCaps.dwPalCaps != 0`; the probes stop after
     `GUID_MotionCompCallbacks`, never reaching `GUID_GetHeapAlignment`.
     On NT the primary's palette is GDI's: `IDirectDrawPalette::SetEntries`
     on the primary's palette lands in `DrvSetPalette`, so the
     `DdCreatePalette` / `DdSetEntries` / `DdSetPalette` callbacks of the
     DDK header are dead on XP. `DDPCAPS_8BIT | DDPCAPS_ALLOW256` palettes
     work without any of it.
  2. **The HAL's Direct3D must not disappear between modes.** The first
     8 bpp cut hid Direct3D (`d3d_init` refused palettized PDEVs); dxg then
     completed every probe but `ddraw.dll`'s object rebuild after
     `SetDisplayMode` still failed (`GetCaps` afterwards showed
     `DDCAPS_NOHARDWARE`, `dwModeIndex = DDUNSUPPORTEDMODE`). With
     `-device d3dpt-vga,ddflags=0x20` (no Direct3D in any mode) the 8 bpp
     chain worked at once, so it is the change of shape across the mode
     switch that the runtime rejects, not 8 bpp. Direct3D is therefore
     offered at 8 bpp too; the runtime refuses `CreateDevice` on a
     palettized primary by itself.
- **Test:** `DDTEST 640 480 8 300` creates a `DDPCAPS_8BIT | ALLOW256`
  palette of four ramps on the primary, draws diagonal bands through all
  256 indices, fills the bar with index 255 and rotates the palette by one
  entry per frame with `SetEntries`; `ddtest.bmp` is written through the
  palette. `tools/xp-driver-test.sh <image> ddtest` now runs 8, 16, 32 and
  windowed from a staged `E:\RUN.BAT` (the chain outgrew the Run dialog:
  the earlier truncated line was why `dd32.log` went missing once) and
  pulls `dd8.log` / `dd8.bmp`; a QMP screendump during the 8 bpp run shows
  the ramps through the device's conversion
  (`build/xp-driver-test/pal-shot/cmd-02.png`).
- **Diablo (1.00, the retail disc) plays.** `tools/xp-diablo.sh install
  <image>` runs Blizzard's installer (three clicks, the game starts by
  itself), `play` runs `C:\Diablo\Diablo.exe`: the Blizzard North and
  intro videos, the title menu with its palette-cycled flames, a new
  Warrior into Tristram, a click to walk, the character sheet, all at
  640×480×8 through the palette; the QMP screendumps (`title.png`,
  `town.png`, `walk.png`, `char.png` in `build/xp-driver-test/diablo-play/`)
  are the device's conversion and show the right colours. The whole run
  takes 2 minutes under KVM. Quirks: the game's menus ignore QMP clicks
  (keyboard instead), the installer's "DirectX 2.0 cannot be detected
  (sound)" warning is the missing sound card, alt+F4 exits the game.
  Not tested: the dungeon levels (the light radius is palette work too),
  a sound card, TCG timing.

## M7b / M7c pointers

- **M7b DirectDraw DDI:** landed in its first cut (above); 8 bpp
  palettized modes since 2026-09-04 evening (Diablo plays); the flip
  chain's vertical blank since 2026-09-05 (below). Left:
  `DrvDeriveSurface`, a present signal in phase with the player's
  swapchain, StarCraft / Age of Empires / Caesar 3 as further 8 bpp
  titles.
- **M7c Direct3D DDI: first cut landed (see the section below); FIFA 2000
  runs on it out of the box (intro, title, attract-mode match).** Left:
  a user-driven match (input, menus, frame rate), claiming T&L, DX8
  tokens / `GUID_D3DCaps` for `D3DCAPS8`, colour keying, render-to-texture,
  state sets. Exit: doc 04 matrix with no DLL in the game folder.

## M7c — the Direct3D DDI (2026-09-04, first cut)

The DX7 HAL behind the display driver, on the doc 14 protocol and executor.
Nothing new was invented for the transport: the adapter's VRAM grew to
128 MiB and its top 64 MiB is a command window in exactly the SysBus
device's layout (`d3dpt_proto.h`: header page, records, return area), so
the guest encoder `d3dpt_enc.h` and `d3dpt_exec_submit` work unchanged;
the DOORBELL register submits the window, D3D_STATUS says whether the
host has an executor, CMD_OFFSET where the window is (register set
version 2, `d3dpt_fb.h`). The executor library is dlopened once per
process (`d3dpt/hw/d3dpt_exec_load.c`, shared with the SysBus device);
each device has its own instance.

```
guest (XP)                                      host
 ddraw.dll (DX7 runtime: T&L in software)        d3dpt-vga: VRAM [heap | 64 MiB window]
   └ dxg.sys ── DDI ──> d3dptdisp.dll              DOORBELL ──> libd3dpt_exec (d3dpt_exec_ddi.cpp)
        CreateSurfaceEx → VRAM_SURFACE record        surface handle → DXVK texture / render target
        ContextCreate  → CTX_CREATE                  d3d9 device, SetRenderTarget + depth
        DrawPrimitives2 → DP2 record (tokens+verts)  token interpreter → DrawPrimitiveUP etc.
        SceneCapture END / Lock / Flip → READBACK    GetRenderTargetData → memcpy into VRAM (+dirty)
        Unlock → VRAM_DIRTY                          texels re-read from VRAM before the next draw
```

- **Surfaces stay in guest VRAM.** dxg's heap allocates every DirectDraw
  surface below the window as before; `DdCreateSurfaceEx`
  (`GUID_Miscellaneous2Callbacks`) registers each one by its
  `dwSurfaceHandle` with VRAM offset, size, pitch, D3DFORMAT (translated
  from the DDPIXELFORMAT) and caps; mip chains send the attached levels'
  offsets. The host creates the DXVK object lazily: a MANAGED texture whose
  levels are filled from the VRAM pointer, a render target, or a depth
  surface (D16/D24X8/D24S8, with fallbacks). `DdUnlock` on a texture or a
  target sends `VRAM_DIRTY` and the host re-reads the texels before the
  next use; `DdDestroySurface` (`NOTHANDLED`, dxg frees the block) sends
  `VRAM_RELEASE`.
- **Rendering goes to a host render target; READBACK brings it back.**
  A context (`D3dContextCreate`) is a render-target + Z pair; the d3d9
  device is created on the first one (backbuffer unused) and the context's
  targets are set on it. The frame is copied into the render target's VRAM
  at `SceneCapture END` (EndScene), at `DdLock` of a target and in `DdFlip`
  before the OFFSET write, so flips, HEL blits, GDI and screenshots see it;
  the copy is skipped when nothing was drawn since (`S_FALSE`). After a
  flip dxg exchanges the two surfaces' VRAM, so `DdFlip` re-registers both
  with swapped offsets (the host keeps the objects, marks them dirty). A
  full `Clear` of the target skips the upload of stale VRAM content; a
  partial one or a draw onto a HEL-blitted background uploads it first
  (sysmem staging → default-pool surface → StretchRect).
- **DrawPrimitives2** copies the runtime's command buffer and the vertex
  buffer (`D3DHALDP2_USERMEMVERTICES` is a user pointer, read directly in
  the caller's context) into one `DP2` record and rings the doorbell; the
  render-state array the runtime keeps next to the stream (`lpdwRStates`)
  is mirrored from the RENDERSTATE tokens by the driver. The host
  interprets the tokens on `IDirect3DDevice9`: RENDERSTATE (DX7 states
  with a d3d9 twin pass through; the DX5/6 ones — texture handle, stipple,
  ROP, colour key — are dropped, once logged), TEXTURESTAGESTATE
  (TEXTUREMAP = surface handle → `SetTexture`; address / filter / LOD
  states → sampler states with the DX7→d3d9 filter renumbering; the rest
  1:1), VIEWPORTINFO + ZRANGE → the viewport (re-applied after every
  target change, d3d9 resets it), SETRENDERTARGET, CLEAR, the list / strip
  / fan tokens and the `_IMM` variants → `DrawPrimitiveUP`, the indexed
  ones → `DrawIndexedPrimitiveUP` over the touched vertex range (the
  `…2` variants add their start vertex), SETMATERIAL / SETLIGHT /
  SETTRANSFORM (WORLD renumbered) → their d3d9 calls for the day T&L is
  claimed, WINFO / SETPRIORITY / SETTEXLOD / CREATELIGHT accepted and
  ignored, STATESET / TEXBLT / palettes logged and skipped. Malformed
  streams answer `D3DERR_COMMAND_UNPARSED` with the offending offset
  (`dwErrorOffset`); out-of-range vertex references skip the primitive.
- **Caps:** `lpD3DGlobalDriverData` (V1 desc: FLOATTLVERTEX, DRAWPRIMITIVES2
  + 2EX, HWRASTERIZATION, TEXTUREVIDEOMEMORY, no HWTRANSFORMANDLIGHT;
  tri/line caps: Z, all blends and compares, Gouraud + specular, fog,
  point/linear/mip filters, wrap/mirror/clamp/border; 9 texture formats:
  X8R8G8B8, A8R8G8B8, R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4, DXT1/3/5),
  `lpD3DHALCallbacks` (ContextCreate/Destroy/DestroyAll, SceneCapture),
  `GUID_D3DCallbacks3` (Clear2, ValidateTextureStageState = 1 pass,
  DrawPrimitives2), `GUID_D3DCallbacks2` (SetRenderTarget),
  `GUID_D3DExtendedCaps` (4096² textures, 8 stages, all texture ops,
  stencil), `GUID_ZPixelFormats` (D16, D24X8, D24S8), `DDCAPS_3D` +
  `DDSCAPS_3DDEVICE|TEXTURE|ZBUFFER|MIPMAP`. `DdCanCreateSurface` accepts
  exactly the formats the host mirrors. `-device d3dpt-vga,ddflags=0x20`
  turns Direct3D off (the M7b DirectDraw-only HAL) without a reinstall;
  0x40 / 0x80 / 0x100 / 0x200 / 0x400 / 0x800 drop the 3D caps bits, every
  D3D GetDriverInfo answer, the D3D buffer callbacks, Callbacks3, Misc2 and
  ParseUnknownCommand one at a time (the bisection knobs below).
- **The finding of the day: `GetDriverState` is mandatory.** With the
  first cut XP's `ddraw.dll` reported `DDCAPS_NOHARDWARE` and no 3D at
  all: the whole HAL dropped, as with `DDCAPS_GDI` in M7b. The `ddflags`
  bisection (five runs) pinned it on the `GUID_Miscellaneous2Callbacks`
  answer, and the disassembly of `ddraw.dll` (pulled out of the image
  with `qemu-img convert` + `7z`) shows why: after validating that table
  the runtime checks `lpD3DGlobalDriverData->hwCaps.dwDevCaps &
  (D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWTRANSFORMANDLIGHT)` and, if
  set, requires `GetDriverState` to be non-NULL, else the object creation
  fails and the HEL-only object is built. Neither the DDK docs nor the
  samples say so. The same pass showed the user-mode GetDriverInfo probe
  (a mangled `GUID_DDStereoMode` the driver must *refuse*), that
  `dxg.sys` queries Callbacks3 and the ParseUnknownCommand pointer at
  enable time and drops Callbacks3 when the latter is refused, and that
  every answer must not exceed `dwExpectedSize` (guard words after the
  buffer are checked). `DdGetDriverState` answers DD_OK with the buffer
  untouched. The second trap of the day was cheaper: `CreateDevice` failed
  with `DDERR_INVALIDPIXELFORMAT` at 32 bpp but worked at 16 bpp, because
  the `DDBD_*` bit-depth flags count *down* (`DDBD_16` 0x400, `DDBD_24`
  0x200, `DDBD_32` 0x100) and the self-contained header had them the
  other way; `dwDeviceRenderBitDepth` is what the runtime checks the
  render target against.
- **Tests.** `tools/d3dpt-dp2-test.cpp` (host stage of `scripts/test.sh`)
  registers a render target, a Z buffer and a texture in a malloc'ed
  VRAM, opens a context and sends the D3D7TEST scene as the DP2 tokens
  the runtime would emit (cyan triangle behind a wrapped checkerboard
  quad, Gouraud fan in front, half-transparent strip: Z test, texture
  wrap, alpha blend), reads back and checks pixels, then feeds hostile
  records (surface beyond VRAM, a record lying about its length, a
  truncated stream, out-of-range vertices, a released texture) and expects
  each refused without killing the executor. `DRIVER\D3D7TEST.EXE`
  draws the same scene through `IDirect3DDevice7` on XP;
  `tools/xp-driver-test.sh <image> d3d7` runs it headless and diffs its
  BMP against the host test's frame.
- **What is not there yet:** T&L (the runtime transforms; claiming
  HWTRANSFORMANDLIGHT is a caps + `D3DCAPS8` change, the tokens are
  mapped), DX8's tokens (streams, shaders, `GUID_D3DCaps`), colour keying
  (`D3DRENDERSTATE_COLORKEYENABLE` is dropped; the plan is key → alpha at
  upload plus alpha test), render-to-texture (a texture with 3DDEVICE
  becomes a render target the host cannot sample), state sets, driver-
  managed textures (TEXBLT), palettized textures, more than one context
  sharing device state, presenting the host frame straight through the
  player's 3D path instead of the readback copy (every frame is a
  1.2 MB `GetRenderTargetData` + memcpy at 640×480×32 today).

Results 2026-09-04 (KVM, RADV host, XP SP3, `tools/xp-driver-test.sh
~/vms/winxp-m7c.qcow2 d3d7`): `D3D7TEST 640 480 32 300` enumerates the
"Direct3D HAL" device (devcaps 0x8ae51, textures 1..4096, 8 stages),
creates the flip chain + 16-bit Z + a 64×64 A8R8G8B8 texture, renders
the scene through DrawPrimitives2 (61 DP2 calls for the run: the runtime
batches every draw of a frame into one call) and reports

| case | result |
|---|---|
| 640×480×32, 300 frames, 4 draws + clear each | 2400–2700 fps |
| 640×480×16, 60 frames | 1277 fps (first run of the session, includes the device creation) |
| the back buffer read through `Lock` after `EndScene` | byte-identical to `build/d3dpt-dp2-test`'s frame (0 of 307200 pixels differ) |

Per frame that is one 1.2 MB readback (`GetRenderTargetData` +
memcpy) plus the page flip; the executor logs the DX7-only render
states the runtime sends (4, 10, 30, 33, 47: texture perspective, line
pattern, ZVISIBLE, stippled alpha, ZBIAS) once and drops them.

### FIFA 2000 on the HAL (2026-09-04)

The first real DX7-era title, the one that was parked on WineD3D (doc 00
known issues, doc 14): with the WineD3D DLLs renamed out of the game
folder (`tools/xp-fifa2000.bat`, run by `GAME_ISO=FIFA2000.ISO SHOTS=24
tools/xp-driver-test.sh ~/vms/winxp-m7f.qcow2 bat tools/xp-fifa2000.bat`)
the game's own DirectX renderer (`THRASH\dx6z.dll`, registry `Thrash
Driver = dx`, `Hardware Acceleration = 1`, `Thrash Resolution = 800x600`)
runs on the HAL **unmodified**: the EA intro at 640×480×16 (19 DP2 calls,
page flips), the title screen, then the attract-mode match at 800×600×16
with textured players, kits, crowd, pitch and the HUD, all through
`DrawPrimitives2` on DXVK and the readback into VRAM (screendumps in
`build/xp-driver-test/fifa/cmd-*.png`). The match does not page-flip (4
`scanout offset` lines for the whole run): the game blits its back buffer
to the primary, so every frame is READBACK + a driver-side blit. Executor
log for the run: the DX5/6 render states dropped once (1–6, 10–13, 17,
18, 21, 30–33, 39, 40, 43–47, 49: texture handle / address / filter /
map-blend, ROP, plane mask, stipple, ZBIAS…), DXVK's own "unhandled
render state 26 / 62 / 128" (dither, an unknown, WRAP0), no colour keying
requested, no unsupported token, no refused record. Nothing crashed;
the run ends with a clean power-down while the match is still playing.
Not yet measured: the frame rate (the player shows it), input (the
headless loop never touches the game), a full user-driven match with the
menus, the 640×480 in-game resolution.

Played by hand the same day: graphics clean, smooth under KVM. The user
found **the keyboard dead in the match under TCG** (`-cpu pentium3`, no
KVM) and working under KVM. What the investigation established
(2026-09-04, all on this Linux box, headless runs driven over QMP with
`tools/xp-driver-test.sh`, plus the real player with `PLAYER_KEYS`):

- Not the emulator's input path. `DRIVER\DITEST.EXE` (a game-style
  DirectInput keyboard, exclusive + foreground, with a busy loop between
  polls) sees every key under TCG, in bare QEMU and through the player's
  own queue; the embed library's new `qemu-embed: input:` statistics show
  no drain latency over 20 ms and no key down/up pair delivered in one
  drain. Not XP's `LowLevelHooksTimeout` (5000 ms changes nothing). Not
  the frame rate: the match renders at the game's own 30 fps cap under
  TCG here (`d3dpt-vga: ddi: 30.0 frames/s` in the log).
- The game itself. `D3DPT\DINPUT.DLL` next to the EXE (a forwarding shim
  that logs the game's DirectInput use, `dinput_log.txt`) shows FIFA's
  keyboard device is `DISCL_NONEXCLUSIVE | DISCL_FOREGROUND`, polled with
  `GetDeviceState` 30 times a second, no errors — and in the match it
  reports **no key at all**, KVM or TCG, while a sampler thread in the
  same process sees every key through `GetAsyncKeyState`. The front end
  (title screen, side selection) works through the same device. On XP a
  non-exclusive DirectInput keyboard is fed by a low-level hook that runs
  on the thread which created the device, only while that thread services
  its message queue; FIFA's match loop does not pump. Why the user's KVM
  session got through is not settled (likely the loop's idle time on a
  fast CPU); the headless KVM runs did not.
- The fix is in the shim: every key `GetAsyncKeyState` reports pressed is
  set in the keyboard state handed back (DIK from the scan code, the
  extended keys mapped by hand), logged once per key when DirectInput's
  own state lacked it. With `DINPUT.DLL` in the game folder the match
  takes 100 ms taps (F2 camera, Esc pause, F12 exit) under KVM and TCG
  alike. The user-facing recipe: copy `D3DPT\DINPUT.DLL` next to
  `fifa2000.exe`; `tools/xp-fifa2000.bat` does it from `E:\DINPUT.DLL`.
  **Confirmed by the user 2026-09-05, both directions:** on a TCG run on
  this Linux box the match takes keys with `DINPUT.DLL` next to the EXE, and
  moving the DLL away brings the dead keyboard straight back. That is the
  causal check the headless harness alone could not give — the shim is the
  variable, not the driver, the CPU model or the run.
- **How much this matters, from the same day:** the user's everyday setup is
  the Linux host run natively (KVM) with `-vga none -device d3dpt-vga`, and
  there they have **no input issues and no custom DLLs anywhere** — no
  WineD3D set renamed out of a game folder, no shim next to any EXE, a stock
  XP on the driver. So the unpumped-hook symptom is a **TCG-only** one, as
  the KVM/TCG split above already suggested: the game's match loop does pump,
  rarely, and only a guest slow enough to stretch the gaps lets the hook fall
  behind. `DINPUT.DLL` is therefore medicine for the Apple Silicon path (TCG
  is the only x86 accelerator there) and for `xp-fifa-match.sh tcg` — not
  something in the normal path on a KVM host, which is one more reason for
  the per-game placement decided below.

**Where the merge lives: next to the game, never system-wide** (decided
2026-09-05, after the confirmation). The tempting alternatives are both
wrong here:

- Replacing `system32\dinput.dll` fights Windows File Protection on XP SP3
  (SFC restores it from `dllcache`), and a forwarding shim cannot carry the
  same name as the DLL it forwards to in the same directory — the original
  would have to be renamed, which breaks SFC and any repair install.
- `AppInit_DLLs` loads the shim into every GUI process on the system,
  including explorer and every installer, to fix one game's match loop.

And the merge is not a neutral improvement: `GetAsyncKeyState` is
system-wide, so a `DISCL_FOREGROUND` device that has correctly gone quiet
(the game is not in front, or is not acquired) would start reporting keys
again, and an application that reads buffered data alongside `GetDeviceState`
would see the two disagree. That is a lie we are happy to tell FIFA's match
loop, having watched it, and not one to tell every process on the guest.
So the shim stays a per-game, side-by-side DLL — the era-correct mechanism,
reversible by deleting one file — and *deploying* it becomes the launcher's
job (M6): a per-game compat list in the machine bundle that stages shim DLLs
next to the EXE, the same shape `D3DPT\DDRAW.DLL` already needs.

Making it shippable (same day): the shim now has two modes. The default is
the fix and nothing else — silent, no `dinput_log.txt`, no sampler thread.
`D3DPT_DINPUT_LOG=1` in the environment restores the full diagnostic build
described above. The observation was the expensive half: the sampler polls
248 virtual keys every 5 ms and every log line is flushed, which is real
money under TCG and not something to leave running in a game folder for a
fix that is 20 lines of merge. `tools/xp-fifa2000.bat` turns the log on when
it finds an `E:\DILOG` marker, which `tools/xp-fifa-match.sh` stages because
it greps the log for its regression check.
- FIFA's own quirks met on the way: its front-end menus need a mouse
  button held ≈1 s (a 100 ms click is ignored; the QMP `click` in
  `qmpc.py` is too short, hold the button by hand in `input-send-event`);
  the intro video can be skipped with Esc; the kickoff starts by itself
  after ≈1 minute; F1–F4 cameras, Esc pause, F12 exit are the readme's
  in-match keys. `tools/xp-fifa-match.sh kvm|tcg <image>` does the whole
  thing headless (menus, side, kickoff, the tap test with screendumps,
  `dinput_log.txt` pulled from the image): the regression check for
  "keys in a real DX7 game" on the HAL.

### Max Payne on the HAL: XP's own d3d8.dll on a DX7 driver (2026-09-05)

The first DX8 title with **no wrapper DLL in the game folder**
(`tools/xp-maxpayne.bat` renames the M4 track's `D3D8.DLL` away; run by
`GAME_ISO=DINO-MAP.iso CPU=pentium3 SHOTS=30 SHOT_KEYS="2:ret,6:ret"
tools/xp-driver-test.sh ~/vms/winxp-m7g.qcow2 bat tools/xp-maxpayne.bat`).
XP's d3d8.dll treats a driver without a `D3DCAPS8` answer as a
"DirectX 7 driver": it does the vertex processing itself and feeds the
DX7 token set (`INDEXEDTRIANGLELIST2`, `TRIANGLEFAN_IMM`, the DX7 render
and stage states) through DrawPrimitives2 — the same HAL FIFA runs on.
Result (on the DX7 face of the driver — since the DX8 DDI below landed,
`ddflags=0x2000` gives d3d8.dll that face again): the launcher, the main
menu and the tutorial level (Max in the alley, textures, lightmaps,
decals, snow, HUD, matching the M4 device's frame of the same scene)
render at 800×600×16, ~290 frames/s under KVM `-cpu pentium3` with 18
DP2 calls and ~230 draws per frame, no unsupported token, no refused
record; screendumps in `build/xp-driver-test/mp-hal10/`. What it took,
and what it taught:

- **`TRIANGLEFAN_IMM` / `LINELIST_IMM`: the payload after the 4-byte
  header is DWORD-aligned, and so is the next token.** The first runs
  desynchronised (garbage tokens 86, 115, 255…, "truncated" errors, the
  runtime re-submitting from `dwErrorOffset`) exactly after every
  inline-vertex token that started at offset 2 mod 4 — which happens
  after an `INDEXEDTRIANGLELIST2` with an even primitive count (2 + 6n
  bytes), a sequence the DX7 runtime never produced for D3D7TEST or FIFA.
  The DX8 runtime's legacy path does it every frame. Aligning only the
  token's *end* made the stream parse but still read the edge flags and
  vertices two bytes early: the per-draw snapshots of the frame trace
  (below) showed one fan per frame with positions of 10¹¹ and colours
  that were the top halves of floats — the **black bands across the
  alley** (a garbage triangle clipped to the screen), which no state,
  texture or fog experiment had explained. The rule (the DDK's perm3
  sample does the same): after the `D3DHAL_DP2COMMAND`, round the
  offset up to 4, then the `D3DHAL_DP2TRIANGLEFAN_IMM` and the vertices;
  round up again for the next command. `tools/d3dpt-dp2-test.cpp` draws
  its fan as an inline token at such an offset with the runtime's
  padding (0xcc bytes, so a parser that reads them as data fails). The
  executor also logs the token history (`ddi: dp2: tokens before it
  (offset:op x count): …`) with every first failure of a kind, which is
  how the pattern showed.
- **DXVK's exceptions abort QEMU; validate before calling.** The garbage
  streams reached `LightEnable` with an index of ~2³² and DXVK grows its
  light array to the index: `std::bad_alloc`. It cannot be caught in the
  executor — DXVK carries its own statically linked unwinder, the system
  `__gxx_personality_v0` is handed its context and `abort()`s (two
  core dumps, `_Unwind_Resume` in `libdxvk_d3d9.so.0` under
  `LightEnable.cold`). The interpreter now drops light indices ≥ 1024 and
  transform ids outside VIEW / PROJECTION / TEXTURE0–7 / WORLD0–3 (DXVK
  indexes an array with them), and `d3dpt_exec_submit` still wraps each
  record in a `try` for the executor's own allocations. The host test
  sends both and expects the draw to go on; on the old library the test
  process aborts (exit 134), which is the reproduction.
- **A per-frame DP2 trace, armed by a file.** `D3DPT_DP2_TRACE=<path>`
  in QEMU's environment: `touch <path>` when the screendump shows the
  scene in question; the executor arms, and from the next frame boundary
  (a readback) to the one after it logs every token with its arguments —
  a snapshot of every render / stage state seen so far at the start
  (most are set once per scene), then render states (marked when
  dropped), stage states, each bound texture's size / format / levels /
  caps and the mean of its VRAM texels, every draw with its first three
  vertices (position, colours, both uv sets), clears, targets, viewports
  — and writes next to the flag file every bound texture's levels
  (`tex-<handle>-l<n>.ppm` + `-a.pgm` for alpha) and the render target
  after every draw (`draw-<n>.ppm`), then removes the file. A readback
  with no draw before it does not end the frame (since 2026-09-09:
  Crimson Skies reads its target back after every render-target switch,
  and five arms in a row caught that empty call and nothing else; the log
  says `readback of N with no draw, the frame goes on`). That last
  part is what found the garbage fan: a script that counts black pixels
  per snapshot names the draw, and its vertex lines in the log name the
  bug. `D3DPT_DDI_REREAD=1` re-reads every texture from VRAM at every
  bind (a stale host copy vs VRAM the guest never wrote);
  `D3DPT_DDI_NOFOG=1` forces FOGENABLE off. Both were used to rule
  things out here. The driver now logs every surface it registers
  (`d3dptdisp: surface <handle> caps … w h fmt at …`, `sysmem, skipped`
  / `no format, skipped`), so an unknown handle in the executor's log can
  be looked up: the `render target handle 3 unknown` line at start is
  the third buffer of the game's flip chain, set as a target once before
  its `CreateSurfaceEx` (harmless).
- The DX7 states the DX8 runtime sets on a legacy driver and the executor
  drops once: 4, 10, 30, 33, 40, 47 (TEXTUREPERSPECTIVE, LINEPATTERN,
  ZVISIBLE, STIPPLEDALPHA, EDGEANTIALIAS, ZBIAS — the last one matters
  for decals and has a d3d9 twin, DEPTHBIAS; the alley sets it to 0).
  Since 2026-09-05 (evening) ZBIAS is mapped: each of its 0..16 steps
  becomes DEPTHBIAS −1/65535, the scale DXVK's own d3d8 layer uses
  (`d8caps::ZBIAS_SCALE`); `d3dpt-dp2-test` draws a coplanar quad that
  loses the Z test without it and wins with ZBIAS 8.
  What the traced frame looks like, for the next title: 230 draws, sky as
  38 opaque fans of one 512×256 texture, world geometry as `ADD(lightmap,
  diffuse)` on stage 0 (uv set 1) then `MODULATE(material)` on stage 1,
  decals and sprites blended SRCALPHA / INVSRCALPHA with A8R8G8B8
  textures (alpha test on for some), fog on with table mode NONE and
  the factor in the specular alpha, all textures 16- or 32-bit RGB (no
  DXT, no palettes).

## M7c — the DirectX 8 DDI (2026-09-05)

The driver is a DirectX 8 driver to d3d8.dll now: `D3DCAPS8` with hardware
transform and lighting, the DX8 token stream, render-to-texture, state
sets. D3DGAME8 (the M4 track's DX8 reference scene) runs through XP's own
d3d8.dll on it with **hardware vertex processing** — cubes, lit ground,
the render-to-texture panel, the index-buffer grid, the particles — at
~575 fps under KVM, with no wrapper DLL near it
(`tools/xp-driver-test.sh <image> d3dgame8`). FIFA and D3D7TEST keep
working; Max Payne renders on it except its clipped fans (the
"clipper" bullet, open). What the DDI is made of:

- **`GetDriverInfo2`.** With `DDHALINFO_GETDRIVERINFO2` in the HAL info
  the runtime sends `GUID_DDStereoMode` queries whose data starts with a
  `DD_GETDRIVERINFO2DATA` header (`dwMagic` = `D3DGDI2_MAGIC`); the
  driver answers `DXVERSION` (the runtime says 0x802), `GETD3DCAPS8`
  (the 212-byte `D3DCAPS8`), `GETFORMATCOUNT` / `GETFORMAT` (the DX8
  format list: `DDPF_D3DFORMAT` entries with the `D3DFORMAT` in `dwFourCC`
  and the operations in the `dwRBitMask` slot — texture, display mode
  with 3D acceleration, offscreen / same-format render target, Z-stencil
  for the twelve formats the host mirrors) and refuses the rest (0x18 is
  asked too). A real stereo query, without the magic, is refused as
  before. **Two findings from `d3d8.dll`'s disassembly:** without the
  HAL-info flag the runtime never asks and stays on the DX7 path (which
  is why the first run reported hardware vertex processing and no
  render-target textures — the T&L claim in the DX7 caps was enough for
  that); and the runtime checks `dwActualSize` against the size *inside*
  the GDI2 header while leaving the outer `dwExpectedSize` at the
  previous query's 24 bytes — an answer clamped to the outer size makes
  it drop the driver altogether (`GetDeviceCaps` fails, `CreateDevice`
  answers `D3DERR_NOTAVAILABLE`). `pf_format` reads the D3DFORMAT-coded
  pixel formats the DX8 runtime creates surfaces with.
- **The caps.** `D3DCAPS8`: the DX7 caps in DX8 form plus
  `HWTRANSFORMANDLIGHT` (also claimed in the DX7 `D3DDEVICEDESC`, with
  the transform / lighting caps, the extended caps' lights, clip planes,
  blend matrices and vertex-processing caps: the executor maps
  SETTRANSFORM / MULTIPLYTRANSFORM / SETLIGHT / SETMATERIAL and the
  lighting states onto DXVK's fixed-function pipeline, so the runtime
  hands us untransformed vertices; `ddflags=0x1000` withdraws the claim),
  `PUREDEVICE`, sixteen streams since protocol v10 (one before: the
  last section), 16-bit indices, vertex / pixel shaders
  `D3DVS_VERSION(1,1)` / `D3DPS_VERSION(1,4)` since the shader section
  below (0.0 in the first cut), 4096² textures, 8
  stages, cube maps since protocol v11 and volume maps since v12 (the
  last sections), and **no `D3DPMISCCAPS_CLIPTLVERTS`**:
  with it the runtime stops clipping pre-transformed vertices and hands
  the driver polygons that cross the camera plane, which the host
  rasterizes as garbage (Max Payne transforms on the CPU even on a T&L
  device — every draw of its frame is an RHW format — and its alley
  walls came out as flat panels at wrong depths until the claim went;
  the DX7 runtime always clipped them). `ddflags=0x2000` keeps the DX7
  face (no GDI2 flag).
- **The tokens.** The runtime's DX8 draws name vertex and index buffers
  by surface handle — buffers in guest system memory the host cannot
  see. The driver keeps a table of every surface dxg reports (VRAM and
  system memory alike, with each level's address and pitch, buffers with
  their linear size) and walks the stream twice in `D3dDrawPrimitives2`:
  SETVERTEXSHADER (an FVF: shader handles have bit 0 set), SETSTREAMSOURCE
  / SETSTREAMSOURCEUM / SETINDICES become driver state, each DRAWPRIMITIVE
  (2) / DRAWINDEXEDPRIMITIVE(2) / CLIPPEDTRIANGLEFAN becomes a
  self-contained `D3DPT_DP2_DRAW8` token (`d3dpt_proto.h` v6) carrying
  the primitive, the FVF, the vertex range and the indices (relative to
  the runtime's MinIndex), TEXBLT is done in the driver (system-memory
  texture → VRAM, every level, then VRAM_DIRTY; the DX8 runtime loads
  managed textures that way instead of blitting), shader tokens, patches,
  volume / buffer blits and dirty rects are dropped by size, the DX7
  tokens pass through (the inline-vertex ones re-padded for their new
  offset). The first pass measures and does the blits, the second writes
  the record. **That state persists between calls:** the runtime sends
  SETVERTEXSHADER / SETSTREAMSOURCE / SETINDICES only on change, so the
  vertex format and the bindings live in the context (`D3DCTX`) — as
  handles, resolved from the table at every call, because a `Lock` with
  DISCARD gives a buffer new memory and dxg reports it with another
  `CreateSurfaceEx` (the log's `at 0x00000000` lines are the old memory
  going); the first D3DGAME8 run lost every `DrawPrimitiveUP` for that
  (they arrive in their own calls, "dx8 draws skipped … fvf 0"). A
  user-memory stream holds `dwVertexLength` vertices of the token's
  stride, not of `dwVertexSize`. The executor draws a
  DRAW8 with `DrawPrimitiveUP` / `DrawIndexedPrimitiveUP` after
  `SetFVF`, and for the DX8 tokens that do reach it knows the sizes and
  drops them with a note.
- **State sets** (`STATESET`): BEGIN / END record into a d3d9 state block
  (a predefined type is `CreateStateBlock`), EXECUTE applies it, CAPTURE
  refreshes it, DELETE releases it. **Render-to-texture:** a texture with
  3DDEVICE caps is a default-pool render-target texture whose level 0 is
  the target; uploads go through the staging path, readback as for any
  target. **MULTIPLYTRANSFORM** and the d3d8-only render states (153,
  164, 172, 173 dropped; the DX8 numbering of the rest is d3d9's).
- **Compressed textures need a `DdCreateSurface` that sizes them.** dxg
  sizes a video-memory surface from its pixel format's bit count before
  it takes it from the heap; a FOURCC format has no bit count, so the
  request was for zero bytes and its one `DDERR_OUTOFVIDEOMEMORY` site
  answered — `CreateTexture(DXT1)` in `D3DPOOL_DEFAULT` failed with
  `D3DERR_OUTOFVIDEOMEMORY`, while `MANAGED` / `SYSTEMMEM` succeeded (the
  runtime's own system-memory copy: a surface with *no* pixel format,
  the compressed bytes as a 128×16 or 256×16 "display-format" image) and
  the video-memory copy then failed silently at the first draw, so the
  runtime kept the previous texture bound (D3DGAME8's particles as its
  gradient). The driver had no `CreateSurface` callback at all. It has
  one now: for a `DDPF_FOURCC` DXT surface it sets `dwBlockSizeX` = the
  linear size, `dwBlockSizeY` = 1, `fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE`
  (dxg allocates that many bytes from the linear heap), `dwLinearSize`
  in the `lPitch` union (which is why dxg's "pitch" of a DXT surface is
  its linear size: the driver put it there) and `DDSD_LINEARSIZE` on the
  description; everything else returns `DDHAL_DRIVER_NOTHANDLED`
  untouched. Managed textures are filled by the runtime through Lock /
  Unlock (Unlock marks the VRAM dirty), not TEXBLT. Also: DirectDraw
  creates a FOURCC surface only when the code is in the driver's FOURCC
  list (`DrvGetDirectDrawInfo`'s `pdwFourCC`, two-call protocol) — the
  DX8 runtime never reads the codes (it passes a null pointer in all
  three `DdQueryDirectDrawObject` calls), the kernel does; and a
  FOURCC-style entry in the GDI2 format list makes `CreateTexture` fail
  outright — d3d8.dll matches formats against `dwFourCC` under
  `DDPF_D3DFORMAT` only. `DRIVER\DXTTEST.EXE` is the probe that found
  it: every format × pool through CheckDeviceFormat, CreateTexture,
  Lock, a textured quad read back (pure red / blue block texels are the
  pass), CreateImageSurface; every HRESULT in `dxttest.log`.
- **The runtime's clipped fans are stream-0 draws; the DP2 vertex buffer
  is a dummy under d3d8.dll.** With `CLIPTLVERTS` withdrawn the runtime
  clips pre-transformed triangles itself and emits them as
  `CLIPPEDTRIANGLEFAN` tokens (58: FirstVertexOffset, dwEdgeFlags,
  PrimitiveCount). Where the vertices are was settled in `d3d8.dll`'s
  disassembly (2026-09-05): the DX8 DDI layer keeps two internal
  "TL streams" (a 44-byte object each: an `IDirect3DVertexBuffer8` it
  creates itself in `D3DPOOL_DEFAULT` with `D3DUSAGE_DYNAMIC`, a stride,
  the bytes used, the base of the current batch), one for the software
  pipeline's output and one for the clipper. `DrawClippedPrim` locks the
  clip stream (`D3DLOCK_NOOVERWRITE`, or `DISCARD` when it wraps), copies
  the fan's vertices in at the *current* FVF's stride, and writes the
  stream's batch base into `FirstVertexOffset`; before that, when the
  clip stream is not the current stream 0, it emits **`SETSTREAMSOURCE`
  (49) for stream 0 with the clip buffer's handle and stride**
  (`SETSTREAMSOURCEUM` if the buffer had no driver handle) and flags the
  application's stream to be re-set on the next draw. So a fan's offset
  is a byte offset into stream 0 as bound at that moment — a vertex
  buffer the driver knows by handle, with its size as the bound. The
  DP2 call's own vertex buffer never carries anything on the DX8 path:
  the DDI layer's init fills `dwFlags` 0x9, `lpVertices` = a 10 × 32-byte
  dummy, `dwVertexLength` 10, `dwVertexSize` 32 once and only
  `DrawPrimitiveUP` swaps its user pointer in temporarily
  (`SETSTREAMSOURCEUM`). The driver first read the fans at `lpVertices
  + FirstVertexOffset` (as the DX7 runtime's fans are laid out) and got
  the dummy's neighbours: heap garbage, zeros mostly — Max Payne's
  nearest walls and ground black. Reading them from stream 0 fixed it;
  the user-memory buffer's bound is the declared `dwVertexLength ×
  dwVertexSize` again.
- Tests: `tools/d3dpt-dp2-test.cpp` sends DRAW8 tokens (an indexed quad
  with a MinIndex of 10, a fan), a recorded state set executed, captured
  and deleted, a DRAW8 with an index beyond its vertices (skipped) and
  one lying about its stride (refused);
  `tools/xp-driver-test.sh <image> d3dgame8` boots the guest-tools ISO,
  copies `D3DGAME8.EXE` out alone and diffs its frame against the native
  d3d9 oracle of `scripts/test.sh` (HUD masked).
- Not there: volume textures, N- and RT-patches (ZBIAS →
  DEPTHBIAS landed 2026-09-05 evening). DXT textures on this path were fixed
  on 2026-09-05 (the compressed-textures bullet above); vertex and pixel
  shaders 1.x the same night (the section below); palettized textures
  with v8; video-memory vertex / index buffers
  (`D3DDEVCAPS_HWVERTEXBUFFER`) with v9; more than one vertex stream with
  v10; cube textures with v11; volume textures with v12 (the last
  sections).

### Vertex and pixel shaders 1.x on the DX8 DDI (2026-09-05, protocol v7)

The caps say `D3DVS_VERSION(1,1)` / `D3DPS_VERSION(1,4)` now (96 vertex
constants, `MaxPixelShaderValue` 8; `ddflags=0x4000` withdraws both), so
d3d8.dll creates every shader through the driver: with hardware vertex
processing the runtime validates the declaration and the function against
the caps and emits `CREATEVERTEXSHADER` (handle, declaration bytes,
function bytes, then both), `SETVERTEXSHADER` with that handle (an FVF has
bit 0 clear, a handle bit 0 set), `SETVERTEXSHADERCONST` (register, count,
float4s), `DELETEVERTEXSHADER`, and the pixel-shader four
(`CREATEPIXELSHADER` is handle + function). The design keeps the driver
out of it:

- **Driver.** The seven shader tokens pass through the DP2 walk unchanged
  (they were dropped by size before). A `SETVERTEXSHADER` value is kept as
  the context's vertex format whatever it is, and a `D3DPT_DP2_DRAW8`
  under a shader carries the handle in its `fvf` field with the stream's
  stride: the driver copies `nverts × stride` bytes as before and only
  the host, which has the declaration, knows what a vertex is (the
  `fvf_stride > stride` check is skipped under a shader; the "1 shader"
  skip reason is gone).
- **Executor** (`d3dpt_exec_ddi.cpp`). Shaders live per context (the
  runtime's handles are per device), by handle. `CREATEVERTEXSHADER`
  converts the `D3DVSD_*` declaration to `D3DVERTEXELEMENT9`s (`STREAM`,
  `REG` with its register number naming the usage by the DX8 convention
  — 0 position, 3 normal, 5 diffuse, 7.. texcoords — and its type
  numbered as `D3DDECLTYPE_*`, `SKIP`, `CONST` runs remembered, tessellator
  and `EXT` tokens skipped) into a d3d9 vertex declaration, remembers the
  bytes it reads of a stream-0 vertex and the streams it touches, and,
  when there is a function, validates it and puts one `dcl_usage vN` per
  input register in front (d3d9 wants them; DX8 shaders have none) before
  `CreateVertexShader`. A declaration without a function is the fixed
  function on that layout (`SetVertexDeclaration` + `SetVertexShader(NULL)`
  — a `D3DVSD_REG(D3DVSDE_DIFFUSE, …)` before the position, which no FVF
  can express, works). `SETVERTEXSHADER` applies the declaration, the
  function and the `D3DVSD_CONST` runs (DX8 loads them when the shader is
  set); an FVF restores `SetVertexShader(NULL)` + `SetFVF`. The constants
  go to `Set*ShaderConstantF` with the register range checked (DXVK
  indexes arrays with them). A DRAW8 under a shader is skipped, with one
  log line, when the handle is unknown, the declaration reads a stream
  the draw did not carry, or it reads more of a stream than that stream's
  stride carries; otherwise it is the same `DrawPrimitiveUP` /
  `DrawIndexedPrimitiveUP` with the declaration bound (a declaration
  reading more than stream 0 since v10: the multi-stream section).
- **The bytecode is validated before DXVK sees it.** The d3d9 half hands
  guest bytecode straight to DXVK, and the hostile case of the host test
  showed why that is not enough here: on a stream with an unknown opcode
  DXVK's compiler logs `No layout known for opcode` and then *asserts*
  (`sm3_parser.h: getDst … m_operands[0u].getInfo().kind == eDstReg`) — an
  abort, not an exception, QEMU dies with it. `sm1_valid` walks the
  tokens (bit 31 = a parameter, else an instruction; `DEF` carries four
  raw floats, `COMMENT` its length, `PHASE` only in ps 1.4) against a
  table of the vs 1.x / ps 1.x opcodes with their operand counts, and
  checks every register against the stage's file sizes. Anything else is
  refused with a log line and the handle stays unknown (its draws are
  skipped). The DX8 runtime validates every shader itself before the
  token, so a real guest never gets there.
- Tests: `tools/d3dpt-dp2-test.cpp` — vs 1.1 through a declaration
  (`oD0 = v5 * c0`: red, then green by the constant alone), a
  declaration-only shader with the colour before the position, a
  `D3DVSD_CONST` in the declaration, ps 1.1 `r0 = c0` and off again,
  ps 1.1 `tex t0` / `mul r0, t0, v0` on XYZRHW vertices, then the
  hostile set (a function without END, one with unknown opcodes, one
  missing its operands, a register off the file, a constant as the
  destination, a declaration reading stream 1, one wider than the
  stride, an unknown handle, constants at c300 / c250, a
  `CREATEVERTEXSHADER` lying about its declaration size →
  `D3DERR_COMMAND_UNPARSED`), and the FVF path after all that.
  `DRIVER\SHTEST.EXE` (`guest-tools/src/d3dptvid/shtest.c`) is the same
  through XP's own d3d8.dll — hand-assembled shaders (no D3DX in mingw),
  every draw read back through `CopyRects` and compared; also the vertex +
  index buffer path (`DrawIndexedPrimitive` under a shader); `tools/xp-driver-test.sh
  <image> shtest` runs it and greps `shtest.log` for "0 failed" (9 cases
  pass on `winxp-m7g`, 2026-09-05). One thing the guest run taught: a
  declaration-only shader must list its registers in FVF order —
  d3d8.dll answers `D3DERR_INVALIDCALL` to the colour-before-position
  layout itself, so the driver never sees such a declaration (the host
  test keeps the case because the executor handles it anyway).


### Vertex and index buffers in video memory (2026-09-05 night, protocol v9)

Until v9 every DX8 vertex and index buffer lived in system memory:
`D3dCreateD3DBuffer` turned each request into `DDSCAPS_SYSTEMMEMORY`, the
runtime filled the buffers in user mode, and `D3dDrawPrimitives2` copied
every draw's vertex range and indices into its `D3DPT_DP2_DRAW8` token —
GTA Vice City's 400–600 draws a frame were 400–600 memcpys through the
command window, in the guest, under TCG. With `D3DDEVCAPS_HWVERTEXBUFFER`
/ `HWINDEXBUFFER` (DDI-only bits of `D3DCAPS8.DevCaps`, `d3dhal.h`) the
runtime asks for its `D3DPOOL_DEFAULT` buffers in video memory instead
and keeps a MANAGED buffer's video-memory copy in step by `BUFFERBLT`
tokens, and a draw can *name* the buffer:

- **Allocation.** The buffer callbacks see `DDSCAPS_EXECUTEBUFFER |
  DDSCAPS_VIDEOMEMORY` with `DDSCAPS2_VERTEXBUFFER` / `INDEXBUFFER` in
  `ddsCapsEx` and `dwLinearSize`; as for a compressed texture the driver
  hands dxg the block size (`dwBlockSizeX` = the bytes, `dwBlockSizeY` =
  1, `fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE`) and dxg takes it from the
  linear heap; the `CreateSurfaceEx` that follows registers it on the
  host as a `D3DPT_VS_BUFFER` surface (width = pitch = its bytes, height
  1, no format, no host object — the host reads it from VRAM). Command
  buffers, and everything under `ddflags=0x100000` (`DDF_NO_HWVB`, the
  A/B), stay in system memory as before.
- **Filling.** `D3dLockD3DBuffer` remembers the range the runtime locks
  (`rArea.left..right` when `bHasRect`, else the whole buffer) and
  `D3dUnlockD3DBuffer` reports it with `VRAM_DIRTY_RANGE` (v9); the
  driver does the `BUFFERBLT` copy itself in pass 1 (system-memory copy →
  VRAM copy, `dwOffset`, `D3DRANGE`) and reports the range the same way.
  The token is 24 bytes: `dwDDDestSurface`, `dwDDSrcSurface`, `dwOffset`,
  the `D3DRANGE` (offset, size) and one more dword — a first cut sized it
  at the 20 the field list suggests and the stream desynchronised right
  after the first blit (a "token 0" whose header was the blit's last
  dword; the `bufferblt … sixth` log line shows that dword). `DISCARD` /
  `NOOVERWRITE` need nothing: every draw before the Lock has already run
  (the DP2 record executes inside the doorbell write), so there is no
  pipeline to rename around. The host keeps no copy yet, so the range
  records are informational (counted in the 5 s line as `N buffer writes
  of M KiB`); they are what a host-side mirror of the buffers would
  consume if the per-draw host copy ever shows up in a profile.
- **Drawing.** `walk_draw` writes `D3DPT_DRAW8_VRAM_VB` / `VRAM_IB` in the
  token's flags (the old `pad`) and one `{handle, byte offset}` pair in
  place of the vertex bytes and / or the index bytes; the host resolves
  the handle to a `D3DPT_VS_BUFFER` surface, checks the range against
  the buffer's size and draws with `DrawPrimitiveUP` /
  `DrawIndexedPrimitiveUP` straight from the VRAM pointer, exactly as
  from an inline copy. Inline and VRAM sources mix freely in one draw
  (the runtime's own clip stream is a `D3DPOOL_DEFAULT` buffer, so the
  clipped fans now come from VRAM too; a user-memory `DrawPrimitiveUP`
  still travels inline). A range beyond the buffer, an unknown handle or
  a texture named as a buffer skip the draw with one log line; an
  unknown flag refuses the record.
- **Tests.** `tools/d3dpt-dp2-test.cpp`: the quad from a VRAM vertex
  buffer and a VRAM index buffer at offsets, the two mixed with inline
  data, the buffer rewritten by the guest (`VRAM_DIRTY_RANGE`) and the
  draw moving with it, the three skipped cases, the refused flag, a
  buffer beyond VRAM and a buffer with texture caps refused. In the
  guest, D3DGAME8 covers MANAGED vertex and index buffers (`BUFFERBLT`)
  and a DYNAMIC `D3DPOOL_DEFAULT` one (Lock with DISCARD / NOOVERWRITE);
  `xp-driver-test.sh d3dgame8` diffs its frame against the native oracle
  as before, SHTEST the index-buffer path under a shader.
- **The numbers (GTA Vice City, `tools/xp-vicecity.sh play`, the city at
  the Ocean View start, ~480 draws and 3 DrawPrimitives2 calls a frame,
  800×600×32, the game's own frame limiter off and the flip chain's
  vertical blank off with `ddflags=0x8000` so the rate is the pipeline's;
  the A/B adds `0x100000`).** Under KVM (`-cpu pentium3`, this host):
  **360–375 frames/s** with the buffers in VRAM against **265–285** with
  every draw's vertices copied through the window — a third more, on the
  path where the guest CPU is as fast as it gets. The buffer writes the
  guest makes are ~1400–1900 ranges of 330–430 MiB per 5 s at that rate
  (RenderWare rewrites its dynamic buffers every frame) — the host reads
  them from VRAM at each draw, which is why no mirror was built. Under
  TCG (`-cpu pentium3`, the Apple Silicon shape, this x86 host): **62–75
  frames/s** against **51–55** — a fifth to a third more, the guest's
  memcpys being the cost the change removes. (The 20–30 fps the user saw
  by hand had the game's 30 fps limiter and the 60 Hz vertical blank on.)
  `tools/xp-vicecity.sh play` gets there on its own under both
  accelerators, reading the log's rate lines (draws per frame: the menu
  is 10, the game hundreds) rather than the clock for every wait.

### More than one vertex stream (2026-09-11, protocol v10)

Until v10 the caps said `MaxStreams` 1 and the driver tracked stream 0
alone: a DX8 declaration splitting a vertex across buffers — the position
and normal in one, texture coordinates or skinning weights in another, the
common shape for a title that shares geometry between passes — was
refused by the runtime before any draw, and a declaration that got through
reading stream 1 had its draws skipped on the host. `MaxStreams` is 16 now
(what the era's T&L parts claim; `ddflags=0x200000`, `DDF_ONE_STREAM`, is
1 again and the A/B).

- **Driver** (`core_dp2.c`). The context keeps all sixteen bindings
  (`st_handle` / `st_stride`, a user-memory bit each) instead of stream
  0's, resolved from the surface table at every call as before; every
  `SETSTREAMSOURCE` / `SETSTREAMSOURCEUM` lands in its slot. A draw under
  a vertex shader handle carries, after stream 0 and the indices, **every
  other stream bound whose range is there** — the driver does not parse
  declarations, the host has them, and a stale binding a stream-0 shader
  never reads costs an 8-byte reference when it is a VRAM buffer (with
  `HWVERTEXBUFFER` they nearly all are) and a copy when it is not. A DX8
  draw indexes all its streams with one vertex number, so stream *n*'s
  range starts at stream 0's first vertex — the token's byte offset over
  stream 0's stride (`DRAWPRIMITIVE2` / `DRAWINDEXEDPRIMITIVE2` carry
  offsets in stream 0's bytes; the two plain draws a vertex index) — at
  stream *n*'s own stride, `nverts` vertices long. A stream whose range
  runs past its buffer is left out rather than skipping the draw, and an
  FVF draw (the fixed function without a declaration, the runtime's
  clipped fans) carries stream 0 alone, as an FVF reads nothing else.
- **Record.** `D3DPT_DRAW8_STREAMS` in the DRAW8's flags; after the
  indices a `{count, 0}` pair and `count` streams, each a
  `d3dpt_dp2_draw8_stream` (number 1..15 in increasing order, stride, its
  own `D3DPT_DRAW8_VRAM_VB`) followed by its bytes or its `{handle,
  offset}` exactly as stream 0's (`d3dpt_proto.h`).
- **Executor.** Every stream's list entry is parsed before anything can
  skip the draw (the next token starts after the last one). The
  declaration's conversion now records the bytes it reads of *each*
  stream; a draw is skipped with one log line when a stream the
  declaration reads did not come with it, or a stream's stride is shorter
  than what is read of it. A declaration reading anything but stream 0
  alone is drawn **interleaved**: the streams it reads are copied into one
  vertex, in stream order, and a copy of the d3d9 declaration with each
  element moved into stream 0 at its stream's base (the strides before it
  added up) is bound after the shader — kept per shader per set of strides,
  eight at most. So the multi-stream draw takes the same
  `DrawPrimitiveUP` / `DrawIndexedPrimitiveUP` path as every other DRAW8,
  and the host holds no vertex buffers of its own. `apply_vs` is left
  alone: its early return is what keeps a `D3DVSD_CONST` run from
  clobbering constants the application set after the shader, and the
  interleaved declaration is simply set again at every such draw.
- **Tests.** `tools/d3dpt-dp2-test.cpp`: position and colour in two
  streams at different strides inline, the colour stream in a VRAM buffer
  under an indexed draw with a MinIndex, streams 0 and 2 with nothing at
  1, the fixed function on a two-stream declaration, a stream the
  declaration does not read carried and ignored; skipped (and the stream
  still in sync, a good draw after them landing): a stream the
  declaration reads missing, a stride shorter than the element, a VRAM
  range past the buffer; refused with `COMMAND_UNPARSED`: stream 0 in the
  list, streams out of order, a count of 16, an unknown stream flag, a
  count running past the record. In the guest, SHTEST's two-stream cases
  go through XP's own d3d8.dll: `DrawPrimitive` from a StartVertex,
  `DrawIndexedPrimitive` with a BaseVertexIndex, the colour stream in a
  `D3DPOOL_SYSTEMMEM` buffer (the copy path) and the fixed function on a
  two-stream declaration, every buffer's first vertices a decoy so a
  stream read from the wrong vertex shows as the wrong colour; stream 1
  then stays bound through the rest of the run, which reads stream 0
  only. **13 cases, 0 failed** (`xp-driver-test.sh shtest`, a qcow2
  overlay of `winxp-m7` under TCG on the Air, 2026-09-11; the runtime
  reports 16 streams, no skipped draw in the QEMU log).

### Cube textures (2026-09-11, protocol v11)

`D3DPTEXTURECAPS_CUBEMAP | MIPCUBEMAP` in `D3DCAPS8.TextureCaps`,
`CubeTextureFilterCaps` = the 2D filter caps, and
`D3DFORMAT_OP_CUBETEXTURE` on every RGB and DXT texture format of the DX8
list (not P8, whose palettes the host keeps per 2D texture); a format that
is a render target is a render-target cube too, because the runtime
checks the two ops independently. `ddflags=0x400000` (`DDF_NO_CUBE`)
withdraws all of it. **The DX8 face only:** a DirectX 7 cube map is
created by user-mode DirectDraw against the driver's own surface caps
(`GUID_DDMoreSurfaceCaps`), which this driver does not answer, so the DX7
`dwTextureCaps` do not claim one.

- **What the runtime builds.** A cube is six DirectDraw surfaces: the root
  is +X's level 0 (`DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_POSITIVEX` in
  `ddsCapsEx`), the other five faces (their own face bit each) hang off
  its attach list, and every face carries its own mip chain. Two things
  in the old walk were wrong for that shape: `d3dpt_os_attached` leaves
  out anything with `DDSCAPS_MIPMAP`, which a mip-mapped cube's faces
  have, and returns a one-level cube's faces as if they were a flip
  chain's other members, which `d3d_register_chain` would have
  registered as five unrelated 2D textures.
- **Driver** (`core_surf.c`). A new hook in both layers,
  `d3dpt_os_attached_all`, lists every attachment; `cube_faces` finds the
  six from the root (the attached surfaces of the root's size, by face
  bit, following the faces' own lists too), `d3d_register_chain` skips
  faces, and a face reached on its own is left to its root. The root's
  registration (`d3d_register_cube`) puts every face into the table under
  the face's own handle — a face-sized TEXBLT or a Lock can name one —
  with the root carrying `SURF_CUBE` (every face's levels and handles,
  allocated for cubes only), and sends the host one `VRAM_SURFACE` with
  `D3DPT_VS_CUBE`: face 0's level 0 in the record, then `6 × levels − 1`
  `{offset, pitch}` pairs face-major; then a `VRAM_CUBE_FACE` {face
  handle, cube, face} for each face with a handle. A TEXBLT whose
  destination is a cube root (the runtime's `UpdateTexture` of a
  default-pool cube from a system-memory one) copies the rectangle on
  every face, every level (`blt_levels`, factored out of the 2D path); a
  cube and a 2D texture in one TEXBLT are refused.
- **Executor.** A plain cube is a managed `IDirect3DCubeTexture9`
  uploaded face by face from the level table; a render-target cube is a
  default-pool one of one level whose face 0 is the cube entry's own
  target, and every other face's entry takes its surface of the cube as
  its `rt` — so `SETRENDERTARGET` by a face's handle, the clear and the
  draws into it, the untracked-write shadow and `READBACK` into the face's
  own VRAM are the 2D render-target path unchanged. `VRAM_DIRTY` of a face
  means the cube: a plain cube is read again whole, a render-target cube
  uploads the faces whose entries are dirty (`faces_dirty`) so a face the
  host drew into and nobody wrote is never overwritten. Releasing or
  re-registering a cube releases its faces' surfaces with it.
- **Found on the way, fixed the same day: the filter stage states'
  numbering.** d3d8.dll hands a DirectX 8 driver its *own* `D3DTEXF_*`
  values (NONE 0, POINT 1, LINEAR 2, ANISOTROPIC 3, FLATCUBIC 4,
  GAUSSIANCUBIC 5) — measured with `D3DPT_DP2_TRACE`: D3DGAME8's
  `MIPFILTER` LINEAR arrives as 2, and stages it never touched read 0 —
  where the DX7 runtime sent `D3DTFG_*` for `MAGFILTER` (ANISOTROPIC 5,
  the cubics 3 and 4) and `D3DTFP_*` for `MIPFILTER` (NONE 1, POINT 2,
  LINEAR 3); `MINFILTER`'s `D3DTFN_*` agree with DX8's. The executor
  reads the DX7 numbering, so every DX8 trilinear filter had been drawn
  point-mipped and every point-mipped one unmipped. The driver tells the
  runtimes apart by `ContextCreate`'s `dwhContext` *on input*, the
  runtime's interface version — **4** for d3d8.dll (D3DGAME8), **3** for
  DirectX 7 (D3D7TEST), **0** for the DirectX 3 execute-buffer path
  (EBTEST), printed as `iface` on the driver's `d3d context` line — and
  rewrites a version-4 context's `MAGFILTER` / `MIPFILTER` into the DX7
  numbering as it copies the tokens (`tss_dx8_filter`, `core_dp2.c`), so
  the host keeps one numbering and the protocol did not move. D3DGAME8
  against the native oracle: **11163 pixels beyond the tolerance of 8
  (max 44) before, 618 (max 11) after** — within the harness's budget of
  1200 for the first time (the rest inside x 166..463, y 150..292);
  D3D7TEST's frame still equals the host test's, EBTEST 5/5 (XP under
  TCG on the Air). The core is shared with the 9x layer, whose d3d8.dll
  interface number has not been read yet. CUBETEST sets `MIPFILTER`
  LINEAR, whose level-1 texels are one colour, so it passed under either
  reading.
- **Tests.** `tools/d3dpt-dp2-test.cpp`: a two-level A8R8G8B8 cube in VRAM
  with a quad at each face's direction (XYZRHW + `TEXCOORDSIZE3`) and +Z
  minified onto 4 × 4 pixels (level 1), a face rewritten and marked dirty
  under its own handle, a render-target cube cleared through two faces'
  handles and sampled, the face read back into its own VRAM; refused: a
  cube that is not square, one short of its face levels, one whose faces
  run past VRAM, face 6, a face of an unknown cube, a face of a 2D
  texture. `DRIVER\CUBETEST.EXE` (`xp-driver-test.sh <image> cubetest`)
  does the same through XP's own d3d8.dll: a MANAGED two-level cube, a
  face rewritten by Lock, a DEFAULT cube filled by `UpdateTexture` from a
  SYSTEMMEM one (the TEXBLT), a DXT1 cube, a render-target cube whose six
  faces are cleared through `GetCubeMapSurface` + `SetRenderTarget`,
  fixed-function `TCI_CAMERASPACENORMAL` generation and a vs 1.1 writing
  the direction to `oT0`; the caps and `CheckDeviceFormat` answers are the
  log's first lines. **9 cases, 0 failed** in the guest (a qcow2 overlay of
  `winxp-m7` under TCG on the Air, 2026-09-11), the ninth an
  `UpdateTexture` into a cube the host had already uploaded — the one case
  that fails if a write reaches VRAM without the host hearing of it.
- **How the runtime actually fills a cube.** The system-memory copies of
  a managed cube and the source of an `UpdateTexture` do reach
  `CreateSurfaceEx`, but some with no pixel format and a shape of their
  own (32 × 4 for a 16-texel cube), and none with its faces attached where
  the root's are; the driver logs them `not mirrored … (system memory)` and
  leaves them out. No `TEXBLT` was seen for a cube at all: the default-pool
  cube's faces are written through Lock / Unlock under each face's own
  handle, which the host's face-to-cube forwarding turns into a re-read
  (the executor's `cube face N written by the guest` line). The cube
  `TEXBLT` path (`blt_levels` on every face) is therefore unexercised by
  the runtime so far; it is kept for a runtime or title that sends one.

### Volume textures (2026-09-11, protocol v12)

`D3DPTEXTURECAPS_VOLUMEMAP | MIPVOLUMEMAP` in `D3DCAPS8.TextureCaps`,
`VolumeTextureFilterCaps` and `VolumeTextureAddressCaps` = the 2D ones,
`MaxVolumeExtent` 256, and `D3DFORMAT_OP_VOLUMETEXTURE` on the six RGB
formats of the DX8 list and, since the same evening, on DXT1 / DXT3 / DXT5 — not
P8. The DXTs waited on the sizing below: `DdCreateSurface` took a row from
`dwRGBBitCount`, which a FOURCC surface does not carry, so a compressed
volume asked dxg for zero bytes. It now sizes the box in the surface's own
rows (`pf_format`, then `fmt_row_bytes` and `surf_rows`: block rows for
DXT), on 9x as on NT, and the slice pitch is a block row times the block
rows; the host already read DXT volumes in blocks. `ddflags=0x1000000` (`DDF_NO_VOLUME`)
withdraws all of it. The DX8 face only, like the cubes.

- **What the runtime builds** (measured with VOLTEST and a log in
  `DdCreateSurface` / `DdLock`). A volume is one DirectDraw surface per
  level, `DDSCAPS2_VOLUME` in `ddsCapsEx.dwCaps2` and its depth in
  `dwCaps4`'s low word (4 for level 0 of a 16 × 16 × 4, 2 for level 1,
  whose `caps2` carries the mip-sublevel bit too), each level through its
  own `DdCreateSurface` call. The runtime fills a video-memory volume by
  locking **the whole level** — no `DDLOCK_HASVOLUMETEXTUREBOXRECT`, the
  level's full rectangle — and writing slice n at n × its slice pitch:
  the managed volume's upload, a `LockBox` rewrite and `UpdateTexture`
  into a default-pool volume all went that way. **No `VOLUMEBLT` was
  seen**, as no cube `TEXBLT` was.
- **The slice pitch trap.** dxg sizes a video-memory surface from its bit
  count — one slice — so the driver asks for the box itself
  (`DDHAL_PLEASEALLOC_BLOCKSIZE`), and `DD_SURFACE_GLOBAL.dwBlockSizeY` is
  a union with `lSlicePitch`. Asked for as one block of `size × 1` (the
  DXT recipe), the slice pitch came out 1: the runtime wrote the four
  slices a byte apart over slice 0 and left the rest of the box black.
  Setting `lSlicePitch` afterwards in `CreateSurfaceEx` reached the
  kernel's copy (the lock log said 0x400) and not user mode's, which is
  taken when `DdCreateSurface` returns. What works is the block asked for
  as **`depth` × `pitch × height`**: the same bytes, and its height is the
  slice pitch the runtime then uses (0x400 and 0x100 in the lock log; the
  managed volume's two levels 0x1000 apart in the heap). 9x's header
  names only `dwBlockSizeY` in that union; the same assignment there.
- **Driver** (`core_surf.c`, `core_dp2.c`). A volume's registration sends
  `VRAM_SURFACE` with `D3DPT_VS_VOLUME`: level 0's first slice in the
  record, the levels' `{offset, pitch}` pairs as a 2D texture's, then one
  `{depth, slice pitch}` pair; the table entry keeps the depth. A
  `VOLUMEBLT` (DP2 token 63) is done in the guest like a `TEXBLT` — the box
  level by level into the VRAM volume, then `VRAM_DIRTY` — for the runtime
  or title that sends one; unexercised so far.
- **Executor.** A managed `IDirect3DVolumeTexture9`, uploaded level by
  level and slice by slice through `LockBox` (level 0's slices at the
  record's slice pitch, another level's one after the other at its pitch ×
  rows); `VRAM_DIRTY` re-reads it whole. Refused: depth 0 or over 256
  (`D3DPT_VOLUME_MAX_DEPTH`), a slice pitch shorter than a slice, slices or
  a level past VRAM, a volume that is also a cube, a render target, a
  primary or a Z buffer, and a record without its `{depth, slice pitch}`.
- **Tests.** `tools/d3dpt-dp2-test.cpp`: a two-level 16 × 16 × 4 A8R8G8B8
  volume in VRAM, a quad at each slice's `w`, a minified quad on level 1,
  a slice rewritten and marked dirty, and the seven hostile records.
  `DRIVER\VOLTEST.EXE` (`xp-driver-test.sh <image> probe VOLTEST`): **4
  cases, 0 failed** through XP's own d3d8.dll — a managed two-level volume
  slice by slice and minified, a slice rewritten by `LockBox`, a default
  volume filled by `UpdateTexture` (TCG on the Air, 2026-09-11); **5 / 5**
  since the DXTs were offered: a 16 × 16 × 4 DXT1 volume, every slice its
  own colour, `create volume … -> 0x200` in the QEMU log (4 block rows ×
  32 bytes × 4 slices). DXT3 and DXT5 share the arithmetic and are not
  exercised.

### Anisotropic filtering (2026-09-11)

Caps only: the executor already mapped the anisotropic filters
(`MINFILTER` 3, the DX7 numbering's `MAGFILTER` 5 — which a d3d8.dll
context's 3 is rewritten to, "Found on the way" in the cube section) and
passed `MAXANISOTROPY` to DXVK. Both faces now claim
`D3DPTFILTERCAPS_MINFANISOTROPIC | MAGFANISOTROPIC` (the cube and volume
filter caps copy them), `D3DPRASTERCAPS_ANISOTROPY` and a
`MaxAnisotropy` / `dwMaxAnisotropy` of 16; `ddflags=0x2000000`
(`DDF_NO_ANISO`) withdraws it. ANISTEST's receding floor of stripes
through XP's own d3d8.dll: far-row contrast **0 under trilinear, 252
under anisotropic ×16** (TCG on the Air); D3D7TEST and the other probes
unchanged.

### The rest of DX8's texture formats (2026-09-11)

L8, A8L8, A4L4, A8, X4R4G4B4, R3G3B2, A8R3G3B2, DXT2 and DXT4 are in the
DX8 format list as 2D textures (`D3DFORMAT_OP_TEXTURE`, no cube or volume
op yet); `ddflags=0x4000000` (`DDF_NO_MORE_FMTS`) takes all nine out. The
list outgrew its 16 slots (32 now). What each half does:

- **The driver.** `pf_format` knows the pixel formats d3d8.dll describes
  them with: `DDPF_LUMINANCE` (8-bit mask 0xff = L8; 16-bit with alpha
  0xff00 = A8L8; 8-bit mask 0x0f with alpha 0xf0 = A4L4), `DDPF_ALPHA`
  alone at 8 bits (A8), and RGB masks 0xe0/0x1c/0x03 (R3G3B2) or 0x00e0
  with alpha 0xff00 (A8R3G3B2). The fields are read through the RGB names
  of their union slots, because the 9x DDK's `DDPIXELFORMAT` has no
  luminance names. DXT2 and DXT4 are in both families' FOURCC lists (NT's
  `DrvGetDirectDrawInfo`, 9x's `fourcc[]` in the HAL block, six slots now),
  because DirectDraw checks that list before it asks about a FOURCC
  surface at all. `fmt_row_bytes` sizes all of them. It **also sizes
  V8U8 now, which it never did**: a V8U8 `TEXBLT` copied rows of zero
  bytes, so a managed bump map never reached VRAM. BUMPTEST passed
  anyway and still does, so its bump maps must reach VRAM another way;
  a managed one's upload is what this fixes.
- **The host.** DXVK takes L8, A8L8, A4L4, A8, DXT2 and DXT4 as they are
  (DXT2 / DXT4 as BC2 / BC3; the premultiplied alpha is the application's
  business). R3G3B2 and A8R3G3B2 have no Vulkan format, and A4L4's is
  optional. So when the device is created, the executor asks
  `CheckDeviceFormat` about each of those formats once. Any it refuses is
  expanded to A8R8G8B8 at upload, the way P8 is (`host_lacks`,
  `texel_argb`), and the log says which:
  `ddi: no host texture format 52 27 29: expanded to A8R8G8B8 at upload`
  on the Air (KosmicKrisp has no R4G4). **X4R4G4B4 is always expanded.**
  DXVK creates it as `VK_FORMAT_A4R4G4B4_UNORM_PACK16` with no swizzle,
  so the X nibble samples as alpha, and a texture written with it 0
  draws transparent. Both tests below found that on their first run.
  It applies to the DX7 face's X4R4G4B4 surfaces too, which `pf_format`
  already mapped.

Evidence: `d3dpt-dp2-test` draws each of the seven non-DXT formats with
its colour and with its alpha replicated, on whichever path this host
takes (all seven right on the Air). FMTTEST through XP's own d3d8.dll
passes **9/9**, including DXT2 and DXT4. The DX7 texture list is
unchanged: no DX7 title is known to want these, and nothing tests them
there.

### Multisampling (2026-09-11, protocol v13)

Full-screen antialiasing on the DX8 face. The format list gives the
render-target formats (X8R8G8B8, A8R8G8B8, R5G6B5, X1R5G5B5) and the depth
formats (D16, D24X8, D24S8) 2 and 4 samples in `MultiSampleCaps`, which
shares `DDPIXELFORMAT`'s green-mask slot: `wFlipMSTypes` in the low word,
`wBltMSTypes` in the high, bit n − 1 for n samples. MSAATEST reads that
back as exactly 2 and 4. `ddflags=0x8000000` (`DDF_NO_MSAA`) takes them
out. The pieces:

- **The driver** reads a surface's sample count off `ddsCapsEx.dwCaps3`
  (`DDSCAPS3_MULTISAMPLE_MASK`, both layers) and puts it in the
  `VRAM_SURFACE` caps (`D3DPT_VS_SAMPLES`, bits 8..12: the protocol v13
  bump) for a render target, a depth buffer or a flip-chain member. VRAM
  stays the size of the resolved image: it only ever holds that.
- **The host** creates such a target and its depth buffer with that many
  samples when this host's DXVK has the count for the format
  (`CheckDeviceMultiSampleType`, else plainly, logged once). Every
  readback, a flip's included, first resolves it into a plain target
  (`StretchRect`, `resolved()`), since d3d9 reads no multisampled surface
  back. Nothing is uploaded into one, because Direct3D 8 locks no
  multisampled surface, so the guest cannot have written it.
  `D3DRS_MULTISAMPLEANTIALIAS` (161) and `MULTISAMPLEMASK` (162) were
  already forwarded.
- **Full screen only.** With the blt types claimed too, d3d8.dll created a
  windowed multisampled device, and its `Present` drew nothing: no
  `DdBlt`, no lock, no readback of the back buffer. A windowed `Present`
  of a multisampled back buffer is a driver blt, and this driver has no
  blitter ("Blit caps and the HEL": a declined blt on XP is an error, not
  a fallback). A flip needs nothing new. Windowed antialiasing would take
  a blitter for exactly that case (resolve, copy or stretch, clip list).

Evidence: `d3dpt-dp2-test` draws a slanted edge into a 64 × 64 4-sample
target and D16 depth buffer: 22 blended pixels on 11 of 11 rows with the
state on, none with it off. MSAATEST through XP's own d3d8.dll: a
full-screen 640 × 480 × 16 device with 4 samples, the frame read back from
the front buffer after the flip. It gets **34 blended edge pixels on 17 of
17 rows** with `MULTISAMPLEANTIALIAS` on (row 110 reads `ff … ff 84 00` across
the edge), and none with it off. Both flip-chain buffers log
`ddi: render target N: 640x480 fmt 23, 4 samples`.

### The DX8 feature probes (2026-09-11)

One program per Direct3D 8 feature in `DRIVER\`, each through XP's own
d3d8.dll with every draw read back in the guest, over one shared header
(`guest-tools/src/d3dptvid/d3d8probe.h`: the windowed device, the log,
row readback, the case bookkeeping). A probe first logs the caps and
`CheckDeviceFormat` answers for its feature; **when the caps say the
driver has no such feature it ends with `(not offered: <why>)` and runs
nothing**, which is what the probe of a feature not built yet prints —
the same program is the feature's check the day the caps claim it.
`tools/xp-driver-test.sh <image> probe <NAME>` runs one, `probes` all
eight in one boot (a staged batch file), and the verdict is PASS, NOT
OFFERED or FAIL. Where they stood on 2026-09-11 (the overlay above):

| Probe | Feature | Verdict |
|---|---|---|
| `CUBETEST` | cube textures (v11) | PASS, 9 cases |
| `STRMTEST` | more than one vertex stream (v10): three streams under a vs 1.1 from a StartVertex, indexed with a BaseVertexIndex and a MinIndex, a system-memory stream, the fixed function on three streams, streams 0 and 3 with a gap, stale streams under an FVF draw | PASS, 6 cases |
| `VOLTEST` | volume textures (incl. `UpdateTexture`, the DDI's `VOLUMEBLT`) | PASS, 5 cases (since v12, the same day; its DXT1 case since the DXTs were offered as volumes, the same evening) |
| `FMTTEST` | L8, A8L8, A4L4, A8, X4R4G4B4, R3G3B2, A8R3G3B2, DXT2, DXT4 (colour and replicated alpha each) | PASS, 9 cases (since the formats were listed, the same day; see the section above) |
| `BUMPTEST` | EMBM on every bump format offered (V8U8 and Q8W8V8U8 under `BUMPENVMAP`, L6V5U5 and X8L8V8U8 under `BUMPENVMAPLUMINANCE`) and DOT3 | PASS, 8 cases (EMBM since V8U8 was listed; the luminance formats since they were and DXVK's patch 07, the same day; Q8W8V8U8 not listed, so skipped) |
| `SPRTEST` | point sprites, and a per-vertex size (`D3DFVF_PSIZE`) | PASS, 4 cases (the per-vertex case since `D3DFVFCAPS_PSIZE` was claimed, the same day) |
| `ANISTEST` | anisotropic filtering | PASS, 1 case (since the caps claimed it, the same day: far-row contrast 0 trilinear, 252 anisotropic) |
| `PATCHTST` | RT- and N-patches | NOT OFFERED |
| `MSAATEST` | multisampling: the types d3d8.dll reports, then a full-screen 4-sample device's slanted edge from the front buffer, with `MULTISAMPLEANTIALIAS` on and off | PASS, 2 cases (since v13, the same day; windowed not offered) |

Two things the probes turned up beyond their verdicts: `TextureOpCaps`
claimed `BUMPENVMAP` / `BUMPENVMAPLUMINANCE` with no bump format to use
them on — **closed the same day for `BUMPENVMAP`**: V8U8 is in both
texture lists (the DX8 one as `D3DFORMAT_OP_TEXTURE | D3DFORMAT_OP_BUMPMAP`,
the DX7 one as a `DDPF_BUMPDUDV` pixel format for the DirectX 6 / 7 EMBM
titles), the driver maps a `DDPF_BUMPDUDV` surface to `D3DFMT_V8U8`, and
the host needs nothing new (the texels go up as they are, DXVK's fixed
function does the op; the bump matrix is an ordinary stage state).
BUMPTEST's EMBM cases pass through d3d8.dll; the DX7 list's entry has no
probe yet. `ddflags=0x800000` takes V8U8 out of both lists for an A/B.
`BUMPENVMAPLUMINANCE` was claimed with no luminance bump format until
the same day, when L6V5U5 and X8L8V8U8 went into the DX8 list
(`D3DFORMAT_OP_TEXTURE | BUMPMAP`, under `ddflags=0x800000` like V8U8).
The driver maps a `DDPF_BUMPDUDV | DDPF_BUMPLUMINANCE` pixel format to
them by its masks (the luminance mask is in the `dwBBitMask` slot) and
sizes them. The host needs only their sizes, because DXVK converts both
to float on the GPU at upload. BUMPTEST runs its EMBM case on each: a
luminance of one half under a scale of 1 comes out as the same colours at
half intensity. Two things came up on the way:

- **DXVK never applied the luminance** (`patches/dxvk/07`). In its
  fixed-function ubershader, `sampleTexture` read the previous stage's
  colour op into a variable declared a second time inside an `if`. The
  outer copy stayed 0, so the luminance branch was dead code, while the
  bump offset (read from the inner copy) still worked. And the branch
  took the luminance from the environment map's texel instead of the bump
  map's. BUMPTEST read full intensity where L = 1/2 was asked for.
  `d3dpt-dp2-test` now has the same case without a guest, and it fails
  on unpatched DXVK.
- **Q8W8V8U8 is not listed.** With it listed, d3d8.dll accepted
  `CreateTexture`, kept only the system-memory copy, and never asked the
  driver about a video-memory surface (no `CanCreateSurface`, no
  registration). So nothing was bound at stage 0 and the draw had no
  bump offset. Why the runtime does that is open; fixed-function EMBM
  titles use V8U8.

The DX7 list still carries V8U8 alone. And per-vertex point size was one `FVFCaps` bit away (the driver's
`fvf_stride` and the host already carried `D3DFVF_PSIZE`) — claimed the
same day, and SPRTEST's per-vertex case (a size of 24 in the vertex over a
`POINTSIZE` of 4) passes through d3d8.dll.
