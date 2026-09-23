# Building and testing on macOS (Apple Silicon)

Everything here runs natively on arm64; the test machine is an M1
MacBook Air. This doc covers setting a Mac up, building, running a
guest, the app and its two builds (ADR-019), and the macOS floor they
target. The stages themselves are in `docs/development.md`; Mac traps
that cut across subsystems are in `docs/00-status.md` ("Running on a
Mac").

## One-time setup

```sh
xcode-select --install                       # Apple clang + git
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
brew install ninja meson pkg-config gnu-sed uv   # build tools only ("The libraries" below)
brew install qt                              # Qt 6: the launcher, and macdeployqt
brew install mingw-w64 xorriso nasm mtools   # guest-tools ISO, the Wine pair, the DOS batteries
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

- **Qt 6** builds `launcher-qt` and brings `macdeployqt`. Without it
  `build.sh` skips its `qt` stage and no package can be made. It is the
  last Homebrew library the app carries ("The libraries" below).
- **QEMU's libraries are not Homebrew's.** `build.sh`'s `deps` stage
  (`scripts/build-deps.sh`) builds glib, pixman, libslirp and zstd from
  pinned upstream tarballs into `build/deps/<arch>`, static, and
  `configure-qemu.sh` lets pkg-config see nothing else. meson, ninja and
  pkg-config are needed to build them and ship nothing.
- **gnu-sed**: qemu-3dfx's `sign_commit` uses GNU `sed -i`, so
  `prepare-qemu.sh` puts gnu-sed's `gnubin` first on `PATH`.
- **No XQuartz and no SDL2.** QEMU has no display of its own
  (`--disable-sdl --disable-cocoa …`, patch 02), and patch 70 replaces
  qemu-3dfx's GLX backend on Darwin with one that only refuses; 3D is
  the player's CGL backend.
- `GL/glcorearb.h` is vendored in `third_party/khronos`, so the build
  never depends on the Mac's headers.

### Vulkan for the Direct3D executor

DXVK's d3d9 (ADR-007) runs over Mesa's **KosmicKrisp**, which needs
macOS 26 and is an optional component of the LunarG SDK. DXVK refuses
MoltenVK (`docs/spikes/spike-c-dxvk-native-macos.md`).

```sh
brew install vulkan-headers vulkan-loader vulkan-tools glslang
curl -sSL -o vulkan-sdk.zip https://sdk.lunarg.com/sdk/download/latest/mac/vulkan-sdk.zip
unzip -q vulkan-sdk.zip                      # vulkansdk-macOS-<ver>.app
V=1.4.357.1                                  # the version the zip carried
vulkansdk-macOS-$V.app/Contents/MacOS/vulkansdk-macOS-$V --root ~/VulkanSDK/$V \
  --accept-licenses --default-answer --confirm-command install
~/VulkanSDK/$V/MaintenanceTool.app/Contents/MacOS/MaintenanceTool \
  --accept-licenses --default-answer --confirm-command install com.lunarg.vulkan.kosmic
export VK_ICD_FILENAMES=~/VulkanSDK/$V/macOS/share/vulkan/icd.d/libkosmickrisp_icd.json
vulkaninfo --summary | grep -E "driverName|apiVersion"   # KosmicKrisp, 1.4.x
```

The installer will not add a component to an existing root, hence the
maintenance tool. SIP strips a `DYLD_*` variable exported to a
`#!/usr/bin/env bash` script (symptom: `Direct3DCreate9 failed`), so
`scripts/test.sh` sets the Vulkan environment itself on Darwin:
Homebrew's loader on `DYLD_LIBRARY_PATH`, and the SDK's KosmicKrisp ICD
unless `VK_ICD_FILENAMES` is set. Put **only**
`/opt/homebrew/opt/vulkan-loader/lib` on `DYLD_LIBRARY_PATH`, never
`/opt/homebrew/lib` (every image decode dies with `SIGBUS`; 00-status),
including in the shell a launcher starts from.

### Open Watcom, for the Win98 display driver

The 16-bit `.drv` and the ring-0 `.vxd` (doc 19) build with Open
Watcom. Its snapshot has an arm64 macOS set (`armo64`) beside
Linux's `binl64`; `build-driver9x.sh` picks by `uname`.

```sh
curl -L -o ow.tar.xz https://github.com/open-watcom/open-watcom-v2/releases/download/Last-CI-build/ow-snapshot.tar.xz
mkdir -p ~/.local/opt/open-watcom && tar xJf ow.tar.xz -C ~/.local/opt/open-watcom
```

Set `WATCOM=` if it lives elsewhere. The Mac's `d3dpt9v.vxd` is
byte-identical to Linux's; `d3dpt9x.drv` differs by one instruction
selection with the same semantics. `wdis` segfaults on our 16-bit
objects on both hosts.

## Building

```sh
git clone --recurse-submodules --shallow-submodules git@github.com:davidrios/2ksbox.git
cd 2ksbox
scripts/build.sh            # QEMU ~10–15 min on the Air, the Rust side ~1 min
target/release/player       # the test pattern, through wgpu on Metal
```

The pattern is colour bars, a 1-px white border and a white line
sweeping down (≈ 8 s a pass): check sharp edges, integer-scaled 4:3 and
no tearing, also after a resize.

Mac specifics of the stages:

- **Everything targets the floor** (below). `build.sh` exports
  `MACOSX_DEPLOYMENT_TARGET=$(scripts/macos-floor.sh)`; a hand-run cargo
  must do the same, or rustc links for 11.0. The value is not in cargo's
  fingerprint, so `build.sh` runs `cargo clean --release` on a workspace
  whose binary was linked for a *newer* macOS when it builds that stage.
  `scripts/test.sh` exports the floor too, or the next `build.sh` would
  clean its player away.
- ld's `building for macOS-15.0, but linking with dylib
  '/opt/homebrew/…' which was built for newer version` is harmless: the
  app carries the floor's builds of those bottles.
- `configure-qemu.sh` passes the target as `-mmacosx-version-min` with
  `-Werror=unguarded-availability-new`, so an API newer than the floor
  without an `@available` check fails the build instead of dying on the
  floor's macOS (`strchrnul`, declared from 15.4 and found by meson
  anyway, is patch 46).
- `configure-qemu.sh` uses uv's Python only; a Python complaint means uv
  is not on `PATH`.
- Homebrew's mingw is symlinked into `/opt/homebrew/bin`, so
  `build-driver.sh` asks the compiler for its sysroot to find the DDK
  headers. `build-wrappers.sh` is `set -e` and writes the ISO last, so
  an ISO older than its sources means a stage died.

## Running a guest

**A bare `qemu-system-i386` has no display, audio backend or 3D**; the
embed library brings the 3D provider (patch 30) and the audiodev (patch
20). With no `-display` QEMU serves VNC on `localhost:5900`, so pass
`-display vnc=:0` and `-audiodev none,id=snd`. A guest asking it for 3D
is refused and keeps running.

```sh
build/qemu/qemu-system-i386 --version        # 9.2.4
printf 'info mtree\nquit\n' | build/qemu/qemu-system-i386 -machine pc -display none \
    -monitor stdio -net none 2>/dev/null | grep -E 'mesapt|glidept'
# expect the Mesa pass-through region and no glidept one (patch 74)
```

The player, as a machine runs it (`launcherx --print-args` prints the
launcher's exact line):

```sh
PLAYER_LATENCY=1 target/release/player --shader third_party/slang-shaders/crt/crt-lottes.slangp -- \
  -L $PWD/qemu/pc-bios -machine pc,hpet=off -cpu pentium3 -m 256 -hda <overlay>.qcow2 \
  -vga none -device d3dpt-vga -net none -usb -device usb-tablet -device sb16,audiodev=embed0
# XP: -m 512 -device AC97,audiodev=embed0
```

On the Air, Win98 with crt-lottes measures p50 6–10 ms, p95 15–17 ms
publish→present (the vsync-phase floor). `-cpu pentium3` is the guest
tools' floor (SSE1, msvcrt); qemu-3dfx's `-cpu max` advice is for its
x86-64-v2 wrappers. Win9x wants at most 512 MB (VCache).

### 3D in the player

The macOS embed backend (`embed/mglcntx_embed.c`) is a drawable-less
CGL context handing frames to the player through an IOSurface ring
(`player/src/iosurface.rs`); doc 12 has the design. On a GL guest
(wglgears) expect:

```
glcntx: CGL (window-less)          renderbuffers 1/2 800x600
GL 2.1 Metal … / Apple M1          default FBO 1 (bound 1) … complete
glcntx: zero-copy: IOSurface ring  [3d] slot 0: imported 800x600
[3d] pass-through on               … and [3d] pass-through off on exit
```

An incomplete FBO publishes no frames (the desktop freezes); the
`glcntx:` lines name the failing call. `zero-copy off: <reason>` or
`[3d] slot N: zero-copy import failed` means readback carries the
frames.

### A Win98 guest by hand

The install must come out **ACPI** (doc 06), which a plain `SETUP`
does. Install on the Cirrus (Windows' in-box driver), then run
`D:\SETUP.EXE /ALL` from the guest-tools ISO for the rest, our display
driver included:

```sh
build/qemu/qemu-img create -f qcow2 ~/vms/win98.qcow2 4G
build/qemu/qemu-system-i386 -machine pc,hpet=off -cpu pentium3 -m 256 \
  -hda ~/vms/win98.qcow2 -cdrom ~/isos/Win98SE.iso -boot d \
  -vga cirrus -display vnc=:0 -net none -audiodev none,id=snd -device sb16,audiodev=snd
```

**Repairing a PnP-BIOS image** (installed before the BIOS stamp:
`info usb` shows the tablet, Windows shows nothing, Device Manager has
"Plug and Play BIOS" with a yellow !). Copy the CD's `WIN98` folder to
`C:\` first (the CD driver goes away mid-way), then Device Manager →
System devices → "Plug and Play BIOS" → Update Driver → "Display a
list…" → Show all hardware → "PCI Bus". Windows re-detects every device.

**The OpenGL check.** `SETUP /ALL` copies `TESTS\` into `C:\2KSBOX`,
and `SETUP /GAME 3 C:\2KSBOX` puts qemu-3dfx's `OPENGL32.DLL` beside
them. In the player `WGLGEARS.EXE` passes with smooth gears, a host
renderer string (not "GDI Generic") and `mesapt: DLL loaded` on stderr;
`TESTS\GLPROBE.EXE` answers the same in a log.

`tools/win98-driver-test.sh` runs here too. It needs only `mtools` from
outside the tree (ImageMagick's `identify` is optional; without it the
colour count reads `?`) and an ACPI image: on a PnP-BIOS one nothing
matches the INF and the run ends on the in-box VGA.

## The app

`scripts/package-macos.sh` makes the signed, notarized `.app` with the
JIT entitlement (doc 07) for a Mac with nothing installed:

```sh
scripts/package-macos.sh                       # build, stage, check, sign, notarize, dmg
scripts/package-macos.sh --no-sign --no-dmg    # the staging and its checks alone, ~20 s
scripts/package-macos.sh --community           # ADR-019's community build (below)
scripts/package-macos.sh --x86_64 --no-notarize # the Intel app, from scripts/build.sh --x86_64 (below)
```

`--no-build`, `--no-notarize`, `--identity`, `--keychain-profile` and
`--out` are in the script's header. Notarization credentials, once:

```sh
xcrun notarytool store-credentials 2ksbox-notary \
    --apple-id <you@example.com> --team-id <TEAMID> --password <app-specific-password>
```

**The bundle is the prefix.** `Contents` has doc 07's `lib` /
`libexec` / `share` layout, found by the same `share/2ksbox` marker.
`MacOS/` does `bin/`'s job (Launch Services starts programs only from
there); `paths::bin_dir()` derives it from the running executable, so a
plain tarball on a Mac is still a Unix prefix.

**Nothing may come from outside the bundle.** The non-system dylib
closure (20-odd libraries, ~14 MB) goes into `Contents/lib/2ksbox`,
every install name becomes `@rpath`, and **every `LC_RPATH` pointing
out of the app is deleted**. Meson gives `libqemu-embed` one per
Homebrew prefix, searched first, so a bundle that keeps them works here
and fails on a Mac without Homebrew.

**Qt comes through `macdeployqt`**, run first on a bundle that already
has its `Info.plist` (it reads `CFBundleExecutable`), into `Frameworks`,
`PlugIns` and `Resources/qml`. It needs help:

- **`-qmldir=launcher-qt/qml` is required.** Our QML is a compiled-in
  resource, so without it the scanner deploys no modules and the app
  dies on `module "QtQuick" is not installed`.
- **It deploys too much.** Homebrew symlinks every Qt formula into one
  tree and `macdeployqt` takes whole categories, so VirtualKeyboard,
  Scene2D/3D, Pdf and the like arrive without their frameworks (34
  `ERROR: Cannot resolve rpath` pairs, folded into one line). The
  staging prunes the plugin under `PlugIns`, then the dangling QML
  module (a module's plugin under `Resources/qml` is a **symlink** into
  `PlugIns`).
- **It leaves what it keeps half-wired.** Homebrew's Qt already uses
  `@rpath/…`, so the copies keep Homebrew's rpaths (`libqsvg` resolved
  nowhere beside `QtSvg.framework`). The staging gives every plugin
  rpaths into `Contents/Frameworks` from itself and from its loading
  executable (a QML plugin reached through the symlink has the wrong
  `@loader_path`), and every plain dylib in `Frameworks` an `@rpath` id
  and `@loader_path`.
- **The ad-hoc re-sign finds every Mach-O by file type, not mode.** An
  arm64 binary whose load commands changed under its signature is
  killed silently, and a QML plugin or Qt framework can arrive mode 644.
- The offscreen platform plugin is copied beside the cocoa one for the
  window check.

**The Vulkan driver** is the one companion no load command names. The
app carries the LunarG loader and KosmicKrisp with its own ICD manifest,
found through DXVK patch 06 (`@loader_path` ahead of bare leaf names).
An installed player sets `D3DPT_EXEC_LIB`, `D3DPT_DXVK_LIB` and
`VK_DRIVER_FILES` when unset
(`player/src/companions.rs`); each `dlopen` search otherwise starts in a
`build/` directory. The launcher's probe (`--host-check`) opens the
app's `lib/2ksbox/libvulkan.1.dylib` by full path
(`host_gpu::shipped_loader`) and names the app's ICD to it at `main`
(`host_gpu::announce_driver`), because a leaf-name `dlopen` finds
nothing in the app and the loader reads drivers only from the
environment and system directories. The packager requires that
`--host-check` to say "the app's own".

**The checks are the point of the script:**

- no Mach-O may name an `@rpath` dependency nothing in the bundle
  resolves, expanded as dyld does (the loading executables' rpaths
  included);
- the staged launcher's `--paths`, run with `env -i` from `/`, resolves
  every companion inside the app; a machine created with the packaged
  `qemu-img` gets `-L` pointed at the packaged firmware;
- the packaged player runs under `DYLD_PRINT_LIBRARIES=1` and **every
  image the loader touches** must be inside the app, `/usr/lib` or
  `/System`;
- the staged launcher **opens a real window**
  (`QT_QPA_PLATFORM=offscreen` with `LAUNCHER_QT_SHOT=<png>`, under the
  same loader watch). Qt finds its platform plugin and QML modules by
  name at run time, so without this a bundle with no QtQuick passes
  everything and opens nothing.

Signing is inside-out, every nested Mach-O before the bundle that seals
it, with `--options runtime` and `packaging/macos/2ksbox.entitlements`
(`com.apple.security.cs.allow-jit`; without it TCG dies on its first
translated block). Notarization requires `--timestamp`, and Apple's
timestamp service drops out for seconds at a time, so that call alone
retries.

### Two builds (ADR-019)

| | App Store | Community |
|---|---|---|
| macOS | 26+, Apple Silicon | Homebrew's floor (15.0 today); Intel permitted, untested ("The Intel build" below) |
| Direct3D | DXVK on KosmicKrisp | the same, plus the executor on Wine below Vulkan 1.3 |
| Distribution | App Store | Developer ID DMG (`--community`) |

Both come from the same script. `--community` adds
`libd3dpt_exec_remote.dylib`, `wine/d3dpt_exec.dll` and
`d3dpt-exec-host.exe`, built by `scripts/build-d3dpt-exec.sh --wine`
with mingw-w64 (ADR-018, doc 14). The App Store build carries nothing of
Wine and never gets a pre-26 version.

Below macOS 26 KosmicKrisp loads but reports no GPU
(`vkEnumeratePhysicalDevices` fails), so the community build runs the
same executor on the user's Wine (`2ksbox --host-check` says "runs through Wine on this
host", exit 0). A Mac with no Wine has no Direct3D pass-through. **No
package ships a Wine**;
the launcher's note says which to install:

- Homebrew's Wine casks are disabled (not notarized), so the options are
  WineHQ's tarball from Gcenx's releases (x86_64, under Rosetta on Apple
  Silicon) or CrossOver. A packaged app finds it in
  `/Applications/Wine {Stable,Staging,Devel}.app`, on `PATH` or through
  `D3DPT_WINE`; a checkout also looks in `build/wine/`.
- Native arm64 Wine has no OpenGL in `winemac.drv` (macOS gives the GL
  compatibility renderer only to Rosetta processes).
- **A macOS VM cannot test this path** (no accelerated OpenGL, which
  Wine's Mac driver requires). A pre-26 macOS on a second APFS volume
  can (`tools/macos-wine-spike-local.sh`; loop in M15's track doc).
- The `exec-wine` check skips over ssh, because Wine's Mac driver needs
  the window server.

The community build permits Intel, untested, because its Wine is
x86_64 on both architectures (ADR-019 has the reasons). No doc claims
Intel until an Intel Mac has run the reference scene.

### The Intel build

**Status (2026-09-23): not buildable as written.** The recipe below
needs an Intel Homebrew, and Homebrew's installer now refuses one
("Homebrew on macOS is only supported on Apple Silicon processors!",
Homebrew 7.0.6's `install.sh`), so the `--x86_64` plumbing is checked in
as work in progress. User decision the same day: depending on Homebrew
at all was a mistake; the app's libraries are built here ("The
libraries" above; QEMU's side done, `build-deps.sh --arch x86_64` is its
Intel half), Qt is next, and the community build then targets the
lowest macOS that allows. What the Intel build still needs from an
Intel Homebrew after that is only build tools: an x86_64 Python for
meson (uv can install one: `uv python install cpython-3.12-macos-x86_64-none`)
and an x86_64 Qt, which the Qt step removes. The plumbing (the Rosetta
re-run, the per-architecture directories, the architecture check) stays,
and the Homebrew lines below are what that replaces.

The Intel app is the community build and nothing else: macOS 15 (the
floor), no App Store version, and **no Vulkan at all**, since KosmicKrisp
exists only as arm64 and MoltenVK is refused (ADR-007). So it carries no
loader, no ICD, no DXVK and no in-process executor; its Direct3D is the
executor on Wine (native x86_64 Wine there, no Rosetta), and a Mac with
no Wine has none. It is made on the Apple Silicon Mac, under Rosetta,
against a second Homebrew:

```sh
# once: the Intel Homebrew at /usr/local (the installer needs sudo), and
# what the build takes from it. uv, mingw-w64 and xorriso stay the
# native ones: an arm64 program runs from a Rosetta shell, and the guest
# ISO it makes is the same file
arch -x86_64 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
arch -x86_64 /usr/local/bin/brew install ninja meson pkg-config glib pixman gnu-sed libslirp qt python@3.13

scripts/build.sh --x86_64                          # qemu, rust, qt, exec (dxvk is skipped); ~40 min
scripts/package-macos.sh --x86_64 --no-notarize    # build/macos-x86_64/2ksbox-<version>-macos-x86_64.dmg
scripts/package-macos.sh --x86_64 --no-sign --no-dmg   # the staging and its checks alone
                                                   # (scripts/test.sh's package-x86_64 check)
```

How it works, so it stays one build and not a second tree of scripts:

- `--x86_64` re-runs the script as an x86_64 process (`arch -x86_64`)
  with `/usr/local/bin` first on `PATH`, and that is all. Under Rosetta
  `uname -m`, Apple's compiler, and the Intel Homebrew's meson, ninja,
  pkg-config and Python all answer x86_64 without being told, and
  `arch -x86_64 scripts/build.sh` is the same build.
- Every script that has a build directory recognises the translated
  process (`sysctl.proc_translated`) and keeps to `build/x86_64/` and
  `target/x86_64-apple-darwin/` beside the native build's
  (`configure-qemu.sh`, `build-d3dpt-exec.sh`, `qemu-embed/build.rs`,
  `build.sh`, `package-macos.sh`), the way the Windows cross build keeps
  `build/win/`. The `qemu/` and `third_party/dxvk` trees and their
  prepare stamps are shared, so the two builds run one after the other,
  never at once.
- `configure-qemu.sh` takes the Intel Homebrew's Python, not uv's: meson
  takes the machine its interpreter runs on for the build machine, and
  uv's is arm64.
- cargo is never translated (rustup's toolchain is arm64) and simply
  cross-compiles with `--target x86_64-apple-darwin`; `cc` adds
  `-arch x86_64` for that target, and `cxx-qt-build` finds the Intel Qt
  through the `qmake6` on `PATH`.
- The bottle tag has the architecture in it: `scripts/macos-floor.sh
  --tag` says `sequoia` where the native build says `arm64_sequoia`,
  and `macos-bottles.py` swaps in the Intel bottles. Homebrew has Intel
  at tier 3 since 7.0.0 (bottles frozen, support ending September 2027),
  so the swap works while every formula the app carries still has an
  Intel bottle of the installed version.
- The packager checks the architecture of every Mach-O in the app
  beside the floor: a file that is not `x86_64` (an arm64 KosmicKrisp,
  say) fails the package. On an Intel Mac itself nothing is translated,
  and the same scripts make its native app with the same checks.

What the Air can and cannot prove: the staged app's own checks run under
Rosetta (the loader's image list, the offscreen window, `--host-check`,
the wizard), and the app can be opened under Rosetta for a look. TCG's
x86-64 backend and the Voodoo 2's SSE2 rasteriser are the Linux rig's
every day. But Rosetta translates the JIT's output and says nothing about
speed, and no Intel Mac is among the test machines, so the DMG goes on
the release page labelled untested until one has run the reference scene
(ADR-019).

### The libraries

User decision (2026-09-23): **depending on Homebrew for what the app
carries was a mistake.** Homebrew builds every library for the macOS it
runs on and publishes bottles for three releases back, so the app's
floor was Homebrew's floor; its installer refuses Intel Macs since
Homebrew 7, which ended the Intel build before it started; and a `brew
upgrade` changed what shipped. So the libraries are built here, from
upstream, and Homebrew is a source of build tools and of recipes to crib
flags and patches from, nothing more.

**QEMU's side is done.** `scripts/build-deps.sh` (the `deps` stage of
`build.sh`, macOS only) builds glib 2.90 with pcre2 10.48 and the libffi
and stub libintl that glib's own tarball carries as subprojects, pixman
0.46, libslirp 4.9 and zstd 1.5, from tarballs pinned by sha256 in the
script, for the architecture named (`--arch x86_64` for the Intel
build) and `MACOSX_DEPLOYMENT_TARGET`, into `build/deps/<arch>` as
**static archives**. `configure-qemu.sh` sets `PKG_CONFIG_LIBDIR` to that
prefix and the SDK's own `.pc` files and passes meson `prefer_static`,
so `libqemu-embed-i386.dylib` and `qemu-img` carry the libraries and
link nothing from `/opt/homebrew`; a library QEMU would auto-detect from
Homebrew (libpng, jpeg-turbo) is simply not found, and `--disable-png
--disable-vnc-jpeg` say so on purpose. The stage is stamped on the
script, the floor and the architecture, and a rebuild reconfigures QEMU.
Linux and the Flatpak keep the distribution's libraries; the script
refuses to run there.

**Qt is next**, and it is why the floor below still reads Homebrew's:
the launcher's Qt and its closure (43 frameworks and about 30 dylibs
after pruning: ICU, dbus, OpenSSL, tiff, webp, harfbuzz and the rest)
are Homebrew's bottles. A qtbase + qtdeclarative built here with Qt's
bundled third-party copies would drop nearly all of that, and the floor
then becomes what Qt supports on Apple Silicon (macOS 11 with Qt 6.5,
12 with 6.8; to be confirmed against Qt's supported platforms before
pinning), not what Homebrew bottles.

### The floor

The app runs down to **the oldest macOS Homebrew supports** for as long
as it carries Homebrew's Qt ("The libraries" above):
`HOMEBREW_MACOS_OLDEST_SUPPORTED` in Homebrew's `brew.sh`, read by
`scripts/macos-floor.sh`: **15.0 (Sequoia)** now. QEMU's libraries are
built for the same target, so nothing of ours raises it, and once Qt is
ours the number becomes a constant of this repository. A `brew update`
that drops a release changes the value, and the next `scripts/build.sh`
retargets everything.

Three pieces make the claim true:

- **Everything of ours is built for the floor**, through the deployment
  target and the availability error flag above (a new floor recompiles
  QEMU and DXVK).
- **The Homebrew libraries (Qt's, now) are the floor's builds.** Homebrew pours the
  bottle for the macOS it runs on. After staging, `scripts/macos-bottles.py` swaps every file
  above the floor for the same version's build from the floor's bottle
  (`scripts/macos-floor.sh --tag`, e.g. `arm64_sequoia`), fetched from
  Homebrew's registry on ghcr.io and cached in `build/macos-bottles`. It
  matches a staged file to its Cellar original by `LC_UUID` (install-name
  rewrites leave it alone), following the keg's symlinks, and gives the
  older build the staged file's install name, dependencies and rpaths.
  The installed version must be one Homebrew has bottles of (`brew
  upgrade` otherwise). `brew fetch --bottle-tag=…` does not work for
  this: on a newer Mac it answers "Bottle for tag … is unavailable",
  though the registry has them all.
- **The package fails above it.** `LSMinimumSystemVersion` is the
  highest `LC_BUILD_VERSION` `minos` in the bundle, and any Mach-O above
  the floor fails `package-macos.sh` by name. The ad-hoc signing pass
  must see the swapped Qt framework binaries (mode 644, no extension);
  one left unsigned kills the launcher at its first framework
  (`SIGKILL (Code Signature Invalid)`). The "still links" check skips a
  file's own install name, the first line `otool -L` prints, because a
  framework keeps Homebrew's absolute one. The LunarG loader and
  KosmicKrisp are 11.0 builds and never set the minimum.
