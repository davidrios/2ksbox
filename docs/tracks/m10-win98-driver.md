# Track: M10 — the native Win98 display driver (doc 19, ADR-012)

The track that gives Win98/Me the driver XP has: the XP driver split
into an OS-independent core plus a thin per-OS layer, and a 9x layer on
that core, on the same `d3dpt-vga` adapter and the same protocol. The
design and every finding are doc 19; doc 15 is the core's
specification (everything it says about the adapter, the flip chain, the
DP2 stream and the DX8 DDI is what the core *is*). Read
`docs/00-status.md` first for the global picture and the track rules.
Work happens on `main`.

## State

Steps 0–4 are done and step 5, real titles, is where the work is.

- **The split** (doc 19 §19): `core/` holds the DP2 walker, surface
  table, caps and flip chain and includes no DDK header of either
  family; six `d3dpt_os_*` hooks are all it asks of the OS, and
  `build-driver.sh` proves that with `nm`. XP's driver was rebuilt on it
  unchanged (pixel-identical frames).
- **A 9x driver is three binaries** (doc 19 §1): the 16-bit DIB Engine
  display driver `d3dpt9x.drv`, the ring-0 mini-VDD `d3dpt9v.vxd`, and
  the ring-3 DirectDraw/Direct3D HAL `d3dpt9hl.dll`, which links the
  core and is loaded at one shared address in every process (§23).
- **Install**: PnP from the INF alone, no clicks (§16); on the ISO as
  `DRIVER9X\`, installed by `SETUP.EXE`'s display-driver component on
  98/Me. Win98 machines default to `d3dpt-vga` since 2026-09-16.
- **The whole M7c matrix reproduces on 98** with no change to `core/`
  (§24, §25): DirectDraw with paced flips and 8 bpp palettes; the DX3,
  DX7 and DX8 faces (`EBTEST` 5/5, `D3D7TEST` byte-identical to
  `d3dpt-dp2-test`'s frame, `SHTEST`, `CKTEST`, `DXTTEST`, `CUBETEST`).
- **The screen switches**: a full-screen DOS box both ways (§26, §29:
  the INT 2Fh hook and USER's repaint), visible blue screens (§29), the
  monitor power-down (§41), the shutdown screen (§35), DirectDraw's own
  Mode X for 320×200 titles (§30).
- **No executor** (`no-exec=on`): the HAL keeps DirectDraw and offers no
  Direct3D (§40); WineD3D can then be the whole machine's DirectDraw,
  decided at every login by `D3DPRE.EXE` (§42, §43; retired with the rest
  of WineD3D-in-guest in M15's last step, not before).
- **Titles**: Total Annihilation, LEGO Island, Carmageddon, Blood (DOS
  box), Crimson Skies (menus and flight, §28, §34), Diablo II (§33),
  3DMark 99, 3DMark2001 SE's whole benchmark (§36–§39).

## Scope and files

Owned by this track:

- `guest-tools/src/d3dptvid/w9x/`: `d3dpt9x.c` + `dibthunk.asm` (the
  `.drv`), `d3dpt9dd.c` (its DirectDraw escapes), `d3dptvxd.c` (the
  mini-VDD), `d3dpthal.c` + `.def` (the HAL DLL), the shared headers,
  `d3dpt9x.inf`, `res/` (the `oembin` resources GDI requires), and the
  probes `ddprobe.c`, `gdiprobe.c`, `pwrprobe.c`, `devcaps.c`,
  `setbpp.c`, `bsod.c` + `bsodvxd.c`.
- `guest-tools/src/d3dptvid/ddk9x/` — the vendored 9x interface headers
  (MIT, from `vmdisp9x`; provenance in its README) —
  and `guest-tools/build-driver9x.sh`.
- `guest-tools/src/setup.c`'s 98/Me display-driver component and
  `tools/setup-guest-test.sh`'s Win98 expectations.
- Tests: `tools/win98-driver-test.sh`, `tools/win98-game-test.sh`,
  `tools/win98-bsod-test.sh`.

Shared (rebase first, edit minimally, name the other track): `core/`
and `nt/` (M7), `d3dpt/d3dpt_fb.h` and `d3dpt/hw/d3dpt_vga.c` (M7 — the
adapter needed no change for 9x, and a change would be a finding),
`d3dpt/d3dpt_proto.h` and `d3dpt/exec/` (M4/M7), `build-wrappers.sh`,
`scripts/test.sh`.

## Where the design lives

| Topic | Doc 19 |
|---|---|
| why a native driver, the split | "Why a native driver…", "The split", §19 |
| the 9x driver model | §1–§10 |
| mini-VDD, linking a VxD the VMM loads | §11, §12 |
| the silent refusals (NE relocation, ring-3 mapping, 16-bit register access, `__loadds`) | §13, §14, §18 |
| reading a fault out of VRAM | §15 |
| install, `SYSTEM.INI`, ending a run | §16, §17 |
| the HAL: publication, HALINFO validation, the shared arena | §20–§23 |
| DirectDraw and Direct3D DDIs | §24, §25 |
| titles and screen switches | §26–§39 |
| no executor, WineD3D as the fallback | §40, §42–§44 |
| monitor power-down | §41 |

## Build and test loop

```sh
scripts/build.sh                           # once: this checkout's QEMU, executor, ISOs
guest-tools/build-driver9x.sh              # .drv + .vxd + HAL DLL + INF -> guest-tools/out/driver9x/
tools/win98-driver-test.sh <image> install # fresh raw copy, driver staged, PnP installs it, reboot
tools/win98-driver-test.sh <image> boot    # every run after: re-stages the binaries, boots (~4 min)
PROG=guest-tools/out/driver9x/ddprobe.exe tools/win98-driver-test.sh <image> boot
```

- `install` converts a fresh raw copy of the image (the user's image is
  never written) and stages the driver and its INF in `WINDOWS\INF`, so
  PnP installs it with no clicks (§16). It is also the reset button and
  the way out of safe mode. `NAME_IN_INI=1` names the driver in
  `SYSTEM.INI` instead, for "does this build work" rather than "does it
  install". `boot` re-stages the binaries and `PROG`, and deletes the
  last run's probe logs.
- `PROG=` names a program in `WIN.INI`'s `run=`: the harness has no
  serial line, and nothing on a Win98 desktop calls `DirectDrawCreate`
  on its own.
- The harness prints the adapter's BARs, the driver's `d3dpt9x:` /
  `d3dpt9dd:` / `d3dpthal:` lines (DEBUG register), the screendump's
  colour count (16 or fewer: Windows fell back to VGA) and **the VGA text
  page read out of VRAM**, where a fatal exception writes itself.
  Believe that over the screendump (§15). A fault names a selector and
  offset: take the selector's base from the LDT (`memsave` at the base
  `info registers` gives), read the code there and disassemble it.
- `DDFLAGS=` passes the adapter's knob through (the 9x driver reads the
  high half, `D9F_*` in `w9x/d3dpt9x.h`), `NO_EXEC=1` the no-executor
  host, `SHOTS=`, `BOOT_WAIT=`, `OUT=`.
- Games: `tools/win98-game-test.sh <image> <name>` (with `PLAYER=1` for
  Glide/OpenGL titles), blue screens: `tools/win98-bsod-test.sh`. Both,
  and the rest, in `docs/testing.md`.
- XP's regression oracle after touching `core/`:
  `tools/xp-driver-test.sh <xp image> d3d7` plus `shtest`, `cktest`,
  `ebtest` (the M7 track).

**Images.** Use a launcher Win98 machine installed after the BIOS-date
stamp (`~/.local/share/2ksbox/machines/<name>/disk.qcow2`; sessions have
used `test98`, `claude98`, `base98-us`, `base98-br`). **Not**
`~/vms/win98.qcow2`: a pre-stamp PnP-BIOS install our INF does not
match, which ends on the inbox VGA with an empty log. The guest runs
**DirectX 9.0c** by decision (§25): the in-box 6.1 cannot run
`D3D7TEST`'s DX7 path, so it has no pixel oracle. Installing DirectX on
an image means booting it on `-vga cirrus`, which rebinds the display
to the in-box driver and drops `D3DPT9V.VXD` — run `install`, not
`boot`, after any hand session on the image.

### The second toolchain

The `.drv` and the `.vxd` need **Open Watcom v2** (mingw makes neither
format; the HAL DLL builds with `i686-w64-mingw32`). One tarball carries
every host (`binl64`, `armo64`, `bino64`, `binnt64`), unpacked with no
sudo where `build-driver9x.sh` looks by default (`WATCOM=` overrides;
Windows: `docs/build-windows.md`):

```sh
curl -L -o ow.tar.xz https://github.com/open-watcom/open-watcom-v2/releases/download/Last-CI-build/ow-snapshot.tar.xz
mkdir -p ~/.local/opt/open-watcom && tar xJf ow.tar.xz -C ~/.local/opt/open-watcom
```

A host without it builds the ISO without `DRIVER9X\` and says so. The
script also applies the post-link fixes `wlink` gets wrong (doc 19 §12
for the VxD; the NE's expected Windows version and zero local heap) and
refuses the three silent failures at build time. The macOS `.drv`
differs from Linux's by one instruction selection, harmlessly
(`docs/build-macos.md`).

**Reference trees**, read and never vendored (gitignored):

```sh
git clone --depth 1 https://github.com/JHRobotics/vmdisp9x build/ref/vmdisp9x  # .drv + VxD
git clone --depth 1 https://github.com/JHRobotics/vmhal9x  build/ref/vmhal9x   # ring-3 HAL
curl -L -o build/ref/fixlink.c https://raw.githubusercontent.com/JHRobotics/fixlink/master/fixlink.c
```

## Traps

Open Watcom's own:

- **Its inline assembler does not resolve a callee through a macro
  parameter**: `_asm { call p }` inside a `#define` calls nothing, and the
  only sign is warning W202 ("defined, but not referenced"). The four
  mini-VDD screen-switch thunks are written out by hand for this.
- **It takes a function's attributes from the first declaration it
  sees**: a DDK prototype without `__loadds` strips it (§18), and a Win16
  API of the same name wins outright (`SetCursor` vs the driver's ordinal
  102). Hide both with a `#define` before the headers.
- **No CRT, so no 32-bit multiply**: `(DWORD)a * b` in 16-bit code links
  against an undefined `__U4M`; use `MulW`.

The guest's:

- **Win98 runs under TCG**, never KVM (Explorer dies at startup).
- **End a run with the ACPI power button**; a machine that does not
  power off leaves the FAT dirty and the next boot is safe mode, which
  reads exactly like the driver failing (§17).
- **A VxD the VMM dislikes is simply not loaded**: no `BOOTLOG.TXT`
  line, nothing anywhere. Suspect the linker first (§12). `BootLog=1`
  did not refresh `BOOTLOG.TXT` on this image — check the file's date
  before believing it.
- **Edit `SYSTEM.INI` in binary or not at all**: a text-mode rewrite
  strips CRLFs and eats a section header, which looks like Windows
  rejecting the setting.
- A value the layer **derives** instead of reading from the adapter
  fails silently (§19's wrong DDK constant, §25's command window 16 KiB
  low: every call returned success, no `ddi:` line on the host). Read
  `D3DPT_FB_REG_CMD_OFFSET` and the other registers, as `nt/` does.
- A title's DOS half needs `cd` before a DOS/4GW EXE, and an image with
  no `SET BLASTER=` fails every DOS game's sound probe before it draws.

## Next steps

1. **The doc 04 title matrix**: the same Win98 titles through this driver
   and through the Glide/WineD3D control — which is faster, which is
   correct, what the launcher defaults to.
2. **Total Annihilation's exit from inside a skirmish** (the user's
   crash report; an exit from the main menu is clean).
3. **A fault inside a HAL callback leaks `cmd_lock`** and freezes the
   session until the process dies (§36); an unwind that releases it
   would make the next such bug one failed call.
4. **ACPI standby**: on resume nothing reprograms the adapter and the
   screen is a blank VGA text page; the player does not report
   `SUSPEND`/`WAKEUP` (§41).
5. WineD3D-in-guest (§42–§44) goes in M15's last step, once the host
   Wine executor has been measured on real games — not earlier.
