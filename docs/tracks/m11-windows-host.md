# Track: M11 — the Windows host (build, package, run)

The handoff for a session working on Windows as a *host*: cross-building
the whole stack from Linux, packaging it, and closing the gaps a Windows
host has that Linux and macOS do not. Read `docs/00-status.md` first for
the global picture and the track rules, then this file, then
`docs/build-windows.md`, which is the prose version of the build.

Opened 2026-09-06 on `track/m11-windows-host` (worktree
`.claude/worktrees/m11-windows-host`), because the user wants to see what
2ksbox is like on Windows and the only machine that builds is this Linux
one. Windows had been "untested" since M1 (doc 08).

## Scope and files (this track owns them)

- The cross toolchain: `packaging/windows/Dockerfile`,
  `scripts/win-cross.sh`.
- The Qt reproducer: `tools/qtmin/` (its README is the recipe).
- The build: `scripts/build-windows.sh`, the `--windows` mode of
  `scripts/configure-qemu.sh` and of `scripts/build-d3dpt-exec.sh`.
- The package: `scripts/package-windows.sh`.
- Windows branches of shared code: `embed/mglcntx_embed.c` (the WGL
  backend) and `tools/wgl-probe.c`, `launcher-core/src/console.rs`,
  `launcher-core/src/fatal.rs`,
  `player/src/qmp.rs`,
  `launcher/src/paths.rs` + `player.rs` + `wizard.rs` + `bundle.rs`
  (the layout and the WHPX naming), `d3dpt/hw/d3dpt_exec_load.c`,
  `d3dpt/exec/*.cpp`, `libdisc/src/bin/discx.rs`.
- Docs: `docs/build-windows.md`, this file, the M11 rows of
  `docs/00-status.md` and doc 08.
- Shared with other tracks (rebase first, edit minimally, say so in the
  commit): `patches/qemu/13-perfmap-darwin.patch`,
  `31-mesa-ctx-weak.patch` and `50-cdimage-block-driver.patch`,
  `scripts/configure-qemu.sh`, `qemu-embed/build.rs`, and the shared
  layer of `embed/mglcntx_embed.c` (M3's file).

## State (2026-09-06; the Qt package built 2026-09-08)

The whole stack cross-builds and packages. **The first cross build since
ADR-015 made `2ksbox.exe` the Qt launcher ran 2026-09-08**, from a
worktree that had no `build/win` of its own (a Windows build belongs to
its checkout like every other): `qemu rust qt exec` in 424 s, then
`package-windows.sh` green — 412 MB staged, a **146 MB zip** (the older
~92 MB figure below is the egui launcher's, before Qt's runtime, plugins
and QML trees came along). The staged launcher answered `--paths` and
wrote its `launcher.log`, the packaged `qemu-img.exe` wrote a qcow2, and
`wgl-probe.exe` drew its frame. The one thing wine still will not do is
open the Qt window offscreen — a report, not a verdict, and the real
answer is `2ksbox-debug.bat` on the PC.

That run also added the check the Linux packages needed
(2026-09-07): the staged **player** must answer `--companions` with the
libraries QEMU `LoadLibrary`s by name rather than through an import
table. On Windows that is `d3dpt_exec.dll` — staged, and resolved inside
the package — with `glide` and `dxvk` legitimately "(not shipped)": there
is no Glide wrapper for Windows yet (M3: the cross build has no glide
stage), and DXVK is not built there because the host has a real
Direct3D 9 and a `d3d9.dll` beside the player would override it.

`scripts/build-windows.sh`
then `scripts/package-windows.sh` produce a zip holding
`2ksbox.exe`, `2ksbox-player.exe`, `qemu-img.exe`,
`libqemu-embed-i386.dll` (all 20 embed API entry points exported),
`d3dpt_exec.dll`, the mingw runtime closure, `pc-bios\`, the guest-tools
ISO and `tools\wgl-probe.exe`. **WHPX is detected and built in**, and the
embed library has a WGL backend, so a Win98 guest's OpenGL has somewhere
to go.

What has actually been *run*, all under wine on the build host:

- `qemu-system-i386.exe -version` prints 9.2.4 with the qemu-3dfx
  signature; `-M pc -accel tcg -m 64 -display none -qmp stdio` starts the
  machine, negotiates QMP, answers and quits cleanly.
- The staged launcher answers `--paths` with every companion inside the
  package, from outside the checkout with an empty environment, and its
  `--wizard-new` runs the packaged `qemu-img.exe` to create a real
  qcow2 — which is also the DLL closure's proof.
- The player's own window does **not** get that far: it initialises
  Vulkan through wine's winevulkan (radv, "not a conformant
  implementation") and then hangs before the first frame. Not chased:
  wine is not the target, and the answer costs less on a real Windows
  machine than in a wine debugger.

### First run on real Windows (2026-09-06)

The user ran the package on their PC. It starts, and two things were
wrong — both of them things only a real Windows could show:

1. **A terminal window.** The launcher was a console-subsystem binary, so
   double-clicking it opened a black console and kept it for the session.
   Both front ends are `windows_subsystem = "windows"` now, with
   `launcher-core/src/console.rs` paying back what that costs: the debug
   verbs borrow the console they were launched from when there is one
   (`AttachConsole(ATTACH_PARENT_PROCESS)`, and only when nothing has
   already handed us a stdout — a redirection or a test harness's pipe
   must be left alone), and every subprocess the launcher starts is given
   `CREATE_NO_WINDOW`, so `qemu-img` no longer flashes a console per call
   and the player no longer sits behind one. Verified under wine both
   ways: `package-windows.sh`'s `--paths` check still reads its pipe, and
   `2ksbox.exe --host-check` from a `.bat` under `cmd` prints into that
   console.
2. **"Failed loading SDL3 library" on Play.** Fedora's `mingw64-SDL2` is
   *sdl2-compat*: an `SDL2.dll` that `LoadLibrary`s `SDL3.dll`. The DLL
   closure walks import tables, and a runtime load is not in one, so
   SDL3 was never shipped and the player could not start on a machine
   without its own. `package-windows.sh` now has a second pass over the
   staged binaries' strings for sysroot DLL names that are not staged
   yet (`SDL3.dll`, and ANGLE's `libEGL`/`libGLESv2`).
   **Since 2026-09-07 neither SDL DLL is staged at all:** QEMU is
   configured `--disable-sdl`, because nothing we ship opens a QEMU
   window. `libEGL`/`libGLESv2` are still there — `libepoxy-0.dll`, the
   Mesa pass-through's GL loader, names them at run time, and the same
   pass catches that. It stays as the net for the next one.

A windowless launcher has nowhere to put the player's output, which is
precisely when a start-up failure needs reading, so `player::spawn`
redirects it to `%APPDATA%\2ksbox\data\player.log` in that case —
appended, with a header per run.

Still open from that first run: whether a machine actually boots, and
what WHPX does with it.

### The QMP monitor's `fd=` on Windows (2026-09-06)

The next run got as far as starting a machine and QEMU refused the
command line: `-chardev socket,id=qmp0,fd=7368: File descriptor '7368' is
not a socket`. 7368 is a `SOCKET` handle, and `fd=` on Windows is not one.
Every socket call in QEMU's Windows build is an `os-win32.h` wrapper
(`#define getsockopt qemu_getsockopt_wrap`) whose first line is
`_get_osfhandle(fd)`, so the number has to be a **C-runtime descriptor**
— and `util/qemu-sockets.c`'s `fd_is_socket()` is the getsockopt that
fails on a raw handle.

`_open_osfhandle()` makes that descriptor, but *whose* table it lands in
depends on which CRT the module linking it uses, and the player cannot
know that about the library it loads (both are msvcrt.dll today; nothing
enforces it). So the handle crosses the boundary as a handle and the
library converts: **`qemu_embed_socket_to_fd()`, embed API v7**. On Unix
an fd is already an fd and it returns the value unchanged, so
`player/src/qmp.rs` has one `into_raw` shape on both platforms.

Anyone pulling this must rebuild the embed library before the player
links (the API version moved).

### The third run: nothing at all (2026-09-06)

The report was "didn't work on windows, didn't start, no error messages
nor anything". That sentence is a bug in this track, not a fact about the
user's machine: since the first run's fix both front ends are
`windows_subsystem = "windows"`, and a windowed program has no stderr. A
panic on the way to the first window — or an `eframe::run_native` that
returns `Err` — printed into nothing and the process vanished. The
terminal that was removed took with it the only place a start-up failure
could be read.

So the launcher has two mouths now that need no toolkit
(`launcher-core/src/fatal.rs`, called from the first line of both
`main`s):

- **A log**, `%APPDATA%\2ksbox\data\launcher.log`, beside the
  `player.log` that was already there: a header per run, then one
  milestone per start-up step (`exe`, `data dir`, `library: N machines`,
  `opening the window`). The *last* line in the file is the answer — a
  run that ends after `data dir` died loading the library — so no
  particular failure had to be guessed at in advance.
- **A message box**, because someone who double-clicked an icon will not
  go looking for a log file. The panic hook writes the message, the
  location and a backtrace to the log and then says so in a
  `MessageBoxW`; `run_native`'s `Err` goes the same way.

Then the case no Rust of ours can catch: a process that dies in the
**loader** (a DLL that is not there) or in a **static initialiser** never
reaches `main` and writes no log at all. So the package carries
**`2ksbox-debug.bat`**, which runs the launcher from a console and keeps
what it says in `2ksbox-debug.log`. Two things it has to do that are not
obvious: every run goes through `start "" /b /wait`, because cmd does not
wait for a windows-subsystem program and would otherwise record no exit
code; and the launcher's *answers* come out of its own log rather than
that console, because `start /b` does not hand the child the console's
redirection — which is what `--diagnose` is for (`--paths` and
`--host-check`, filed into the log instead of printed into nowhere).

**The absence of `launcher.log` is itself the reading**: nothing of ours
ran, and the exit code beside it names the loader failure (`0xC0000135` a
missing DLL, `0xC0000142` an initialiser that failed, `0xC0000005` a
fault — which is what the Qt binary does under wine). One .bat therefore
asks both packages the same question and tells the two cases apart.

Verified under wine: the packaged `2ksbox.exe` with no display reaches
`[start] opening the window` and stops there — the winevulkan hang from
the first run — which is the log doing exactly its job; and the panic
hook's message, location and backtrace land in the log on the native
build.

### The fourth run: two answers at once (2026-09-06)

`2ksbox-debug.bat` did its job on the user's PC the first time it was
asked, and both halves of the report were new facts.

**The Qt front end dies before `main` on real Windows too**: exit
`-1073741819` = `0xC0000005`, an access violation, and no `launcher.log`
at all. So it is not wine — the loader completes there as it does here
and a static initialiser faults, which makes it **ours to fix** and not
a question about wine's Qt 6 support. (What has been ruled out so far: a
CRT mismatch — every binary in the package imports `msvcrt.dll`, Qt's
included — and a malformed constructor list: the exe's `.ctors` is a
well-formed `-1`, fifteen entries inside `.text`, `NULL`.) The egui
package is the one to use meanwhile. **Found and fixed the same day** —
`__once_proxy` across the libstdc++ DLL boundary, "The Qt binary's
fault, found" below — and since ADR-015 (2026-09-07) the Qt build is the
only package there is.

**The egui front end started, and the machine it started did not**: the
player's log said

    network backend 'user' is not compiled into this binary

`-netdev user` is a *compiled-in* backend — it is libslirp — and Fedora
has no `mingw64-libslirp`, so the Windows QEMU had been built without it
since the first day of this track (`slirp support: NO` in the configure
summary, which nobody had read). The cross image now builds libslirp
4.9.4 from source into the mingw sysroot, the same release the Flatpak
manifest pins for the same reason, and the DLL closure picks
`libslirp-0.dll` up on its own because QEMU imports it.

Every check in `package-windows.sh` was green while this was broken,
because none of them had ever asked our QEMU for anything the *launcher*
writes: the wine checks start `-M pc`, and machines come from
`bundle.rs`. There is a check for it now, and it is a static one —
the package holds no `qemu-system-*.exe` (QEMU is in-process, inside
`libqemu-embed-i386.dll`) and the player that would load it is the
binary wine hangs in, so the question is put to the embed library's
**import table**: `net/slirp.c` is libslirp's only consumer, so the
import is the backend.

### Both front ends (2026-09-06) — one package since ADR-015 (2026-09-07)

`scripts/package-windows.sh --qt` rolled a second, complete package whose
`2ksbox.exe` was `launcher-qt`, so the two could be unzipped side by side
on one machine and compared. That flag is gone: the Qt build is the
launcher (ADR-015), the one zip carries it and Qt, and `launcher.exe` is
still cross-built by the `rust` stage for anyone who wants a second
opinion out of `target/x86_64-pc-windows-gnu/release`. Qt crosses better
than expected: Fedora has
`mingw64-qt6-*` to link against, a native Qt of the same version for the
tools that run here, and a `x86_64-w64-mingw32-qmake-qt6` whose `-query`
splits `QT_INSTALL_*` (target) from `QT_HOST_*` (host) exactly the way
cxx-qt's cargo-only build wants. Two things had to be said:

- `CXX_QT_AUTORCC_OPTIONS=--no-zstd` (a supported cxx-qt env var, found
  after nearly wrapping `rcc` by hand): the host rcc has zstd and the
  mingw Qt6Core does not, so the default algorithm produces a resource
  that asks the target for `qResourceFeatureZstd()` and will not link.
- Qt deployment by hand, since Fedora has no cross `windeployqt`: the
  plugin directories, the QML module trees, and a `qt.conf` so both
  resolve relative to the executable. The DLL closure had to grow roots:
  a platform plugin or a QML module's DLL sits in a subdirectory and
  imports half of Qt, and nothing above it names either — it now walks
  every binary anywhere in the package.

### The Qt binary's fault, found (2026-09-06)

`tools/qtmin/` is the smallest cxx-qt program that can be cross-built
here, in three rungs — no bridge, one bridge, a QML module — so that the
rung which stops printing `qtmin: main` names the layer. **Rung 1 already
faults**: linking `cxx-qt-lib` is enough, and neither the QML
registration nor the bridge nor a line of ours is involved.

Read out of the crash with `wine winedbg` and `objdump`:

1. `_GLOBAL__sub_I_call_initializers.cpp`, a static initialiser in the
   C++ `cxx-qt-build` generates, calls `cxx_qt_init_crate_cxx_qt`;
2. which uses `std::call_once`, and on mingw-w64 that parks the callable
   in `std::__once_call` — a `__thread` variable reached through
   **emulated TLS** — and asks `pthread_once` to run `__once_proxy`;
3. `__once_proxy` is in `libstdc++-6.dll`, and its import is bound
   correctly (the thunk really does land in the DLL — checked in the
   debugger, not assumed). It reads `__once_call` back through emutls;
4. but emutls keys its storage per *libgcc*, and there are two: the exe
   links libgcc statically — rustc does that for `x86_64-pc-windows-gnu`
   and neither `-C link-self-contained=no` nor `-shared-libgcc` moves it
   — while `libstdc++-6.dll` uses `libgcc_s_seh-1.dll`'s. The proxy
   reads a slot the exe never wrote, finds `NULL`, and calls it.

Which is exactly the `rip=0` with a return address inside `pthread_once`
that every dump of `launcher-qt.exe` shows, on wine and on the user's PC.

**Fixed the same day**, in eleven lines
(`launcher-qt/src/once_proxy.cpp`, and the same file in the reproducer):

```cpp
namespace std { extern __thread void (*__once_call)(); }
extern "C" void __once_proxy() { std::__once_call(); }
```

A local definition is one the linker prefers over an import, so the proxy
that runs is ours, and it reads `__once_call` through *this* module's
emutls — the registry `call_once` just wrote it to. It is exactly what
libstdc++'s own proxy does (`src/c++11/mutex.cc`): the function exists
only to be a plain `void()` whose address `pthread_once` can take. It
compiles to nothing off Windows, where the C++ runtime is one module.

All three rungs of `qtmin` reach `main` with it, and **the Qt package
passes every check the egui one does** — `--paths` from inside the
package, its own `launcher.log`, and the packaged `qemu-img` driven
through the wizard, all under wine. The three excuses
`package-windows.sh` used to make for `--qt` are gone with the bug, so a
Qt package that cannot do those things fails now.

What did *not* work, for the next person who meets this: `-C
link-arg=-static-libstdc++` (something on the link line still asks for
the DLL, and the exe keeps importing `__once_proxy`),
`-C link-self-contained=no`, and `-C link-arg=-shared-libgcc` — rustc
links libgcc statically for this target regardless, so there are still
two emutls registries. This fix makes that harmless rather than arguing
with it.

**The Qt binary did not start at all** — on wine or on the user's PC,
where it exited `0xC0000005` — until the `std::call_once` fix above. It
faulted on a call to address 0 before `main` ran, with either subsystem,
while the egui binary in the same folder with the same DLLs answered
`--paths` fine. Two things narrowed it before `tools/qtmin` named it:
with `WINEDEBUG=+loaddll` the loader gets **all the way through** the DLL
set — `Qt6Core`, `Qt6Gui`, `Qt6Network`, `Qt6Qml` and the system
libraries after them — and only then faults, so nothing was missing; and
`launcher.log` was **never created**, although the first statement in
that binary's `main` is what opens it (`fatal::install("qt")`), so the
process never reached `main`.

### Dark mode, and half a theme (2026-09-06)

The Qt launcher runs on the user's PC now, and the first thing it showed
is that a Windows set to dark mode got it "all mixed up between dark and
light". Reproduced here by forcing a dark palette on the launcher and
grabbing a frame: the window and its labels go dark and every *control*
stays light, because a Quick Controls style paints its buttons, fields
and combo boxes from its own colours and only the surfaces around them
come from the palette.

So the launcher is a light-mode application, on every platform and
whatever the desktop says (`launcher-qt/src/appearance.cpp`): the colour
scheme is requested *and* a matching palette is handed over, because
`setColorScheme` is only a request — Windows honours it, this checkout's
Wayland session and the offscreen plugin do not, and the palette is what
the controls actually read. `LAUNCHER_QT_SCHEME=system` gives the
desktop's own back, `=dark` forces the other one.

The style is left alone where a platform has a real one. Asking
`QQuickStyle::name()` is what makes Qt resolve a style — the environment
variable, then a config file, then the platform's own — so Windows has
already answered "Windows" by the time we ask and keeps it; only the
platforms whose default is "Basic" (no system colours at all) are given
Fusion. The start-up log now carries the line that settles all of this:

    [start] style Windows, scheme light, window #efefef, base #ffffff

Two things that cost a build each: `QQuickStyle` needs
`.qt_module("QuickControls2")`, and on Windows that is not enough — the
generated archive holding `appearance.cpp` is linked *after* the Qt
import libraries, and a PE import library only satisfies symbols already
undefined when the linker walks past it, so `build.rs` names
`-lQt6QuickControls2` again as a `rustc-link-arg`. On ELF the same code
links either way, which is why it built here and not there.

### OpenGL in the embed library

Written the same day: `embed/mglcntx_embed.c` grew a Windows branch using
WGL with a `WGL_ARB_pbuffer` as the drawable — the Linux backend's shape
(the pbuffer is FBO 0, the swap is a readback of its back buffer), not
the macOS one's FBO stand-in, because Windows has a real offscreen
drawable and macOS does not. One 1×1 window is created and never shown,
because WGL cannot reach a device's pixel formats or its ARB entry points
without one. Everything goes through epoxy, which the embed library
already links.

Three things this needed elsewhere:

- **The weak-symbol trick does not work on Windows.** Marking
  `hw/mesa/mglcntx_mingw.c`'s entry points weak the way patch 31 does for
  GLX links the DLL fine and then leaves `qemu-system-i386.exe`
  with `undefined reference to MGLCreateContext` — a COFF weak external
  is not an ELF weak definition, and ld.bfd does not fall back to the
  aliased body (the object's own calls to its own weak symbols are
  undefined too). The file is split instead: its WGL backend behind
  `MESAGL_WGL_BACKEND`, its platform-independent helpers behind the
  negation, and patch 10's meson hunk compiles it once more — backend
  half only — into the emulators alone, so the archive both consumers
  share holds the helpers and no backend. `strings` says it worked: the
  emulator carries the WGL backend's messages and the DLL the
  window-less one's, neither the other's.
- The shared WGL layer at the top of `mglcntx_embed.c` *defines*
  `PIXELFORMATDESCRIPTOR`, `WORD`, `DWORD`, `BYTE` because Linux and
  macOS have no `windows.h`; on Windows those give way to the real ones.
- `HPBUFFERARB` in that layer is a record of what the guest asked for,
  not a handle, and on Windows the name belongs to `wglext.h` — it is
  now `FakePBuffer`, which is what it always was.

`tools/wgl-probe.c` is the test: the same sequence without QEMU, drawing
the frame `tools/embed-3d-test.c` draws and checking the same three
pixels. It passes under wine on this host (Mesa 26.2, RX 9060 XT), which
is what says the sequence itself is right; it ships in the package as
`tools\wgl-probe.exe` so the same question can be asked on the machine
where a guest's 3D actually fails.

### The six things that were in the way

1. **`tcg/perf.c`** — patch 13 compiles it on every host, and half of it
   (jitdump) needs `mmap` and `flockfile`. The perfmap half is plain
   stdio and stays; `-jitdump` on Windows says it is unavailable.
2. **libdisc's link libraries** are per-platform and meson forbids a
   chained ternary, so patch 50's `link_args` became an `if/elif`
   (`-lkernel32 -lntdll -luserenv -lws2_32 -ldbghelp`, from
   `rustc --print native-static-libs`).
3. **`d3dpt_exec_load.c`** dlopens the executor: `LoadLibrary` /
   `GetProcAddress` behind three macros.
4. **The player's QMP monitor is a socketpair**, which Windows has not
   got. A loopback pair stands in, and the accepted connection is proved
   to be our own (peer address == our connecting socket's own address)
   before the monitor is handed to it — otherwise a local process that
   raced for the port would be handed a QMP monitor. QEMU's `fd=` is a
   plain integer on both platforms.
5. **The executor** compiled almost unchanged against mingw's own
   `<windows.h>` / `<d3d9.h>` instead of DXVK's stand-ins for them (x86-64
   has one calling convention, so the COM ABI is the same). It loads
   plain `d3d9.dll` at run time: the system implementation, or a DXVK
   build dropped next to the player, which the loader prefers because it
   searches the executable's directory first. `access()` became a
   `fopen` probe and `%zu` needs `__USE_MINGW_ANSI_STDIO`.
6. **The package layout.** A Unix prefix's `bin`/`lib`/`libexec`/`share`
   split is wrong on Windows, where the loader wants the DLLs beside the
   exe and the user wants one folder. `paths.rs` now knows both shapes;
   the strings at the call sites did not change.

## Build / test loop

```sh
scripts/win-cross.sh --build          # once
scripts/build-windows.sh              # qemu, rust, exec, guest
scripts/package-windows.sh            # zip + the wine checks
scripts/build-windows.sh rust         # the inner loop while changing Rust
```

`scripts/test.sh` is unchanged and stays a Linux suite: it needs guest
images and a GPU, and now a Windows host too. The Windows evidence is
`package-windows.sh`'s own checks.

## Next steps, in order

1. **Rebuild and re-package for the ADR-015 shape**, then
   `2ksbox-debug.bat` on the PC and read the `2ksbox-debug.log` it
   writes: there is one zip now, its `2ksbox.exe` is the Qt launcher, and
   nothing about that path has been run since the packager changed. A
   `launcher.log` with milestones names the step that failed; no
   `launcher.log` at all plus an exit code names a loader failure.
2. **Does the window come up on Windows?** It starts and does real work
   under wine, and the packaging check grabs one offscreen there — but
   wine's Qt 6 is not the target's, and only that machine can say.
3. **Boot a machine on the user's Windows PC.** The QMP monitor's `fd=`
   is a CRT descriptor and the QEMU beside it has libslirp now, which is
   where runs two and four stopped; what a guest does under WHPX is the
   next unknown.
4. **A Win98 guest with 3D on real Windows.** The WGL backend is written
   and its sequence passes under wine (`tools/wgl-probe.exe`), but no
   guest has used it.
5. **The installer.** Doc 07 wants an installer as well as the portable
   zip. QEMU's own `mingw32-nsis` recipe is in the cross image's reach.
6. **Zero-copy frames** through a DXGI shared handle, the Windows answer
   to the dma-buf ring and IOSurface.
7. **A second Windows check that boots a guest**, once (1) says what
   actually happens. The shape to aim for is `xp-driver-test.sh`'s: drive
   the machine over QMP, pull the artefacts out, diff a frame.

## Gotchas found here

- **Fedora has no `mingw64-libslirp`**, and QEMU's `-netdev user` is a
  compiled-in backend rather than a plugin: without it every machine the
  launcher writes fails at start-up with "network backend 'user' is not
  compiled into this binary". The cross image builds it from source. The
  configure summary says `slirp support: NO` and nothing else does.

- Fedora's default Python breaks QEMU's `mkvenv` (3.14 vs 3.8–3.13); the
  image pins 3.13 and installs a real `distlib` for it.
- `scripts/win-cross.sh` forwards a **whitelist** of environment
  variables into the container. A knob that seems to be ignored is
  probably not on it.
- A `wine` command whose output goes through a pipe can look like a hang;
  redirect to a file.
- A kept copy of the cross image's DLLs (`build/win/sysroot-bin`) is a
  snapshot of an older image: the first `--qt` package took one from
  before Qt was installed and shipped a Qt launcher with no `Qt6Core.dll`.
  Both sysroot copies are refreshed on every packaging run now.
- Running the packaged binaries under wine with a display attached puts
  wine's crash dialog **on the user's desktop** when one of them faults.
  Unset `DISPLAY`/`WAYLAND_DISPLAY` for anything that might crash.
- The launcher's Windows data directory is `%APPDATA%\2ksbox\data`
  (`directories`' own convention), not `%APPDATA%\2ksbox`.
- A DLL loaded with `LoadLibrary` is not in any import table, so a
  closure walked from those alone is not a closure. Fedora's SDL2 was
  sdl2-compat and loaded SDL3 that way (SDL is no longer built or staged);
  `package-windows.sh` keeps the strings-based second pass for the next one.
- **cmd does not wait for a windows-subsystem program**, and `start ""
  /b /wait` (which does) does not pass the console's redirection to the
  child: a `.bat` that runs a windowed program gets neither its output
  nor its exit code by writing the obvious thing. Take the exit code
  from `start /b /wait` and the output from a file the program writes
  itself (`--diagnose`).
- `windows_subsystem = "windows"` is what stops the terminal window, and
  it costs a console for the debug verbs and a place to put a child's
  output. Both are in `launcher-core/src/console.rs`; neither is
  optional once the attribute is set. It costs a third thing that took
  a whole run on the user's PC to notice: there is nowhere for a
  *failure* to go either, so `launcher-core/src/fatal.rs` is not
  optional either.
