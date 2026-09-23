# Building and packaging for Windows (from Linux)

The Windows package is a **cross build**, done on the Linux machine that
does the rest of the work. QEMU with a mingw toolchain, Rust for
`x86_64-pc-windows-gnu`, the Qt launcher and the Direct3D executor all
cross cleanly, and the result is a portable zip. The same stages also
build **natively on a Windows PC** under MSYS2, for debugging there with
gdb ("Building on Windows" below); the package still comes from Linux.

The package runs on the user's PC (Ryzen 9 5900X, RTX 3090), with Win98
guests on the Voodoo 2, dxdiag and 3DMark 99 on DXVK, D3D7TEST on both
Direct3D backends, GLPROBE through the GL pass-through, MIDI and CD
audio. The track record is `docs/tracks/m11-windows-host.md`. Names and
the install layout are in doc 07.

## The short version

```sh
scripts/win-cross.sh --build      # once: the cross container (~5 min, ~3 GB)
scripts/build-windows.sh          # qemu, rust, qt, exec, guest-tools
scripts/package-windows.sh        # the zip, checked under wine
```

The artefact is `build/win/package/2ksbox-<version>-windows-x86_64.zip`.
Nothing here touches `build/qemu` or `target/release`. Windows output
goes to `build/win/` and `target/x86_64-pc-windows-gnu/`, so a checkout
holds both builds side by side. The *sources* are shared, though, so never
run `build-windows.sh` (which re-applies the patch queue) while another
build reads `qemu/` (00-status, "Building").

## Why a container

QEMU needs glib, pixman, zlib and libepoxy **for the mingw target**.
Arch packages `mingw-w64-gcc` and nothing else; Fedora packages them
all, and QEMU's own Windows CI uses that base
(`qemu/tests/docker/dockerfiles/fedora-win64-cross.docker`), so a build
failure in here is one upstream would see too.
`packaging/windows/Dockerfile` adds:

- **rustup with `x86_64-pc-windows-gnu`.** The player links the embed
  DLL and `libdisc` is linked into QEMU, so Rust must share the mingw
  ABI (`-msvc` would mean a second toolchain and import-library work).
- **Python 3.13.** Fedora's default is 3.14 and QEMU 9.2's `mkvenv`
  takes 3.8–3.13. The real `distlib` is installed because pip ≥ 26's
  vendored copy is incomplete ("found no usable distlib"), and the image
  build is the last moment with a network.
- **libslirp from source.** There is no `mingw64-libslirp`, and `-netdev
  user`, which every launcher machine asks for, must be compiled in.
  Without it a machine dies with "network backend 'user' is not compiled
  into this binary" while configure only said `slirp support: NO`.
  `package-windows.sh` checks the embed DLL's import table for it.
- **clang and lld**, with `packaging/windows/clang-mingw-cc` / `-cxx` as
  the compiler wrappers. QEMU is built with clang (patch 68), because mingw
  GCC has only emulated TLS and QEMU touches thread-locals on every
  device access (a VGA register read cost 2.3x Linux's).
  `WIN_QEMU_CC=gcc` builds the old way.

`scripts/win-cross.sh` runs a command in the container with the checkout
bind-mounted **at the same absolute path**, so meson's absolute paths
work from both sides, and with rootless podman's `--userns=keep-id`
everything comes out owned by you. `CONTAINER=docker` switches engines;
`WIN_CROSS_FEDORA=` picks another base.

Three things that look like the build ignoring you:

- `win-cross.sh` builds the image only when it is *missing*. After a
  Dockerfile change run `scripts/win-cross.sh --build`.
- It forwards a **whitelist** of environment variables (`JOBS`,
  `QEMU_PYTHON`, `WIN_QEMU_CC`, …). A knob that seems ignored is probably
  not on it. Stages are positional: `scripts/build-windows.sh qemu`.
- `build-windows.sh` configures QEMU only when `build/win/qemu/build.ninja`
  is missing or the compiler changed, so a changed configure flag reaches
  the native build and not this one until
  `scripts/win-cross.sh scripts/configure-qemu.sh --windows` is run by
  hand.

## What each stage produces

| Stage | Output | Notes |
|---|---|---|
| `qemu` | `build/win/qemu/{qemu-system-i386,qemu-img,qemu-io}.exe`, `libqemu-embed-i386.dll` | `configure-qemu.sh --windows`; WHPX built in; clang |
| `rust` | `target/x86_64-pc-windows-gnu/release/{player,launcherx,discx}.exe` | `qemu-embed/build.rs` finds the DLL in `build/win/qemu` |
| `qt` | `launcher-qt/target/x86_64-pc-windows-gnu/release/launcher-qt.exe` | the package's `2ksbox.exe` (ADR-015); its own workspace |
| `exec` | `build/win/dxvk/src/d3d9/d3d9.dll`, `build/win/d3dpt/d3dpt_exec.dll`, `build/win/d3dpt-dp2-test.exe`, `build/win/d3dpt-exec-test.exe`, `build/win/wgl-probe.exe` | DXVK (patch 08's headless WSI), the executor, its two host tests, the offscreen-GL probe |
| `guest` | `guest-tools/out/guest-tools-*.iso` | host-independent, built only if absent |

## The package

A Windows package is **one folder**, not a Unix prefix. Executables are
at the top, every DLL beside them (where the loader looks, so no rpath
and no `PATH`), and data directories under it.

```
2ksbox.exe  2ksbox-player.exe  qemu-img.exe
libqemu-embed-i386.dll  d3dpt_exec.dll  dxvk_d3d9.dll  <the mingw runtime, Qt>
pc-bios\  guest-tools\  shaders\  tools\  doc\  plugins\  qml\  qt.conf
2ksbox-debug.bat
```

`launcher-core/src/paths.rs` knows both shapes. On Windows the prefix is
the executable's directory and `pc-bios\` is the marker. The user's data
lives in `%APPDATA%\2ksbox\data`.

**Both programs are windowed** (`windows_subsystem = "windows"`), or a
double-click opens a black terminal for the session. That leaves three
things to provide:

- a console for the debug verbs. `launcher-core/src/console.rs`
  attaches the one they were launched from, and starts every child
  (player, `qemu-img`) with `CREATE_NO_WINDOW`;
- a home for the player's output, `%APPDATA%\2ksbox\data\player.log`;
- somewhere for a failure to go. `launcher-core/src/fatal.rs`, the
  launcher's first call, writes `launcher.log` beside `player.log` with a
  milestone per start-up step (the last line names the step that died),
  and installs a panic hook that files message, location and backtrace
  and says so in a message box. `--diagnose` writes `--paths` and
  `--host-check` there.

Every Play also writes the whole player command line into
`launcher.log` as `[player] …`, into the head of `player.log`, and to a
terminal if there is one, quoted for pasting back into a shell.

**`2ksbox-debug.bat`** is the double-click for all of it. It runs the
launcher from a console through `start "" /b /wait` (cmd does not wait
for a windowed program, and `start /b /wait` does not pass its
redirection on, so output comes from the file the program writes) and
writes `2ksbox-debug.log` with the exit codes and a copy of
`launcher.log`. **A missing `launcher.log` is itself the answer.**
Nothing of ours ran, and the exit code says why: `0xC0000135` a missing
DLL, `0xC0000142` an initialiser, `0xC0000005` a fault.

**The DLLs are a closure, not a list.** `objdump` walks the import
tables from the staged binaries, ships what is in the mingw sysroot and
never what is Windows' own (`kernel32`, `opengl32`, `d3d9`, the
`api-ms-win-*` sets). A system DLL copied in is how an app ends up
running only where it was built. An import table does not name what is
loaded at run time (Fedora's SDL2 was sdl2-compat and `LoadLibrary`ed
SDL3, "Failed loading SDL3 library" on the first real PC; `libepoxy-0.dll`
names `libEGL` / `libGLESv2` the same way), so a second pass searches
every staged binary for the name of any sysroot DLL not yet staged and
ships it too. SDL is no longer built at all.

The cross-built `qemu-system-i386.exe` therefore has no display and no
audio backend, as on every platform. Given no `-display` it starts a VNC
server on `localhost:5900`, and the player is what draws and sounds a
guest.

**`package-windows.sh` runs the staged package under wine**, from
outside the checkout with an empty environment:

- the launcher's `--paths` must answer inside the package;
- the player's `--companions` must name the staged executor and
  `dxvk_d3d9.dll` (loaded by name, in no import table);
- the display driver's host test must draw through that pair and read
  the right pixels (through winevulkan; skipped without a Vulkan device);
- the packaged `qemu-img.exe` must write a qcow2, which also proves the
  DLL closure.

Wine is not the target, so a failure there is investigated, not
believed. But a package that fails these is broken for every Windows.

## Which Direct3D 9 the executor runs on

DXVK, as everywhere (ADR-007). On Windows only, the **system's own
Direct3D 9** is the fallback for a host below DXVK's Vulkan 1.3 floor
(ADR-007's second amendment). Pre-Broadwell Intel, Kepler
and older, TeraScale all have a good D3D9 driver and will never answer
Vulkan 1.3. The alternative for them was WineD3D inside the guest. DXVK
stays the default and the only rasteriser a frame is compared against.

```sh
D3DPT_D3D9=auto      # DXVK, then the system library if DXVK opens no adapter
D3DPT_D3D9=dxvk      # DXVK or nothing
D3DPT_D3D9=system    # this PC's own d3d9.dll (%SystemRoot%\system32, by full path)
```

A machine says it as `-device d3dpt-vga,d3d9=<which>`. The machine
form's **Direct3D** row writes that, and the launcher resolves `auto`
from its own Vulkan probe, since only that side can tell a software
Vulkan device from a real one. The environment variable wins over the
property, which is how the two are compared on one host. DXVK is loaded
only as `dxvk_d3d9.dll`, never by the system's name.

**Both host tests run on both backends, and that is the check** (the
first version of this backend had no oracle and reached a user's PC
drawing black):

```sh
export D3DPT_EXEC_LIB=build/win/d3dpt/d3dpt_exec.dll
export D3DPT_DXVK_LIB="$PWD/build/win/d3dpt/dxvk_d3d9.dll"
for b in dxvk system; do
  D3DPT_D3D9=$b build/win/d3dpt-dp2-test.exe  out-dp2-$b.bmp    # the display driver's 107 checks
  D3DPT_D3D9=$b build/win/d3dpt-exec-test.exe out-exec-$b.bmp   # the guest DLLs': swapchain, scene, Present
done
```

Both must PASS, and each pair of BMPs is expected to be byte-identical
(it was on the RTX 3090). They are not the same test. The display
driver's records never present, so only the second exercises the
swapchain, the scene and the Present, which the system implementation is
strictest about.

## OpenGL for a Win98 guest

A Win98 guest's 3D is qemu-3dfx's Mesa pass-through, which needs a GL
context **inside the embed library**, where there is no window.
`embed/mglcntx_embed.c` uses EGL on Linux and CGL on macOS. On Windows
it is WGL with a `WGL_ARB_pbuffer` standing in for the window, closer to
Linux than macOS is, since Windows has a real offscreen drawable. It
works. `TESTS\GLPROBE.EXE` in a Win98 guest on the user's PC reads
`NVIDIA GeForce RTX 3090/PCIe/SSE2` through the pass-through.

- **Every ARB call borrows a context.** An ARB entry point resolved
  through libepoxy with no context current *faults* rather than failing,
  which took the first GL guest (GLQuake) and the whole process down
  (doc 12, "The WGL rule").
- **One window remains**, a 1×1 popup, created and never shown. WGL
  reaches a device's pixel formats and extension entry points only
  through a window's DC, and `wglCreatePbufferARB` takes one to name the
  device.
- **`mglcntx_mingw.c` is split, not weakened.** qemu-3dfx's own WGL
  backend stays for `qemu-system-i386.exe`, which has a window. Linux
  and macOS arrange that with weak symbols (patch 31), but a COFF weak
  external is not an ELF weak definition and leaves the emulator with
  undefined references. So the backend sits behind `MESAGL_WGL_BACKEND`,
  the helpers behind its negation, and patch 10's meson hunk compiles
  the backend half into the emulators alone. `strings` is the check. The
  emulator has the WGL backend's messages, the DLL the window-less
  one's, neither the other's.
- **`tools\wgl-probe.exe`**, shipped in the package, performs the pbuffer
  sequence with no QEMU and prints where it stops. Run it first when a
  Win98 guest gets no 3D. It resolves its own pointers with a context
  current, so it answers "will this driver give me an offscreen
  pbuffer", not "does our dispatch survive".

## Qt, which the package carries

`2ksbox.exe` is `launcher-qt` (ADR-015), so the zip carries Qt's DLLs,
the platform plugin and the QtQuick QML trees. Fedora ships
`mingw64-qt6-*` to link against and a native Qt of the same version for
the build-time tools, plus `x86_64-w64-mingw32-qmake-qt6`, which
answers `QT_INSTALL_*` with the target's paths and `QT_HOST_*` with the
host's, the split cxx-qt's cargo-only build asks for.

- `CXX_QT_AUTORCC_OPTIONS=--no-zstd` (set in the image), because the
  host `rcc` has zstd and the mingw `Qt6Core` does not, so a default
  resource does not link.
- **There is no cross `windeployqt`**, so `package-windows.sh` deploys
  Qt by hand. It copies the plugin directories (**no window without
  `plugins\platforms\qwindows.dll`**, which is in no import table), the QML
  module trees the views import, and a `qt.conf` pointing at both. The
  DLL closure walks every binary, plugins and QML modules included.
- A PE import library only satisfies symbols already undefined when the
  linker reaches it, so `launcher-qt/build.rs` names
  `-lQt6QuickControls2` again after the archive holding
  `appearance.cpp`.
- **Two emutls registries.** rustc links libgcc statically while
  `libstdc++-6.dll` uses `libgcc_s_seh-1.dll`'s, so `std::call_once` in
  cxx-qt's generated initialiser reached a `__once_proxy` reading `NULL`
  and the launcher died before `main` (`0xC0000005`).
  `launcher-qt/src/once_proxy.cpp` supplies a local proxy
  (`tools/qtmin/` has the diagnosis).

The package checks hold the Qt binary to the same standard as the rest.
`--paths` must answer and the staged launcher must open a window
offscreen. Under wine the window grab is a report rather than a verdict
(wine's Qt 6 is not the target's). The last word is `2ksbox-debug.bat`
on a real PC.

## Acceleration

Windows' hardware acceleration is **WHPX** (Windows Hypervisor
Platform), and the build has it. The launcher names it rather than
KVM. `Accel::Auto` is `whpx:tcg`, "hardware acceleration
required" is `whpx`, and the wizard's hint asks `WHvGetCapability`,
because the feature can be installed and still off (Hyper-V or WSL2 may
hold the root partition). Turn it on with:

```
dism /online /enable-feature /featurename:HypervisorPlatform /all
```

A machine with a fixed processor speed (doc 06's DOS family) is
emulated regardless.

## What is not there yet

- **Zero-copy 3D frames.** The dma-buf ring is Linux and IOSurface is
  macOS. On Windows frames take the readback path (correct, a copy per
  frame). The counterpart would be a DXGI shared handle.
- **Live control on a real PC.** The launcher's QMP socket for snapshots
  and the disc shelf is a Unix-domain socket on Windows too. QEMU binds
  `unix:`, and the launcher connects through Winsock AF_UNIX
  (`launcher-core/src/control.rs`). Wine has no AF_UNIX (`socket()`
  answers 10047), so it has never run. On the PC, `live control off: …`
  in `launcher.log` means the trial bind failed and the machine ran
  without it.
- **No Glide wrapper.** The cross build has no glide stage, so `player
  --companions` reports it "(not shipped)". Glide games on the Voodoo 2
  use 3dfx's own Glide on the emulated card.
- **No installer** beside the zip (doc 07 wants one; QEMU's
  `mingw32-nsis` recipe is within the image's reach).
- **No Windows check that boots a guest**, in the shape of
  `tools/xp-driver-test.sh`.

## Building on Windows

For debugging a fault that shows only on real Windows, the same
`scripts/build-windows.sh` runs in **MSYS2's MINGW64 shell** (its
`qemu`, `rust`, `qt`, `exec` and `guest` stages, with no container),
and `scripts/win-run.sh` runs the result out of the checkout. Every
stage builds on the user's PC and the launcher runs there. The ISO this
build makes has not yet been booted in a guest. The package stays on
Linux.

**MINGW64, not UCRT64 or CLANG64**, because it is the cross image's ABI
(msvcrt, GCC's runtime and libstdc++, Rust's `x86_64-pc-windows-gnu`). A
fault reproduced here is almost always a fault in the build that ships.
The scripts refuse the other two shells.

Once, on the PC:

```sh
# 1. MSYS2 from https://www.msys2.org, then the "MSYS2 MINGW64" shell:
pacman -Syu                                   # again if it asks to restart
pacman -S git
git config --global core.autocrlf false       # CRLF breaks every patch of the queue
cd /c && git clone --recurse-submodules --shallow-submodules https://github.com/davidrios/2ksbox
cd 2ksbox && scripts/build-windows.sh --msys2-deps

# 2. Rust, from rustup's own installer (https://rustup.rs), with the GNU host --
#    the default MSVC one needs Microsoft's linker for every build script:
./rustup-init.exe -y --default-host x86_64-pc-windows-gnu
echo 'export PATH="$(cygpath "$USERPROFILE")/.cargo/bin:$PATH"' >> ~/.bashrc && . ~/.bashrc

# 3. Open Watcom, for the Win98 display driver and GLIDE2X.OVL: the same
#    ow-snapshot.tar.xz as on Linux (open-watcom-v2's Last-CI-build release),
#    whose Windows binaries are in binnt64:
mkdir -p /c/WATCOM && tar -C /c/WATCOM -xf ow-snapshot.tar.xz
echo 'export WATCOM=/c/WATCOM' >> ~/.bashrc && . ~/.bashrc
```

Then, as often as needed:

```sh
scripts/build-windows.sh                  # qemu rust qt exec, and the ISO if there is none
scripts/build-windows.sh rust             # one stage
scripts/build-windows.sh guest            # the ISO again, after a driver change
scripts/win-run.sh launcher               # the Qt launcher, out of the checkout
GDB=1 scripts/win-run.sh player ...       # the player under gdb
```

`scripts/win-run.sh` does what a package does by being one folder. It
puts `build/win/qemu` on `PATH` for the embed DLL, names the player to
the launcher, and names the executor and DXVK to QEMU (DXVK copied to
`dxvk_d3d9.dll`). The launcher writes nothing to the terminal, so paste the
`[player] …` line from `launcher.log` after `GDB=1 scripts/win-run.sh
player`.

What differs from the cross build, and why:

- **Python is MSYS2's own 3.14**, with `python-distlib`. MSYS2 has no
  older one, and a python.org interpreter makes a venv with `Scripts\`
  where configure looks for `bin/`. With the real `distlib` QEMU 9.2
  configures and builds on 3.14, and `configure-qemu.sh` accepts 3.14
  only then. Patch 69 fixes the `file://C:/…` wheels URL mkvenv gives
  pip on Windows (a host named `C:`).
- **Optional libraries are pinned off.** MSYS2 with Qt has zstd, gnutls
  and others the cross image lacks, and QEMU links what it detects, so
  native `configure-qemu.sh` disables each one the cross summary says NO
  to.
- **lld is named through meson's `CC_LD`** rather than
  `clang-mingw-cc`, because a native meson cannot run a shell script as a
  compiler. Same clang, linker and flags.
- **Paths in meson's files are `C:/…`** (`cygpath -m`), for native
  compilers. `qemu-embed/build.rs` strips `canonicalize`'s `\\?\`
  prefix, because the linker appends `/libqemu-embed-…` and `/` is not a
  separator in a verbatim path.
- **Qt's tools run from `build/win/qt-host`.** qt-build-utils starts
  moc, rcc, qmltyperegistrar and qmlcachegen with an *empty*
  environment, and MSYS2 installs them in `share/qt6/bin`, away from
  their DLLs, so none starts ("moc unexpectedly exited"). The `qt` stage
  copies each tool beside its DLLs (found with `ldd`) and sets `QMAKE` to
  `packaging/windows/qmake-host.c`, a static wrapper that answers the
  tool-directory queries with that folder and passes the rest to
  `qmake6`. It then runs each tool with an empty environment itself and
  prints exit codes and output on failure (qt-build-utils only says
  "could not find Qt").
- **GCC 16 has no `std::__once_call` to borrow.** Its libstdc++ is built
  with `_GLIBCXX_NO_EXTERN_THREAD_LOCAL` and reaches `call_once`'s
  callable through functions inside the DLL, so the pre-`main` fault
  cannot happen and `once_proxy.cpp` compiles only where that macro is
  absent. The package still carries it.
- **Package versions follow MSYS2** (Qt 6.11 against Fedora's 6.10, GCC
  16 against 15). A difference that matters shows up as a fault in one
  and not the other, so the zip remains the verdict.
- `prepare-qemu.sh` runs every git through `qgit`, which retries on
  `Unable to create index.lock: File exists`, a scanner holding the lock
  of the git that just exited (00-status, "Building").

**The guest-tools ISO** is built by the same scripts as on Linux
(`build-wrappers.sh`, `build-driver.sh`, `build-driver9x.sh`), each of
which sources `guest-tools/msys2-i686.sh` first. That file is the whole
port:

- qemu-3dfx's `conf_wrapper` builds its wrappers natively only with
  `MSYSTEM=MINGW32` and an i686 `gcc`, so the file sets `MSYSTEM=MINGW32`
  and puts `/mingw32/bin` first on `PATH` (the x86_64 python3 and gendef
  behind it), and adds shims for the target-prefixed binutils our
  scripts call (`i686-w64-mingw32-objdump`, `-nm`, `-ar`, `-windres`).
  conf_wrapper writes a plain `gcc` into its Makefiles, so
  `build-wrappers.sh` gives plain `gcc` the same msvcrt and
  `-march=pentium3` flags.
- **It links Linux's i686 runtime, not MSYS2's.** MSYS2 builds its
  32-bit runtime for the Pentium 4, so its printf/dtoa, libgcc's
  double-to-unsigned conversion, msvcrt's stat and time helpers and
  libwinpthread all carry SSE2, and nearly every guest program linked
  with them fails the ISO's Pentium III check. The first run downloads
  Arch's `mingw-w64-crt` and `-winpthreads` 14.0.0-1 and the i686 libgcc
  of `mingw-w64-gcc` 16.2.0-2, pinned by sha256, into
  `guest-tools/tools/i686-runtime/`. The i686 `gcc` shims link against
  them with `-B`/`-L`, and headers stay MSYS2's. MSYS2's i686 gcc must be
  16.2.0, or the script says to re-pin.
- Open Watcom runs from `binnt64`. `build-wrappers.sh` prepares
  OpenGLide's Glide SDK header itself when it finds it unpatched, and
  `GLIDE2X.OVL` needs `qemu/hw/3dfx` from the `qemu` stage's
  `prepare-qemu.sh` (its absence is named).
- Paths need no conversion. MSYS2 rewrites `/c/…` arguments and
  colon-separated lists in the environment (Watcom's `INCLUDE`) for a
  native program. Only Watcom's `@file` gets `cygpath -m`.
- QEMU's seven symbolic links are in Linux-only subprojects that are
  never built, so a checkout without symlink support is fine.

## Running it there

Unzip anywhere and run `2ksbox.exe`. The user's data (machines,
`discs.toml`, shader profiles, a downloaded preset collection) lives in
`%APPDATA%\2ksbox\data`, the same content Linux keeps in
`~/.local/share/2ksbox`. When something misbehaves, run
`2ksbox-debug.bat` and send `2ksbox-debug.log`. `PLAYER_KEYBOARD_LOG=1`
adds the keyboard capture's decisions to `player.log`.
