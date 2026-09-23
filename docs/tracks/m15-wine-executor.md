# Track M15: the Direct3D executor on Wine, on the host

The Direct3D fallback for a Linux or macOS host below DXVK's Vulkan 1.3
floor. It runs the **same** paravirtual device and the **same** executor
as everywhere else on **Wine's d3d9 (WineD3D over OpenGL), in a Wine
process on the host**, instead of copying a 2015 Wine into the guest
next to every game. The user opened it on 2026-09-22 ("running an old
unsupported Wine version in the guest is bad UX and doesn't make
sense"). ADR-018 in doc 10 is the decision and its rejected
alternatives. Doc 14 "The executor on Wine, in another process" is the
design (the child, the wire, the shared file). `d3dpt/exec/d3dpt_exec.h`
is the API carried across. A Windows host uses the system's own d3d9 in
process instead of Wine (ADR-007's second amendment).

## Scope and files

- The child is `d3dpt/exec/d3dpt_exec_host.c` → `d3dpt-exec-host.exe`,
  a PE program that loads the Windows build of `d3dpt_exec.dll`.
- QEMU's side is `d3dpt/exec/d3dpt_exec_remote.c` →
  `libd3dpt_exec_remote`, the wire `d3dpt/exec/d3dpt_remote.h`, and the
  back-end choice in `d3dpt/hw/d3dpt_exec_load.[ch]`.
- The shared file behind the devices' memory is in
  `d3dpt/hw/d3dpt_vga.c` (VRAM, QEMU patch 73) and `d3dpt/hw/d3dpt_mm.c`
  (the SysBus window).
- Build and packages are the `--wine` stage of
  `scripts/build-d3dpt-exec.sh` (the PE pair into `build/d3dpt/wine/`),
  `scripts/build.sh`, `package-linux.sh`, `package-macos.sh
  --community` and the Flatpak manifest.
- The launcher side is `launcher-core/src/host_gpu.rs`
  (`D3dBackend::Wine`, the third verdict), the form's Direct3D note
  (`wizard::Form::d3d9_note()`) and `player/src/companions.rs` (`wine`,
  `wine-host`, `d3dpt-remote`).
- Tests are the `exec-wine` check, the `EXEC=` knob of
  `tools/xp-driver-test.sh`, `tools/xp-fifa-match.sh` and
  `tools/tcg-profile.sh`, and `tools/macvm-wine-spike.sh` /
  `tools/macos-wine-spike-local.sh`.
- Shared and edited minimally: `d3dpt/exec/d3dpt_exec.cpp` needs nothing
  for Wine, because the hidden window and executor-owned scene of the
  `system` branch are what wined3d wants. `d3dpt/d3dpt_proto.h` is
  untouched.
- **Not this track's until step 6** is everything of WineD3D-in-guest:
  the wine9x build in `guest-tools/build-wrappers.sh`,
  `patches/wine9x/`, the ISO's `WINED3D\` folders, `SETUP /GAME 4`/`5`
  and `/I 7`, `guest-tools/src/d3dpre.c`, `tools/xp-wined3d-test.sh`,
  `tools/wined3d-sys-test.sh`, doc 19 §42–44. It stays the running
  fallback for a below-floor host with no Wine until step 6 removes it.

## State

Built and working end to end on the M1 Air. One acceptance run is left
(step 5).

- **Host tests.** `d3dpt-dp2-test` and `d3dpt-exec-test` through the
  child draw frames byte-identical to in-process DXVK's, on the Air
  under Rosetta (WineHQ 11.17) and on a real macOS 15.8 on the M1's own
  GL (328 fps, user-confirmed). The `exec-wine` check holds it.
- **XP guest.** D3DGAME8 runs within the rig budget (600 frames in
  3.4 s against 2.1 s in process). The ten DX8 DDI probes give the
  in-process verdicts (nine PASS, PATCHTST NOT OFFERED). FIFA 2000 plays
  a match at 22.6 frames/s against 19.0 in process.
- **Win98 guest.** D3D7TEST runs through the 9x HAL, 300 frames at
  57 fps, byte-identical to `dp2-test.bmp`.
- **Launcher and packages.** The third verdict works, `--host-check`
  exits 0 through Wine, and `--paths` / `--companions` list the Wine and
  the pair. The Linux package and the macOS community build carry
  `libd3dpt_exec_remote` + the PE pair (all or none). No package ships a
  Wine; the note says which to install.
- **The community app on a real macOS 15.** Its first start crashed at
  the adapter's realize (Traps, the DXVK probe). With that fixed and the
  executor rebuilt on the 15 side, the packaged player boots the XP
  machine to its desktop on the Wine executor through the packaged pair
  (`linear mode on (800x600x16)` at 25 s, `Direct3D executor in another
  process (Wine), ready`). The launcher there probed no Vulkan at all;
  that is fixed too, and `build/macos-community/` is rebuilt with both
  fixes. The crash is under "Traps" below, the probe in
  `docs/build-macos.md` "The app".

## Rejected alternatives

ADR-018 gives the first three. They are listed here so nobody
re-proposes them.

- **wined3d in process (winelib).** `d3d9.dll` and `wined3d.dll` are PE
  modules over `opengl32.dll` and need Wine's loader and a `wineserver`.
  There is no `libwined3d.so`, and a winelib binary *is* a Wine process.
- **An executor of our own over GL, Metal or wgpu.** That is a second
  implementation of D3D9 semantics, which WineD3D already is.
- **Native arm64 Wine on macOS.** `winemac.drv` gets no OpenGL there.
  Revisit when Wine's Mac driver has Metal-backed GL or the floor is a
  macOS with Vulkan.
- **Copying VRAM over the pipe** instead of sharing it. The DDI path
  reads vertex buffers out of VRAM per draw and the executor writes a
  readback per frame, so a copy is a round trip per draw.
- **A socket instead of stdio.** Nothing needs more than the child's two
  pipes, and Wine's AF_UNIX support is recent.

## Test loop

The knobs (`D3DPT_EXEC`, the adapter's `exec=`, `D3DPT_WINE`,
`D3DPT_WINEPREFIX`, `D3DPT_EXEC_REMOTE_LIB`, `D3DPT_EXEC_HOST`,
`D3DPT_WINE_RENDERER`, `D3DPT_REMOTE_DIR`) are in `docs/development.md`,
the tools in `docs/testing.md`. On a host that has Vulkan, `exec=wine`
is the A/B.

```sh
scripts/build.sh                  # the native stack, libd3dpt_exec_remote, and with mingw the PE pair
scripts/test.sh host              # exec-wine (SKIPs without a Wine or the pair)
build/qemu/qemu-img create -f qcow2 -b ~/vms/winxp-m7.qcow2 -F qcow2 build/xp.qcow2   # never the image
tools/xp-driver-test.sh build/xp.qcow2 install                 # once: the image's driver predates 2026-09-12
EXEC=wine tools/xp-driver-test.sh build/xp.qcow2 d3dgame8      # the reference scene on Wine
EXEC=dxvk tools/xp-driver-test.sh build/xp.qcow2 d3dgame8      # the control
EXEC=wine tools/xp-driver-test.sh build/xp.qcow2 probes        # the ten DX8 DDI probes
EXEC=wine tools/xp-fifa-match.sh tcg build/xp.qcow2            # a real game
# the Win98 HAL: base98-us has a Voodoo 2, in the launcher's slot (addr=0x05)
STAGE=guest-tools/out/driver9x/d3d7test.exe PULL='2KSBOX\D3D7TEST.LOG 2KSBOX\D3D7TEST.BMP' \
  GUEST_CMD=$'start /w C:\\D3D7TEST.EXE 640 480 32 300' \
  EXTRA='-device voodoo2,addr=0x05 -global voodoo2.texmem=2 -global d3dpt-vga.exec=wine' \
  tools/win98-game-test.sh "<machines dir>/base98-us/disk.qcow2" d3d7wine
python3 tools/bmpdiff.py build/test/dp2-test.bmp build/w98game/d3d7wine/D3D7TEST.BMP --tolerance 8
```

**A pre-26 macOS** is the community build's user (ADR-019). A virtual
machine cannot test the GL path. Apple's paravirtual GPU offers no
accelerated OpenGL, and Wine's Mac driver requires `kCGLPFAAccelerated`
for its bootstrap context, so Wine has no OpenGL at all there
(`tools/macvm-wine-spike.sh` says so; `RENDERER=vulkan` over MoltenVK is
a data point only). The real one is a second APFS volume on the Air
with macOS 15. One machine runs one macOS at a time, so the run happens
from a Terminal booted into that volume, with
`tools/macos-wine-spike-local.sh` from the other volume's checkout. A
bare macOS 15 needs Rosetta (`softwareupdate --install-rosetta
--agree-to-license`). Its Python is the Command Line Tools stub, so the
script falls back to uv's.

**Which Wine.** Linux uses the distro's (`wine`/`wine64` on `PATH`). On
a Mac, Homebrew's Wine casks are disabled (not notarized), so it is
WineHQ's tarball from Gcenx's releases (x86_64, under Rosetta on Apple
Silicon). Unpack it into `build/wine/` in a checkout, or install the
app in `/Applications`, which is where a packaged app looks (besides
`PATH` and `D3DPT_WINE`).

## Traps

- **Run `install` on an XP overlay first.** `winxp-m7.qcow2`'s driver
  (2026-09-08) predates the any-newer-version rule and refuses the
  adapter. XP then runs on its VGA fallback, D3DGAME8 dies in a modal
  box, and the killed guest loses its unflushed logs. The proof of an
  install is the mode-set line after the restart.
- **XP with its display driver refused shows a black screen** on the
  adapter, while BIOS text and mode 13h render. The VGA core sits in a
  chained 256-colour 800×600 mode (`sr4=0a gr5=50`) that renders
  nothing. Every screendump of a failed install run is black; read COM1.
- **A first batch stalls the vCPU for as long as wined3d compiles its
  shaders** (the first second at ~6 fps). So the child makes its device
  at the library's probe, during the adapter's realize, not at the first
  `create()` inside an MMIO read. A 3.5 s stall there reset the Win98
  machine. A long stall also let patch 65 raise a quarter second of owed
  PIT ticks back to back, which Windows 98's early-acknowledging timer
  handler nested into a VMM fault. The patch now paces the catch-up
  (`patches/qemu/README.md`, 65). Any executor that slow would have done
  it, and a below-floor host is exactly that.
- **Carry `base98-us`'s Voodoo 2 in the launcher's slot** in a harness
  run. Without the card, 3dfx's login helper fails in an "Error" box
  that takes exclusive mode from the test (`DDERR_NOEXCLUSIVEMODE` at the
  first `BeginScene`). In another slot Windows finds new hardware and
  wants a restart before the shell.
- **Moto Racer is no Direct3D test on these images.** `winxp-m7`'s copy
  runs its software renderer (0 draws, flip-chain readbacks only), and on
  `base98-us` the game takes the Voodoo 2.
- **QEMU must survive DXVK's probe on a host with no Vulkan.** DXVK with
  no loader calls through a null pointer in `Direct3DCreate9`, and
  unloading a probed library crashed too. The executor asks for
  `libvulkan` before trying DXVK and never closes a probed library.
- **It must also survive a loader with no working device.** On macOS 15
  KosmicKrisp loads and reports no GPU, and DXVK's constructor throws out
  of `Direct3DCreate9`. The executor caught it and tried its next
  candidate, the same library by its leaf name. DXVK's `Singleton` had
  already counted a user, so that second call got a null instance and
  faulted. DXVK patch 09 counts after constructing, the executor never
  asks a refused library twice, and the `exec-no-device` check asks the
  artefacts.
- **The shared-file path is POSIX-only** in QEMU.
  `memory_region_init_ram_from_fd` is under `CONFIG_POSIX`, so both call
  sites carry the same guard or the Windows build fails ("call to
  undeclared function"). Windows loses nothing, having no remote
  executor, but a future Windows out-of-process executor needs another
  mapping call.
- An overlay's absolute backing path does not exist on the other macOS
  volume. Flatten the image (`qemu-img convert`) for a run there.
- Homebrew's mingw links `libwinpthread-1.dll` dynamically (the Fedora
  cross image's does not), so the pair needs it beside the `.exe`.

## Steps

Numbered as ADR-018, doc 07 and CLAUDE.md cite them.

1. **The spike.** Done: the Windows executor on Wine's own d3d9, both
   host tests byte-identical to DXVK, no executor change. Left is the
   same on Linux Wine with a real GL (the rig). `scripts/test.sh host`'s
   `exec-wine` does it given the distro's Wine, so it is a run, not a
   question.
2. **The transport.** Done (doc 14). A pipe round trip per `submit` did
   not matter to the host tests (553 fps through the pipe against 929 in
   process). If a guest profile ever shows it, the designed answer is a
   bounded spin on a sequence word in the shared file before blocking on
   the pipe.
3. **The guest.** Done on XP and Win98 (State). Open beside it is the XP
   black screen on the VGA fallback (Traps): who programs that mode and
   why it draws nothing.
4. **The launcher and the packages.** Done (State; doc 07 has the
   verdicts and sentences).
5. **A real game on a below-floor host.** This is the acceptance and
   what is left. The community app is built on the 26 side
   (`scripts/package-macos.sh --community --no-sign --no-dmg --out
   build/macos-community`; the packager needs Homebrew, `macdeployqt` and
   the Vulkan SDK) with `build/xp-mac15.qcow2` (the XP overlay,
   flattened). Booted into macOS 15, in a Terminal:

   ```sh
   cd "/Volumes/Macintosh HD - Data/Users/david/work/win-98-xp-virt"
   cp -R "build/wine/Wine Staging.app" /Applications/   # once
   build/macos-community/2ksbox.app/Contents/MacOS/2ksbox --host-check
   #   "runs through Wine on this host (Wine 11.17, OpenGL)", exit 0: KosmicKrisp
   #   loads below 26 but reports no GPU, so DXVK finds no device and the third verdict is the one
   open build/macos-community/2ksbox.app
   #   an XP machine on a copy of build/xp-mac15.qcow2, Direct3D "auto";
   #   FIFA 2000 into a match
   ```

   It passes when the QEMU log says `exec: Direct3D executor in another
   process (Wine), ready`, the match draws on the M1's GL through the
   packaged pair, and its frame rate is written down against the Air's
   22.6. The reboot is the user's to do. The first run (2026-09-23)
   reached XP's desktop after the realize fix (State). Left is FIFA 2000
   into a match from the launcher there, with its frame rate.
6. **Retire WineD3D-in-guest, in one commit**, once 5 passes. That
   removes the ISO's `WINED3D\` folders and README, `SETUP /GAME 4`/`5`,
   `/I 7` with `D3DPRE.EXE` and the `DDRAWME`/`DDSYS` switcher, the
   wine9x build and `patches/wine9x/`, `tools/xp-wined3d-test.sh` and
   `wined3d-sys-test.sh`, the launcher's WineD3D advice, doc 04's rows,
   doc 19 §42–44's status, and CLAUDE.md's sentence about never deleting
   the `WINED3D\` ISO folder. The same step decides the Flatpak's Wine
   (bundle a trimmed one, the `org.winehq.Wine` extension, or "install
   it") and its PE pair (the SDK has no mingw, so a checked-in build as
   `firmware/vgabios-*.bin` is, or a release asset).

## Rules

- One decoder for every back end (DXVK native, DXVK on Windows, the
  Windows system d3d9, Wine). Nothing Wine-specific goes into
  `d3dpt_exec.cpp`. A WineD3D quirk is answered in the child or written
  down, never by forking the decoder.
- Every claim about a frame comes from a BMP in `build/` diffed with
  `tools/bmpdiff.py`.
- The guest-side WineD3D stack is not touched before step 6, and step 6
  is one commit.
- One TCG guest at a time; end scripted Win98 runs with the ACPI power
  button (CLAUDE.md).
