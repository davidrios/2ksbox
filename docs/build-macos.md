# Building and testing on macOS (Apple Silicon)

Everything here runs natively on arm64; the test machine is an M1
MacBook Air. This covers setting a Mac up, building, running a guest,
the app and its two builds (ADR-019), and the floor they target. The
stages themselves are `docs/development.md`'s; the Mac-side traps that
cut across subsystems are in `docs/00-status.md` ("Running on a Mac").

## One-time setup

```sh
xcode-select --install                       # Apple clang + git
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
brew install ninja meson pkg-config glib pixman gnu-sed uv libslirp
brew install qt                              # Qt 6: the launcher, and macdeployqt
brew install mingw-w64 xorriso nasm mtools   # guest-tools ISO, the Wine pair, the DOS batteries
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

- **Qt 6** — the launcher is `launcher-qt` (ADR-015), and the formula
  brings `macdeployqt`, which puts Qt inside the `.app`. Without it
  `scripts/build.sh` skips its `qt` stage and the Mac can build
  everything but a package.
- **gnu-sed** — `sign_commit` uses GNU `sed -i`; `prepare-qemu.sh` puts
  `gsed` first on `PATH`.
- **No XQuartz and no SDL2.** QEMU is built with no display of its own
  (`--disable-sdl --disable-cocoa …`; patch 02 drops qemu-3dfx's SDL
  requirement), and patch 70 replaced qemu-3dfx's GLX-on-XQuartz backend
  on Darwin with one that only refuses a context. 3D is the player's CGL
  backend. Either library, if installed, is simply unused.
- The Khronos `GL/glcorearb.h` is vendored in `third_party/khronos`, so
  the build never depends on what headers the Mac has.

### Vulkan for the Direct3D executor

DXVK's d3d9 (ADR-007) runs over Mesa's **KosmicKrisp**, which needs
macOS 26 and ships in the LunarG SDK as an optional component. MoltenVK
is refused by DXVK (`docs/spikes/spike-c-dxvk-native-macos.md`).

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

The installer refuses to add a component to an existing root, hence the
maintenance tool for the second step; the SDK stays under
`~/VulkanSDK`. `scripts/test.sh` sets the Vulkan environment itself on
Darwin — Homebrew's loader on `DYLD_LIBRARY_PATH`, the SDK's KosmicKrisp
ICD unless `VK_ICD_FILENAMES` is set — because SIP strips a `DYLD_*`
variable exported to a `#!/usr/bin/env bash` script (the symptom was
`Direct3DCreate9 failed`). Put **only**
`/opt/homebrew/opt/vulkan-loader/lib` on `DYLD_LIBRARY_PATH`, never all
of `/opt/homebrew/lib`, and check the shell a launcher is started from
for the same export: that directory answers ImageIO's codec names and
kills every image decode with `SIGBUS` (00-status, "Running on a Mac").

### Open Watcom, for the Win98 display driver

The 16-bit `.drv` and ring-0 `.vxd` of doc 19 (and `GLIDE2X.OVL`) build
with Open Watcom, whose snapshot carries an arm64 macOS set (`armo64`)
beside Linux's `binl64`; `build-driver9x.sh` picks by `uname`.

```sh
curl -L -o ow.tar.xz https://github.com/open-watcom/open-watcom-v2/releases/download/Last-CI-build/ow-snapshot.tar.xz
mkdir -p ~/.local/opt/open-watcom && tar xJf ow.tar.xz -C ~/.local/opt/open-watcom
```

(`WATCOM=` if it is elsewhere.) The Mac's `d3dpt9v.vxd` is byte-identical
to Linux's; `d3dpt9x.drv` differs by one instruction selection with the
same semantics, which is expected. `wdis` segfaults on our 16-bit
objects on both hosts.

## Building

```sh
git clone --recurse-submodules --shallow-submodules git@github.com:davidrios/2ksbox.git
cd 2ksbox
scripts/build.sh            # QEMU ~10–15 min on the Air, the Rust side ~1 min
target/release/player       # the test pattern, through wgpu on Metal
```

The test pattern is colour bars with a 1-px white border and a white line
sweeping down (a pass ≈ 8 s): sharp edges, integer-scaled 4:3, no
tearing, still 4:3 and pixel-aligned after a resize.

Mac specifics of the stages:

- **Everything targets the floor** (below): `build.sh` exports
  `MACOSX_DEPLOYMENT_TARGET=$(scripts/macos-floor.sh)`, and a hand-run
  cargo must do the same. rustc otherwise links for 11.0, and cargo does
  not rebuild when the value changes (it is not in its fingerprint), so
  `build.sh` runs `cargo clean --release` in a workspace whose binary was
  linked for a *newer* macOS, only when that workspace's stage is being
  built. `scripts/test.sh` exports the floor too, or its player build
  would link for 11.0 and the next `build.sh` would clean it away.
- ld still warns `building for macOS-15.0, but linking with dylib
  '/opt/homebrew/…' which was built for newer version` about this Mac's
  own Homebrew bottles. Warnings only: the app carries the floor's
  builds of them instead.
- `configure-qemu.sh` passes the target as `-mmacosx-version-min` with
  `-Werror=unguarded-availability-new`, so an API newer than the floor
  used without an `@available` check fails the build instead of making a
  binary that dies on the floor's macOS (`strchrnul`, declared from 15.4
  and found by meson anyway, is patch 46).
- `configure-qemu.sh` uses uv's Python only; a Python complaint means uv
  is not on `PATH`.
- Homebrew's mingw is symlinked into `/opt/homebrew/bin`, so
  `build-driver.sh` asks the compiler for its sysroot to find the DDK
  headers. `build-wrappers.sh` is `set -e` and writes the ISO last: an
  ISO older than its sources means a stage died.

### The Glide wrapper

`scripts/prepare-openglide.sh && scripts/build-glide.sh` builds
`build/glide/libglide2x.dylib` (doc 12 §5). OpenGLide includes
`<GL/gl.h>` and `<GL/glext.h>`, and macOS has no `GL/` directory — the
one a Mac may have is XQuartz's Mesa, which would bind the wrapper to a
GLX library that never sees a CGL context. So `glidept/host/macos/GL/`
holds a forwarding `gl.h` and a `glext.h` supplying what Apple's 2003
header lacks (the seventeen `PFNGL…PROC` typedefs OpenGLide names,
`APIENTRY`, and four paletted-texture / packed-pixel enums that only have
to compile). Check the binding:

```sh
otool -L build/glide/libglide2x.dylib                      # OpenGL.framework and libSystem only
nm -gU build/glide/libglide2x.dylib | grep -c '_gr\|_gu'   # 120
```

The `glide-host` check does not run on a Mac (it drives the embed
backend through EGL).

## Running a guest

**A bare `qemu-system-i386` has no display, no audio backend and no
3D**: the player is the front end, the embed library appends `-display
none`, and it brings the 3D provider (patch 30) and the audiodev (patch
20). Given no `-display`, QEMU starts a VNC server on `localhost:5900`
(`open vnc://localhost:5900` in Screen Sharing); pass `-display vnc=:0`
and `-audiodev none,id=snd` explicitly. A guest asking for 3D on a bare
QEMU is refused and keeps running.

```sh
build/qemu/qemu-system-i386 --version        # 9.2.4
printf 'info mtree\nquit\n' | build/qemu/qemu-system-i386 -machine pc -display none \
    -monitor stdio -net none 2>/dev/null | grep -E 'glidept|glidelfb|glideshm|mesapt'
# expect the four pass-through MMIO regions
```

The player, the way a machine runs (`launcherx --print-args` gives the
launcher's exact line):

```sh
PLAYER_LATENCY=1 target/release/player --shader third_party/slang-shaders/crt/crt-lottes.slangp -- \
  -L $PWD/qemu/pc-bios -machine pc,hpet=off -cpu pentium3 -m 256 -hda <overlay>.qcow2 \
  -vga none -device d3dpt-vga -net none -usb -device usb-tablet -device sb16,audiodev=embed0
# XP: -m 512 -device AC97,audiodev=embed0
```

On the Air, Win98 with crt-lottes measures p50 6–10 ms, p95 15–17 ms
publish→present (the vsync-phase floor). `-cpu pentium3` is the guest
tools' floor (SSE1, msvcrt) — qemu-3dfx's `-cpu max` advice is for
*its* x86-64-v2 wrappers — and Win9x wants at most 512 MB (VCache).

### 3D in the player

The embed library's macOS backend (`embed/mglcntx_embed.c`, doc 12) is
a CGL context with no drawable and an FBO standing in for framebuffer
0, with frames handed to the player through a ring of three IOSurfaces
the player wraps as Metal textures (`player/src/iosurface.rs`). Every
GL / CGL / IOSurface call is resolved with `dlsym` on the
OpenGL.framework handle, never by link. On a GL guest (wglgears) the
expected stderr is:

```
glcntx: CGL (window-less)          renderbuffers 1/2 800x600
GL 2.1 Metal … / Apple M1          default FBO 1 (bound 1) … complete
glcntx: zero-copy: IOSurface ring  [3d] slot 0: imported 800x600
[3d] pass-through on               … and [3d] pass-through off on exit
```

An incomplete FBO publishes no frames (the desktop stays frozen) and
the `glcntx:` lines name the failing call. `zero-copy off: <reason>`
from the backend, or `[3d] slot N: zero-copy import failed` from the
player, means the readback path is carrying the frames instead.

### A Win98 guest by hand

The install must come out **ACPI**, or QEMU's PCI hot-adds (USB tablet,
AC'97, NIC) are never seen; a plain `SETUP` does that because
`prepare-qemu.sh` stamps the BIOS date (doc 06). Install on the Cirrus
(Windows' in-box driver), then `D:\SETUP.EXE /ALL` from the guest-tools
ISO installs the rest, our display driver included:

```sh
build/qemu/qemu-img create -f qcow2 ~/vms/win98.qcow2 4G
build/qemu/qemu-system-i386 -machine pc,hpet=off -cpu pentium3 -m 256 \
  -hda ~/vms/win98.qcow2 -cdrom ~/isos/Win98SE.iso -boot d \
  -vga cirrus -display vnc=:0 -net none -audiodev none,id=snd -device sb16,audiodev=snd
```

**Repairing a PnP-BIOS image** (installed before the BIOS stamp: `info
usb` shows the tablet, Windows shows nothing, Device Manager has "Plug
and Play BIOS" with a yellow !) without reinstalling: copy the CD's
`WIN98` folder to `C:\` first (the CD driver goes away mid-way), then
Device Manager → System devices → "Plug and Play BIOS" → Update Driver
→ "Display a list…" → Show all hardware → "PCI Bus". Windows then
re-detects every device.

**The OpenGL check**: `SETUP /ALL` copies `TESTS\` (`WGLGEARS.EXE`
among them) into `C:\2KSBOX`, and `SETUP /GAME 3 C:\2KSBOX` puts
qemu-3dfx's `OPENGL32.DLL` beside it; in
the player a smooth gears window and a host renderer string (not "GDI
Generic") is the pass, with `mesapt: DLL loaded` on stderr.
`TESTS\GLPROBE.EXE` answers the same question in a log.

The guest half of the Win98 display driver runs here as well:
`tools/win98-driver-test.sh` needs only `mtools` from outside the tree
(`identify`, from ImageMagick, is optional; without it the colour count
reads `?`). It needs an ACPI image: on a PnP-BIOS one nothing matches
the INF and the run ends on the in-box VGA.

## The app

`scripts/package-macos.sh` makes doc 07's "signed .app, JIT entitlement,
notarized", for a Mac that has none of this checked out:

```sh
scripts/package-macos.sh                       # build, stage, check, sign, notarize, dmg
scripts/package-macos.sh --no-sign --no-dmg    # the staging and its checks alone, ~20 s
scripts/package-macos.sh --community           # ADR-019's community build (below)
```

`--no-build`, `--no-notarize`, `--identity`, `--keychain-profile` and
`--out` are in the script's header. Notarization credentials are stored
once, by hand:

```sh
xcrun notarytool store-credentials 2ksbox-notary \
    --apple-id <you@example.com> --team-id <TEAMID> --password <app-specific-password>
```

**The bundle is the prefix.** `Contents` has the `lib` / `libexec` /
`share` shape of doc 07's install layout, so `launcher_core::paths`
finds it by the same `share/2ksbox` marker. The one difference is
`MacOS/`, which does `bin/`'s job because Launch Services starts
programs only from there; `paths::bin_dir()` decides it from the running
executable's directory, so a plain tarball on a Mac is still a Unix
prefix.

**Nothing may come from outside the bundle.** The Mac that runs it has
no Homebrew and no Vulkan, so the whole non-system dylib closure (20-odd
libraries, ~14 MB) is copied into `Contents/lib/2ksbox`, every install
name rewritten to `@rpath`, and **every `LC_RPATH` pointing out of the
app deleted** — meson gives `libqemu-embed` one per Homebrew prefix,
searched before the app's own, so a bundle that keeps them loads *this*
Mac's Homebrew and fails only on a Mac without it.

**Qt comes through `macdeployqt`**, which runs first, on a bundle that
already has its `Info.plist` (it reads `CFBundleExecutable`), and copies
the frameworks, the cocoa platform plugin and the QML tree into
`Frameworks`, `PlugIns` and `Resources/qml`. What it needs help with:

- **`-qmldir=launcher-qt/qml` is required.** Our QML is compiled in as a
  Qt resource, so the import scanner (which reads source) finds nothing
  without it, deploys no modules, and the app dies on `module "QtQuick"
  is not installed` after starting perfectly.
- **It deploys too much.** Homebrew's Qt is modular but symlinks every
  formula's plugins and QML modules into one tree, and `macdeployqt`
  deploys whole plugin categories and module directories: VirtualKeyboard,
  Scene2D/3D, Pdf and the like arrive with frameworks it never collected
  (34 `ERROR: Cannot resolve rpath` pairs, folded into one line). None of
  them can load, so the staging prunes them — the plugin under `PlugIns`,
  then the QML module left dangling (a module's plugin under
  `Resources/qml` is a **symlink** into `PlugIns`).
- **It leaves what it keeps half-wired.** Homebrew's Qt already
  references `@rpath/…`, so `macdeployqt` rewrites nothing and the copies
  keep Homebrew's rpaths, which point into the build directory or at
  `Contents/lib` (`libqsvg` resolved nowhere with `QtSvg.framework`
  beside it). The staging gives every plugin rpaths into
  `Contents/Frameworks` both from itself and from the executable that
  loads it (a QML plugin reached through that symlink has an
  `@loader_path` that is not its real directory), and every plain dylib
  in `Frameworks` an `@rpath` id and `@loader_path`.
- **The ad-hoc re-sign covers every Mach-O**, found by file type, not
  mode: `macdeployqt` rewrites load commands, an arm64 binary whose
  signature no longer matches is killed without a message, and a QML
  plugin or a Qt framework binary can arrive mode 644.
- The offscreen platform plugin is copied beside the cocoa one for the
  window check.

**The Vulkan driver** is the one companion no load command names: the
app carries the LunarG loader and KosmicKrisp with an ICD manifest of
its own, found through DXVK patch 06 (`@loader_path` ahead of bare leaf
names). `player/src/companions.rs` sets `QEMU_GLIDE_LIB`,
`D3DPT_EXEC_LIB`, `D3DPT_DXVK_LIB` and `VK_DRIVER_FILES` when an
installed player finds them unset, since each `dlopen` search otherwise
starts in a `build/` directory a package does not have.

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
  same loader watch): Qt finds its platform plugin and QML modules by name
  at run time, so without this a bundle with no QtQuick passes everything
  and opens nothing.

Signing is inside-out, every nested Mach-O before the bundle that seals
it, with `--options runtime` and `packaging/macos/2ksbox.entitlements`
(`com.apple.security.cs.allow-jit`, without which TCG dies on its first
translated block). `--timestamp` is required by notarization and Apple's
timestamp service drops out for seconds at a time, so that call alone
retries.

### Two builds (ADR-019)

| | App Store | Community |
|---|---|---|
| macOS | 26+, Apple Silicon | Homebrew's floor (15.0 today); Intel permitted, untested |
| Direct3D | DXVK on KosmicKrisp | the same, plus the executor on Wine below Vulkan 1.3 |
| Distribution | App Store | Developer ID DMG (`--community`) |

Both come from the same script; `--community` adds
`libd3dpt_exec_remote.dylib` and `wine/d3dpt_exec.dll` +
`d3dpt-exec-host.exe`, built by `scripts/build-d3dpt-exec.sh --wine`
with mingw-w64 (ADR-018, doc 14). The App Store build carries nothing
of Wine and never gets a pre-26 version.

Below macOS 26 there is no KosmicKrisp, so DXVK finds no device. The
community build then runs the same executor on the user's Wine
(`launcher --host-check`: "runs through Wine on this host", exit 0); a
Mac with no Wine falls back to WineD3D in the guest, until M15's last
step retires that path. **No package ships a Wine**, and the launcher's
note says which to install:

- Homebrew's Wine casks are disabled (not notarized), so it is WineHQ's
  tarball from Gcenx's releases — x86_64, under Rosetta on Apple Silicon
  — or CrossOver. A packaged app finds it in
  `/Applications/Wine {Stable,Staging,Devel}.app`, on `PATH` or through
  `D3DPT_WINE`; a checkout also looks in `build/wine/`.
- Native arm64 Wine has no OpenGL in `winemac.drv` (macOS gives the GL
  compatibility renderer only to Rosetta processes).
- **A macOS VM cannot test this path**: Apple's paravirtual GPU has no
  accelerated OpenGL and Wine's Mac driver requires
  `kCGLPFAAccelerated`. A real pre-26 macOS on a second APFS volume can
  (`tools/macos-wine-spike-local.sh`; M15's track doc has the loop).
- The `exec-wine` check skips over ssh: Wine's Mac driver needs the
  window server.

Intel is permitted by the community build because its Wine is x86_64 on
both architectures, and untested: Homebrew moved Intel to tier 3 in
7.0.0 (bottles frozen, support ending with macOS 27), and no doc claims
it until an Intel Mac has run the reference scene.

### The floor

The app runs down to **the oldest macOS Homebrew supports** (user
decision, 2026-09-12): `HOMEBREW_MACOS_OLDEST_SUPPORTED` in Homebrew's
`brew.sh`, which `scripts/macos-floor.sh` reads — **15.0 (Sequoia)**
since Homebrew raised it on 2026-09-10 (it was 14.0 when ADR-019 was
written; the rule is the same). It cannot go lower, because the app
carries Homebrew's libraries (glib, pixman, libslirp, zstd, libpng,
jpeg-turbo, Qt) and Homebrew builds each for the macOS versions it
supports and none older. It moves with Homebrew: a `brew update` that
drops a release changes the value, and the next `scripts/build.sh`
retargets everything.

Three pieces make the claim true:

- **Everything of ours is built for the floor** — the deployment target
  and the availability error flag above; QEMU and DXVK take it as a
  compiler flag, so a new floor recompiles them rather than relinking.
- **The Homebrew libraries are the floor's builds.** Homebrew pours the
  bottle for the macOS it runs on, so on a macOS 26 Mac several libraries
  are 26.0 builds. After staging, `scripts/macos-bottles.py` swaps every
  file above the floor for the same version's build from the floor's
  bottle (`scripts/macos-floor.sh --tag`, e.g. `arm64_sequoia`), fetched
  from Homebrew's registry on ghcr.io and cached in
  `build/macos-bottles`. A staged file is matched to its Cellar original
  by `LC_UUID` (which install-name rewrites do not touch), following the
  keg's symlinks, and the older build is given the staged file's install
  name, dependencies and rpaths. It wants the version Homebrew has
  bottles of installed (`brew upgrade` otherwise). `brew fetch
  --bottle-tag=…` does not work for this: on a newer Mac it answers
  "Bottle for tag … is unavailable", though the registry has them all.
- **The package fails above it.** `LSMinimumSystemVersion` is measured,
  the highest `LC_BUILD_VERSION` `minos` in the bundle, and any Mach-O
  above the floor fails `package-macos.sh` by name. The ad-hoc signing
  pass must see the swapped Qt framework binaries (mode 644, no
  extension): one left unsigned kills the launcher at its first framework
  (`SIGKILL (Code Signature Invalid)`). The "still links" check skips a
  file's own install name, the first line `otool -L` prints — a framework
  keeps Homebrew's absolute one. The LunarG loader and KosmicKrisp are
  11.0 builds and never set the minimum.
