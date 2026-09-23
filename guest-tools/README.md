# guest-tools

The guest-tools ISO holds everything a guest needs from us: the display
drivers, the Glide / OpenGL / Direct3D wrappers, the installer that puts
them in place, the disc-shelf program and the test programs. This file
covers how the disc is built, what is on it and what `SETUP.EXE` does.
The end-user text on the disc is `README-ISO.txt` (the root
`README.TXT`), `README-WINED3D.txt` and `README-DRIVER.txt`. Every test
program's use is in `docs/testing.md`, and the drivers' designs are in
docs 15 (XP) and 19 (9x). Era binaries are built at build time and never
committed (`out/` is git-ignored).

## Building

`scripts/build.sh` runs the `guest` stage. By hand it is
`guest-tools/build-wrappers.sh`, which builds everything below and rolls
`out/guest-tools-3dfx-<rev>.iso`. `<rev>` is the `third_party/qemu-3dfx`
commit the host QEMU is signed with. The wrappers are built from that
same commit and the host checks the stamp; a mismatch means no
acceleration.

- **Toolchain.** i686 mingw-w64, nasm, gendef, xxd, shasum and an ISO
  tool (xorriso, or genisoimage/mkisofs). Arch: `mingw-w64-gcc
  mingw-w64-tools nasm xorriso`; macOS: `brew install mingw-w64 nasm
  xorriso`. Homebrew's mingw-w64 has no `gendef`, so the script builds it
  from pinned mingw-w64 sources into `out/tools/bin`
  (`GENDEF_FORCE_BUILD=1` forces that path).
- **Open Watcom v2** for the two formats mingw cannot make: the Win98
  display driver (a 16-bit NE `.drv` and a ring-0 LE `.vxd`,
  `build-driver9x.sh`) and the DOS Glide overlay `GLIDE2X.OVL`. The build
  looks in `WATCOM=`, then `~/.local/opt/open-watcom` (the CI release's
  `ow-snapshot.tar.xz`, which carries every host's binaries). Without it the ISO is still
  built, minus those files, and the build says so.
- **The XP driver** is `build-driver.sh` (doc 15). Its failure stops the
  ISO build.
- **On Windows**, `scripts/build-windows.sh guest` builds the disc in
  MSYS2 through `msys2-i686.sh` (MSYS2's i686 toolchain as MINGW32,
  which qemu-3dfx's wrapper build requires).

Every binary on the disc passes two checks before it is packaged:

- **CRT.** Win9x has no UCRT, and modern mingw-w64 links it by default
  (`api-ms-win-crt-*.dll` imports, "required DLL not found" on Win98). A
  compiler shim forces classic `msvcrt.dll` (`-D__MSVCRT_VERSION__=0x700
  -mcrtdll=msvcrt-os`), and the script refuses anything that still
  imports the UCRT api-sets.
- **ISA.** Upstream builds the wrappers `-march=x86-64-v2` for `-cpu
  host`. Our reference guests are `pentium3`, so the shim appends
  `-march=pentium3` and the script rejects SSE2+ or POPCNT instructions
  ("invalid instruction in module opengl32.dll" otherwise).

Every text file on the disc is converted to CRLF, because Win9x Notepad
shows LF-only text as one line.

## The disc

```
SETUP.EXE   the installer (below; guest-tools/src/setup.c)
README.TXT  README-ISO.txt with the commit stamped in
GLIDE\      the device mapper and Glide: GLIDE.DLL GLIDE2X.DLL
            GLIDE3X.DLL, FXMEMMAP.VXD (9x), FXPTL.SYS + INSTDRV.EXE
            (2000/XP), GLIDE2X.OVL (DOS; needs Open Watcom)
DRIVER\     the 2000/XP display driver for d3dpt-vga (D3DPTVID.SYS,
            D3DPTDISP.DLL, D3DPTVID.INF, DRVINST.EXE) and its test
            programs; README.TXT
DRIVER9X\   the 98/Me display driver for d3dpt-vga: D3DPT9X.INF,
            D3DPT9X.DRV, D3DPT9V.VXD, the DirectDraw HAL D3DPT9HL.DLL;
            and the blue-screen test VxDs
D3DPT\      per game: D3D8.DLL D3D9.DLL DDRAW.DLL DINPUT.DLL over the
            paravirtual device (doc 14)
OPENGL\     per game: OPENGL32.DLL (the GL pass-through) and
            WRAPGL32.EXT, its extension-list cap
WINED3D\    per game, whole folder: D3D8-9\ for DirectX 8/9, DDRAW\ for
            DirectX 7 and older, each with WINED3D.DLL and OPENGL32.DLL;
            SYSTEM9X\ (DDRAWME.DLL, D3DPRE.EXE) for the 9x machine-wide
            install; README.TXT says which and when
VOODOO2\    V2START.EXE, the start-up guard for 3dfx's Voodoo 2 driver
CDSHELF\    CDSHELF.EXE (98/2000/XP), CDSHELF.COM (DOS)
TESTS\      every test, benchmark and calibration program
```

**One folder per role, one copy of every file.** Which stack a game gets
is decided by which folder it is copied from, so no name on the disc
means two things. `D3D9.DLL` is ours in `D3DPT\` and Wine's in
`WINED3D\D3D8-9\`, and the two never share a folder. The one deliberate
duplicate is WineD3D's. Each of its folders carries the DLLs under the
names a game loads plus `WINED3D.DLL` and our `OPENGL32.DLL`, so a user
copies one folder from Explorer and renames nothing (user request). Without our `OPENGL32.DLL` beside it, WineD3D draws through
Windows' software GL 1.1. The files are the same for 98 and XP.

**The mapper is not optional.** `OPENGL32.DLL` and the `D3DPT\` DLLs reach
the device through it and refuse to load without it (`0xc0000142` on NT).
9x has it as a VxD in `SYSTEM`; NT as a kernel driver that the `MAPMEM`
service points at, registered by `INSTDRV.EXE`.

**`WRAPGL32.EXT`** (`guest-tools/wrapgl32.ext`, `ExtensionsYear,1997`)
caps the extension list a game is shown, because a 1990s game reads the
host's several thousand characters into a fixed buffer (GLQuake's is
4096 bytes). It is the one file a user edits per title, so `SETUP /GAME
3` never overwrites one already next to a game. The rest of the
reasoning is in `docs/development.md`, "OpenGL pass-through".

## SETUP.EXE

A console program on purpose. It is the one interface Windows 98, XP and
a rescue command prompt all have, and every step is scriptable. It
installs from the folder it is in, so it works from the CD, a copy on
disk or a share.

```
SETUP                  a menu
SETUP /ALL             every component this Windows can use
SETUP /I <n> [<n>...]  those components
SETUP /LIST            the component and file-set lists
SETUP /GAME <n> <dir>  copy file set <n> next to a game's EXE
SETUP /REBOOT          with /ALL or /I: restart if a step asked for it
SETUP /LOG <file>      the log elsewhere (default C:\2KSBOX\SETUP.LOG)
```

**Components** (installed into Windows). Only the ones this family can
use are listed and numbered, in this order, and new ones are appended so
no existing `/I` number moves:

| `/I` 9x | `/I` NT | Component | What it does |
|---|---|---|---|
| 1 | 1 | Display adapter driver | NT: `DRVINST.EXE` on `DRIVER\D3DPTVID.INF`. 9x: the four `DRIVER9X\` files into `WINDOWS\INF` (and the three binaries into `SYSTEM`), where PnP installs them on the next boot (doc 19 §16). Restart required |
| 2 | 2 | Glide and the device mapper | `GLIDE*.DLL` into the system folder; 9x: `FXMEMMAP.VXD`, and `GLIDE2X.OVL` into `WINDOWS` for DOS-box games; NT: `FXPTL.SYS` and the `MAPMEM` service, checked running afterwards |
| 3 | 3 | Disc shelf tool | `CDSHELF.EXE` into `WINDOWS`, on both families' search path |
| 4 | 4 | Test programs | `TESTS\` into `C:\2KSBOX`; off in the menu, on with `/ALL` |
| 5 | | Sound Blaster 16 device names | only where a translation made an SB16 wave name too long for DirectX 9: a shorter one in the override `SB16.VXD` reads (doc 20 §5.3) |
| 6 | | Voodoo 2 start-up guard | only with a 3dfx card. 3dfx's `Voodoo2` Run entry moves to `HKLM\SOFTWARE\2ksbox\Voodoo2` and `V2START.EXE` takes its place (doc 21 §11) |
| 7 | | WineD3D as this machine's DirectDraw | wine9x's switcher as `DDRAWME.DLL`, the machine's own DirectDraw kept as `DDSYS.DLL`, the GL pass-through as the system `OPENGL32.DLL`, and `D3DPRE.EXE` in the Run key, which points `KnownDLLs\DDRAW` at WineD3D only on a host with no executor (doc 19 §43) |

**File sets** (`/GAME <n> <dir>`) are copied next to one game, never into
the system folder. Each set is self-contained, so two stacks never share
a folder.

| n | Set | From |
|---|---|---|
| 1 | Direct3D 8/9 on the paravirtual device (`D3D8.DLL D3D9.DLL DDRAW.DLL`) | `D3DPT\` |
| 2 | DirectInput keyboard fix (`DINPUT.DLL`) | `D3DPT\` |
| 3 | OpenGL pass-through (`OPENGL32.DLL WRAPGL32.EXT`) | `OPENGL\` |
| 4 | WineD3D, Direct3D 8/9 | `WINED3D\D3D8-9\` |
| 5 | WineD3D, DirectDraw and Direct3D up to 7 | `WINED3D\DDRAW\` |
| 6 | Glide pass-through (`GLIDE*.DLL`) | `GLIDE\` |
| 7 | DOS Glide pass-through (`GLIDE2X.OVL`) | `GLIDE\` |

`DDRAW.DLL` in set 1 forwards to Windows' own and reports 256 MB of video
memory, for launchers that ask DirectDraw rather than Direct3D (GTA Vice
City refuses 4 MB). `DINPUT.DLL` merges `GetAsyncKeyState` into a
non-exclusive keyboard's state, for a game whose loop stops pumping
messages (FIFA 2000's match, doc 15); `D3DPT_DINPUT_LOG=1` adds its log.
Both are per game by decision, never system-wide.

**A machine with a 3dfx card** (the emulated Voodoo 2, doc 21) gets its
Glide from 3dfx's driver under the same names. SETUP finds the card in
the live devnode tree (`HKEY_DYN_DATA` on 9x, `CM_Locate_DevNode` on NT),
not the registry's history. Component 2 then leaves `GLIDE*.DLL`, an
`FXMEMMAP.VXD` already there (3dfx's own binary, same IOCTLs) and
`GLIDE2X.OVL` alone. Before, whichever copy came last decided, without a
word, whether every Glide game drew on the card or the pass-through. Sets 6 and 7 put ours
next to one game.

### On Windows 98/Me

- **An installed driver file is never overwritten in place.** A module
  whose file is replaced under it runs the new build's bytes at the old
  build's addresses. KERNEL reloads a 16-bit `.DRV`'s discarded code
  segments from disk, and a ring-3 DLL is demand-paged from its file. The
  result is a fault in the display driver, which on 9x is a blue screen. The `.DRV`
  and VxD are held open and refuse the copy, but the HAL DLL is not held
  while an application has it loaded. So when a driver file already
  exists, all seven (four in `WINDOWS\INF`, three in `SYSTEM`) are staged
  beside their targets with the extension's last character made `_`
  (`D3DPT9X.DR_`, `D3DPT9X.IN_`, since the INF and `.DRV` share a base
  name)
  and listed in `WININIT.INI [rename]`, which WININIT applies before the
  GUI on the restart the step asks for anyway. A first install copies
  outright. (NT uses `MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)` for a
  locked file.)
- **The restart comes from a detached copy of SETUP.** `ExitWindowsEx`
  from a console process on Windows 98 never returns and starts no
  shutdown. A console program has no message queue of its own, and the
  stuck call holds the Win16Mutex, so a second thread pumping messages
  goes down with it. SETUP therefore re-runs itself as `SETUP /REBOOTNOW`
  with `DETACHED_PROCESS`, and that copy restarts the machine (~20 s).
  `rundll32 shell32.dll,SHExitWindowsEx 2` does nothing, and `rundll32
  krnl386.exe,exitkernel` forces an exit that leaves the FAT dirty. NT
  restarts from SETUP itself.

`tools/setup-guest-test.sh <image> [xp|win98]` guards all of it in a real
guest (`REBOOT=1` for the restart, `VOODOO=1` for the 3dfx card;
`docs/testing.md`).

## WineD3D (wine9x)

Direct3D 8/9 and DirectDraw → OpenGL → the pass-through, from
[JHRobotics/wine9x](https://github.com/JHRobotics/wine9x) (Wine 1.7.55
with 9x/XP fixes, LGPL), pinned by commit in `build-wrappers.sh`. It is
the guest-side fallback for a host with no Direct3D executor (ADR-013),
retired in M15's last step and not before (ADR-018). `wined3d.dll`
renders through the first `opengl32.dll` the loader finds, which is ours
in the game folder. XP needs OpenGL 2.1 with BGRA from the host. Wine's d3d8/d3d9
set the x87 to 24-bit precision on `CreateDevice`, as native Direct3D
does, which doc 13's PC=24 path covers.

The disc offers the per-game install (sets 4 and 5) and, on 9x, the
machine-wide DirectDraw (component 7), because on 9x a per-game folder
reaches only the session's first DirectDraw program. wine9x's other
system-wide switchers (`*_98` / `*_XP`, routing each EXE by
`HKLM\Software\DDSwitcher`) left the disc by user decision, as a third
way to install the same thing; they are still built in
`out/wine9x/`, whose README has their steps.

Build notes:

- wine9x links the CRT through the same shim.
- Its `pthread9x` sub-build hardcodes `ar`, which the script overrides
  with the mingw one (macOS has BSD `ar`).
- wine9x keeps its own `-march=pentium2` rather than the shim's
  `pentium3`, with which GCC emits a `memset` call into the CRT-less
  switcher DLLs. The ISA check still covers every file on the disc.

Our queue is `patches/wine9x/*.patch`, git-format diffs against the
pinned commit:

- `01-24bit-desktop-mode`. wined3d maps 24- and 32-bit desktops alike to
  `B8G8R8X8` and so asks for 32 bpp on a 24-bit desktop. A driver without
  32-bit modes (QEMU's Cirrus on XP at 800×600) refuses, and Wine 1.7.55
  crashes in its own error path (`glsl_fragment_pipe_free` on a NULL
  priv). Now a 24-bit desktop stays at 24 when the size matches, and a
  failed 32-bpp switch retries at 24 (found with FIFA 2000; worth sending
  upstream).
- `02-debug-log-flush` flushes Wine's log per line, so a crash keeps the
  tail (release builds compile logging out).

**A debug build** keeps logs that survive a crash. It is a second wine9x
checkout with the same patches, built with `SPEED= WINED3D_SILENT=` and the
overrides in `build_wined3d`. Its `wined3d.dll` / `winedd.dll` /
`ddraw_xp.dll` write `proc_<pid>_dwine.log` and `proc_<pid>_wined3d.log`
into the game folder (`set WINEDEBUG=+ddraw,+d3d` for traces). Read them
from a shut-down guest's qcow2 on the host (on macOS: `qemu-img convert
-O raw`, `hdiutil attach -readonly -nomount -imagekey
diskimage-class=CRawDiskImage`, `diskutil mount readOnly`); Dr Watson's
`drwtsn32.log` (UTF-16) names the faulting module.

## CDSHELF: the disc shelf from inside the machine

The launcher's shelf (doc 07) from a guest that may be mid-game.
`CDSHELF` lists it, `CDSHELF <n>` puts a disc in the drive, `CDSHELF E`
empties it. Both builds talk to the machine's *own CD-ROM drive*, which
answers a vendor ATAPI opcode with the shelf (patch 52,
`cdshelf/cdshelf_proto.h`). It is the one channel DOS, Win98 and XP can
all reach, so there is no new device and nothing to install.

- **`CDSHELF.EXE`** (`src/cdshelf.c`), one binary for both Windows
  families. XP/2000 use SPTI (`IOCTL_SCSI_PASS_THROUGH_DIRECT` on
  `\\.\<letter>:`); 98/Me use ASPI, with `WNASPI32.DLL` loaded at run time
  (it does not exist on XP; a stock 98 has it with `APIX.VXD`).
  `SendASPI32Command` is **`__cdecl`**, not stdcall, and its exports carry
  no `@n` to say so. Declared `WINAPI`, it links and leaves the stack four
  bytes out on the first call, and Windows kills the program a moment
  later. Every CD-ROM drive is asked and the one that answers is used;
  `-d E:` overrides on XP, `-v` shows each CDB. Log:
  `C:\2KSBOX\CDSHELF.LOG`.

  With no arguments it opens a window (plain USER32 controls, nothing
  newer than Windows 95, the insert on a worker thread so it keeps
  painting). **Insert is grey while a disc is in the drive** (user
  decision), so the tray is emptied with Eject on purpose; the command
  line still swaps in one step. It is a `-mwindows` program whose verbs
  print to a redirected stdout. XP's `cmd` passes that on; under Win98's
  `COMMAND.COM`, read the log instead.
- **`CDSHELF.COM`** (`src/cdshelf.asm`, NASM), driving the drive by PIO
  as `tools/atapi-guest-test.py` does. It finds the drive with IDENTIFY
  PACKET DEVICE, because by the time a DOS program runs the BIOS has left
  the ATAPI signature registers at zero. With no arguments it lists the
  shelf and waits for a key: 0–9 loads that disc, `E` empties, `R`
  re-reads, Esc quits. `CDSHELF LIST` is the non-interactive form.

Both **empty the drive and wait for it to report the tray empty before
loading**. Windows and MSCDEX cache the last disc, so a swap they never
saw as a removal leaves the old files on screen, and the device runs a
medium change from one bottom half, so an eject and a load sent back to
back collapse into one. The Windows build then dismounts the volume
(`FSCTL_DISMOUNT_VOLUME`), because its own TEST UNIT READY polling
consumes the one media-change sense the drive raises.

`tools/atapi-guest-test.py` runs the opcode and then
`CDSHELF.COM` on a FreeDOS floppy every time (`atapi-guest`);
`tools/cdshelf-guest-test.sh <image> xp` runs `CDSHELF.EXE` in XP (list,
load, `dir`/`type` off the loaded disc, the missing entry refused,
eject). The user has run the EXE by hand on Win98; a scripted `win98`
pass is not recorded.

## Other programs on the disc

- **`D3DGAME9.EXE` / `D3DGAME8.EXE`** (`src/d3dgame9.c`, `d3dgame8.c`,
  scene in `d3dgame.h`), the Direct3D reference workload (doc 14). It is
  the same deterministic scene on both APIs (mipmapped ground, lit indexed
  cubes, a per-frame dynamic vertex buffer, DXT1 particles through
  `DrawPrimitiveUP`, render-to-texture, a frame-time graph), windowed or
  `-fs` (`-w -h -bpp16 -novsync`). `-frames N` runs a fixed-step
  sequence and exits, `-dump N file.bmp` writes frame N; `-shader` adds a
  vs_1_1/ps_1_1 path when a d3dx9 DLL is present (the pixel shader is
  assembled, because d3dx9_33+ HLSL refuses ps_1_x). Keys: WASD/arrows/Q/E, F1
  wireframe, Space pause, Esc. Everything printed also goes to
  `d3dgame9.log` / `d3dgame8.log` (`-log file`). The rig (P4 + GeForce
  6200) runs it first and its BMPs are the goldens in `reference/d3d/`.
  Every emulated path is diffed against them with `tools/bmpdiff.py`.
- **`D3D9TEST.EXE`**, the D3D9 counterpart of wglgears, prints the adapter
  name
  (WineD3D reports a GL-derived one), HAL caps, the x87 control word after
  `CreateDevice` (`PC=24` expected), a spinning triangle's fps; an
  optional frame count.
- **`MODETEST.EXE`** prints the current desktop mode, the driver's mode
  list and the result of the `ChangeDisplaySettingsEx` calls ddraw/wined3d
  make. Run it when a full-screen game dies at start-up.
- **`WGLGEARS.EXE`** is Mesa's wglgears from qemu-3dfx's demos. Next to
  `OPENGL32.DLL` it is the zero-dependency GL pass-through check.

Every program of ours writes its log (and any BMP it dumps) to
`C:\2KSBOX` through `src/guestlog.h` (`guest_log_open()`,
`guest_path()`), whatever folder it was started from; `set BOXLOG=<dir>`
moves it. The rest of `TESTS\` and `DRIVER\` is catalogued in
`docs/testing.md`.

Later: SoftGPU (a pinned release), AC'97 and network drivers, an
in-guest `verify` tool (docs 04, 06).
