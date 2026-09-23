# Track: M11 — the Windows host (build, package, run)

Windows as a *host*: the whole stack cross-built from Linux into a
portable zip, the same stages built natively on a Windows PC for
debugging, and the Windows branches of shared code. The track is done:
the package runs on the user's PC (Ryzen 9 5900X, RTX 3090, the
`base98-br` image), including 3D guests on both Direct3D backends and the
OpenGL pass-through. This file is the record — scope, where the design
lives, how to test, the traps, what stayed open.

- **The build, the package and the platform's quirks:**
  `docs/build-windows.md` (the container, the stages, the DLL closure, Qt
  deployment, `2ksbox-debug.bat`, the Direct3D 9 backends, WGL, WHPX,
  the native MSYS2 build).
- The clang-built QEMU: `patches/qemu/README.md`, patch 68.
- The WGL backend's rule: doc 12, "The WGL rule".
- Which Direct3D 9 the executor runs on: ADR-007 and its second
  amendment (doc 10), `docs/build-windows.md`.
- The Windows keyboard capture: doc 03 §"Input path",
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

`package-windows.sh` is the Windows evidence `scripts/test.sh` cannot
give: the staged launcher's `--paths` and an offscreen window grab, the
player's `--companions` (the libraries QEMU loads by name), both
Direct3D host tests through the staged `d3dpt_exec.dll` + `dxvk_d3d9.dll`
(107 checks, frame byte-identical to Linux's), `qemu-img.exe` writing a
qcow2 (which also proves the DLL closure), and libslirp in the embed
DLL's import table. Wine is not the target: a wine failure is
investigated, not believed.

On the PC, in MSYS2's MINGW64 shell (one-time setup in
`docs/build-windows.md`, "Building on Windows"):

```sh
scripts/build-windows.sh              # natively
scripts/win-run.sh launcher           # the launcher out of the checkout
GDB=1 scripts/win-run.sh player ...   # the [player] line from launcher.log
build/win/d3dpt-dp2-test.exe / d3dpt-exec-test.exe   # with D3DPT_D3D9=dxvk and =system
```

What to ask the user for when a package misbehaves: `2ksbox-debug.bat`
and its `2ksbox-debug.log`. The last line of `launcher.log` names the
start-up step that died; no `launcher.log` at all means the loader or a
static initialiser, and the exit code says which (`0xC0000135` missing
DLL, `0xC0000142` initialiser, `0xC0000005` fault).
`PLAYER_KEYBOARD_LOG=1` puts the keyboard capture's decisions in
`player.log`. Both logs are in `%APPDATA%\2ksbox\data`.

## Traps

- **An import-table closure is not a closure.** A DLL loaded with
  `LoadLibrary` is in no import table: Fedora's SDL2 was sdl2-compat and
  loaded SDL3 that way ("Failed loading SDL3 library" on the first real
  run), and `libepoxy-0.dll` names `libEGL`/`libGLESv2` at run time.
  `package-windows.sh`'s strings pass catches them; Qt's platform plugin
  and QML modules are why the closure walks every binary in the package.
- **Fedora has no `mingw64-libslirp`**, and `-netdev user` is compiled
  in: without it every launcher machine dies with "network backend
  'user' is not compiled into this binary", while configure only says
  `slirp support: NO`. The image builds libslirp from source.
- **`windows_subsystem = "windows"` costs three things**: a console for
  the debug verbs (`console.rs` attaches the parent's, and gives every
  child `CREATE_NO_WINDOW`), a home for the player's output
  (`player.log`), and anywhere for a failure to go (`fatal.rs`: the log
  plus a message box). None is optional once the attribute is set.
- **cmd does not wait for a windowed program**, and `start "" /b /wait`
  (which does) does not pass the console's redirection to the child.
  Take the exit code from `start /b /wait` and the output from a file the
  program writes (`--diagnose`).
- **QMP `fd=` on Windows is a C-runtime descriptor**, not a `SOCKET`
  (QEMU's socket wrappers start with `_get_osfhandle`). Whose CRT table
  it lands in depends on the module, so the handle crosses into the
  embed library, which converts: `qemu_embed_socket_to_fd()`, embed API
  v7 (doc 11).
- **Two emutls registries.** rustc links libgcc statically for
  `x86_64-pc-windows-gnu`, `libstdc++-6.dll` uses `libgcc_s_seh-1.dll`'s,
  so `std::call_once` in cxx-qt's generated initialiser reached a
  `__once_proxy` reading `NULL`: the Qt launcher died before `main` with
  `0xC0000005`. A local `__once_proxy` fixes it; `-static-libstdc++`,
  `-C link-self-contained=no` and `-shared-libgcc` do not. MSYS2's GCC 16
  has no exported `std::__once_call`, so the proxy compiles only where
  `_GLIBCXX_NO_EXTERN_THREAD_LOCAL` is absent.
- **A COFF weak external is not an ELF weak definition**: patch 31's
  trick leaves `qemu-system-i386.exe` with undefined references, so
  `mglcntx_mingw.c` is split instead (`docs/build-windows.md`, "OpenGL
  for a Win98 guest").
- **A PE import library only satisfies symbols already undefined** when
  the linker reaches it: `launcher-qt/build.rs` names
  `-lQt6QuickControls2` again as a `rustc-link-arg`, since the archive
  holding `appearance.cpp` links after the Qt import libraries. ELF links
  either way.
- **`CXX_QT_AUTORCC_OPTIONS=--no-zstd`**: the host rcc has zstd and the
  mingw Qt6Core does not.
- **A low-level keyboard hook in the player is never called while the
  player is the foreground window** — the only time it is wanted; the
  cause was not found. The Windows keys are taken with raw input and
  `RIDEV_NOHOTKEYS` instead (doc 03 §"Input path"). System hotkeys
  (Alt+Tab, Alt+F4, Ctrl+Alt+Del, Win+L) reach no program.
- **Emulated TLS**: mingw GCC's `__thread` is a call per access, and QEMU
  touches thread-locals on every device access — a VGA register read
  cost 2.3x Linux's. The QEMU is built with clang (patch 68;
  `WIN_QEMU_CC=gcc` is the old build).
- `win-cross.sh` forwards a **whitelist** of environment variables into
  the container: a knob that seems ignored is probably not on it.
  `build-windows.sh` configures QEMU only when `build.ninja` is missing
  or the compiler changed, so a changed configure flag needs
  `scripts/win-cross.sh scripts/configure-qemu.sh --windows` by hand.
- A `wine` command piped into another looks like a hang: redirect to a
  file. Unset `DISPLAY`/`WAYLAND_DISPLAY` for anything under wine that
  might crash, or its crash dialog lands on the user's desktop.
- `%APPDATA%\2ksbox\data`, not `%APPDATA%\2ksbox`, is the data directory
  (`directories`' convention).

## What stayed open

1. **Moto Racer slow on the 5900X with the CPU at 5 %** — in the menus
   and the software-renderer race; 5 % is one of 24 threads, so
   CPU-bound. Not reproduced on Linux (60 flips/s, also under 15.6 ms
   waits). The user's log has no software-race window (no `0 draws`
   rates) and its last session switches to 640x480 at **8 bits** twice
   with no page flips, where the Linux run was 16 bits and flipping:
   that, and timing the clang-built QEMU (patch 68) there, are the next
   questions for the PC.
2. **Live control on the PC**: the launcher's monitor socket through
   Winsock AF_UNIX (`launcher-core/src/control.rs`) is written and not
   run — wine has no AF_UNIX (`socket()` answers 10047). Start a machine,
   take a snapshot, swap a disc from the shelf; `live control off: …` in
   `launcher.log` means the trial bind failed.
3. **The Windows-built guest-tools ISO in a guest** (its `SETUP.EXE` and a
   driver it installs). The native build builds every stage and runs the
   launcher; the ISO it builds has not been booted.
4. **An installer** beside the portable zip (doc 07); QEMU's own
   `mingw32-nsis` recipe is in the cross image's reach.
5. **Zero-copy frames** through a DXGI shared handle, the Windows
   counterpart of the dma-buf ring and IOSurface; frames take the
   readback path today.
6. **A Windows check that boots a guest**, in the shape of
   `tools/xp-driver-test.sh`: drive it over QMP, pull the artefacts, diff
   a frame.
7. **No Glide wrapper on Windows**: the cross build has no glide stage, so
   `player --companions` reports it "(not shipped)".
