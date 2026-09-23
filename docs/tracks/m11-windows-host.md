# Track M11: the Windows host (build, package, run)

Windows as a *host*: the whole stack cross-built from Linux into a
portable zip, the same stages built natively on a Windows PC for
debugging, and the Windows branches of shared code. The track is done.
The package runs on the user's PC (Ryzen 9 5900X, RTX 3090, the
`base98-br` image), including 3D guests on both Direct3D backends and the
OpenGL pass-through. This file keeps the scope, the test loop, the traps
and what stayed open. The design lives elsewhere:

- The build, the package and the platform's quirks are in
  `docs/build-windows.md` (the container, the stages, the DLL closure, Qt
  deployment, `2ksbox-debug.bat`, the Direct3D 9 backends, WGL, WHPX,
  the native MSYS2 build).
- The clang-built QEMU is patch 68 in `patches/qemu/README.md`.
- The WGL backend's rule is doc 12, "The WGL rule".
- Which Direct3D 9 the executor runs on is ADR-007 and its second
  amendment (doc 10), and `docs/build-windows.md`.
- The Windows keyboard capture is doc 03 "Input path" and
  `player/src/kbcapture.rs`.

## Scope and files

- Cross toolchain: `packaging/windows/Dockerfile`, `scripts/win-cross.sh`,
  `packaging/windows/clang-mingw-cc` / `-cxx`.
- Build: `scripts/build-windows.sh` (cross, and native under MSYS2
  MINGW64), the `--windows` mode of `scripts/configure-qemu.sh` and
  `scripts/build-d3dpt-exec.sh`, `scripts/win-run.sh`,
  `packaging/windows/qmake-host.c`, `guest-tools/msys2-i686.sh`.
- Package: `scripts/package-windows.sh`.
- The Qt reproducer: `tools/qtmin/` (its README has the `__once_proxy`
  diagnosis); the fix is `launcher-qt/src/once_proxy.cpp`.
- Windows branches of shared code: `embed/mglcntx_embed.c` (WGL) and
  `tools/wgl-probe.c`; `launcher-core/src/console.rs`, `fatal.rs`,
  `paths.rs`, `player.rs`, `wizard.rs`, `bundle.rs` (the one-folder
  layout, WHPX naming), `control.rs` (live control through Winsock
  AF_UNIX); `player/src/qmp.rs`, `player/src/kbcapture.rs`;
  `launcher-qt/src/appearance.cpp`; `d3dpt/hw/d3dpt_exec_load.c`,
  `d3dpt/exec/*.cpp`; `libdisc/src/bin/discx.rs`.
- Patches it added: 68 (clang), 69 (mkvenv's `file://C:/…` wheels URL
  under Python 3.14); Windows hunks in 10, 13, 50.

## Test loop

```sh
scripts/win-cross.sh --build          # once, and after a Dockerfile change
scripts/build-windows.sh              # qemu rust qt exec guest (cross)
scripts/build-windows.sh rust         # one stage (stages are positional)
scripts/package-windows.sh            # the zip, then its checks under wine
```

`package-windows.sh` gives the Windows evidence `scripts/test.sh`
cannot. It runs the staged package under wine (`docs/build-windows.md`
"The package"): the launcher's `--paths` and an offscreen window grab,
the player's `--companions`, both Direct3D host tests through the
staged `d3dpt_exec.dll` + `dxvk_d3d9.dll` (107 checks, frame
byte-identical to Linux's), `qemu-img.exe` writing a qcow2, and libslirp
in the embed DLL's import table. Wine is not the target, so a wine
failure is investigated, not believed.

On the PC, in MSYS2's MINGW64 shell (one-time setup in
`docs/build-windows.md`, "Building on Windows"):

```sh
scripts/build-windows.sh              # natively
scripts/win-run.sh launcher           # the launcher out of the checkout
GDB=1 scripts/win-run.sh player ...   # the [player] line from launcher.log
build/win/d3dpt-dp2-test.exe / d3dpt-exec-test.exe   # with D3DPT_D3D9=dxvk and =system
```

When a package misbehaves, ask the user for the `2ksbox-debug.log` that
`2ksbox-debug.bat` writes. `docs/build-windows.md` "The package" says
how to read it (the last `launcher.log` line, the exit codes when there
is no `launcher.log`). `PLAYER_KEYBOARD_LOG=1` puts the keyboard
capture's decisions in `player.log`. Both logs are in
`%APPDATA%\2ksbox\data` (`directories`' convention), not
`%APPDATA%\2ksbox`.

## Traps

These are in `docs/build-windows.md`, with the detail:

- An import-table closure misses DLLs loaded with `LoadLibrary`
  (sdl2-compat's SDL3, libepoxy's `libEGL`), so the package runs a
  strings pass ("The package").
- Fedora has no `mingw64-libslirp`; without it every machine dies with
  "network backend 'user' is not compiled into this binary" ("Why a
  container").
- `windows_subsystem = "windows"` costs a console for the debug verbs,
  a home for the player's output and somewhere for a failure to go, and
  cmd does not wait for a windowed program ("The package").
- Two emutls registries killed the Qt launcher before `main`
  (`0xC0000005`); a PE import library only satisfies symbols already
  undefined; the host rcc needs `CXX_QT_AUTORCC_OPTIONS=--no-zstd`
  ("Qt, which the package carries").
- A COFF weak external is not an ELF weak definition, so
  `mglcntx_mingw.c` is split ("OpenGL for a Win98 guest").
- mingw GCC's emulated TLS made a VGA register read 2.3x Linux's, so
  QEMU is built with clang (patch 68; `WIN_QEMU_CC=gcc` is the old
  build).
- `win-cross.sh` forwards a whitelist of environment variables, and a
  changed configure flag needs `scripts/win-cross.sh
  scripts/configure-qemu.sh --windows` by hand ("Why a container").

Kept here:

- **The emutls fix's limits.** A local `__once_proxy` fixes it;
  `-static-libstdc++`, `-C link-self-contained=no` and `-shared-libgcc`
  do not. MSYS2's GCC 16 has no exported `std::__once_call`, so the
  proxy compiles only where `_GLIBCXX_NO_EXTERN_THREAD_LOCAL` is absent.
- **QMP `fd=` on Windows is a C-runtime descriptor**, not a `SOCKET`.
  The embed library converts it with `qemu_embed_socket_to_fd()`, embed
  API v7 (doc 11).
- **A low-level keyboard hook in the player is never called while the
  player is the foreground window**, the only time it is wanted, and the
  cause was not found. The player takes the Windows keys with raw input
  and `RIDEV_NOHOTKEYS` instead (doc 03 "Input path"). System hotkeys
  (Alt+Tab, Alt+F4, Ctrl+Alt+Del, Win+L) reach no program.
- A `wine` command piped into another looks like a hang, so redirect it
  to a file. Unset `DISPLAY`/`WAYLAND_DISPLAY` for anything under wine
  that might crash, or its crash dialog lands on the user's desktop.

## What stayed open

1. **Moto Racer is slow on the 5900X with the CPU at 5 %**, in the menus
   and the software-renderer race. 5 % is one of 24 threads, so it is
   CPU-bound. Not reproduced on Linux (60 flips/s, also under 15.6 ms
   waits). The user's log has no software-race window (no `0 draws`
   rates), and its last session switches to 640x480 at **8 bits** twice
   with no page flips, where the Linux run was 16 bits and flipping.
   That, and timing the clang-built QEMU (patch 68) there, are the next
   questions for the PC.
2. **Live control on the PC.** The launcher's monitor socket through
   Winsock AF_UNIX (`launcher-core/src/control.rs`) is written and has
   not run, because wine has no AF_UNIX (`socket()` answers 10047). Start
   a machine, take a snapshot, swap a disc from the shelf; `live control
   off: …` in `launcher.log` means the trial bind failed.
3. **The Windows-built guest-tools ISO in a guest** (its `SETUP.EXE` and a
   driver it installs). The native build builds every stage and runs the
   launcher; the ISO it builds has not been booted.
4. **An installer** beside the portable zip (doc 07). QEMU's own
   `mingw32-nsis` recipe is within the cross image's reach.
5. **Zero-copy frames** through a DXGI shared handle, the Windows
   counterpart of the dma-buf ring and IOSurface. Frames take the
   readback path today.
6. **A Windows check that boots a guest**, in the shape of
   `tools/xp-driver-test.sh`: drive it over QMP, pull the artefacts, diff
   a frame.
7. **No Glide wrapper on Windows.** The cross build has no glide stage, so
   `player --companions` reports it "(not shipped)".
