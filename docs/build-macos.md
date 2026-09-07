# Building and testing on macOS (Apple Silicon)

Everything below runs natively on arm64. Tested target: M1 MacBook Air.

## One-time setup

```sh
xcode-select --install                       # Apple clang + git
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
brew install ninja meson pkg-config glib pixman gnu-sed uv libslirp
brew install qt                              # Qt 6: the launcher, and macdeployqt
brew install --cask xquartz                  # log out/in once after installing
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh      # Rust toolchain
```

Why each of the odd ones:
- **Qt 6** — the launcher is `launcher-qt` (ADR-015), and the same
  formula brings `macdeployqt`, which is what puts Qt inside the `.app`.
  A Mac without it still builds everything else (`scripts/build.sh` skips
  its `qt` stage and says so); it just cannot package.
- **XQuartz** — qemu-3dfx's Mesa pass-through uses its GLX backend on
  macOS (it dlopens `/opt/X11/lib/libGL.dylib` at runtime); the patched
  `meson.build` hardcodes `-I/opt/X11/include` and links
  `-L/opt/X11/lib -lX11 -lXxf86vm -lGL -framework OpenGL`. Without it:
  `GL/glcorearb.h not found`, then link errors, then no 3D at runtime.
- **no sdl2** — qemu-3dfx makes SDL2 mandatory
  (`error('Featuring qemu-3dfx required SDL2')`), but patch 02 removes
  that and `configure-qemu.sh` passes `--disable-sdl`: nothing we ship
  opens a QEMU window (2026-09-07). If you have SDL2 installed for
  something else it is simply unused.
- **gnu-sed** — `sign_commit` uses GNU `sed -i` syntax;
  `scripts/prepare-qemu.sh` puts Homebrew's `gsed` first on PATH when present.
- The Khronos `GL/glcorearb.h` is additionally vendored in
  `third_party/khronos` and put on the include path by
  `scripts/configure-qemu.sh`, so the header itself never depends on
  XQuartz's Mesa headers version.

## Vulkan for the Direct3D executor (macOS 26 only)

DXVK's d3d9 (ADR-007) runs over Mesa's KosmicKrisp, shipped in the LunarG
SDK as an optional component. Homebrew's `vulkan-loader` and `molten-vk`
stay installed (MoltenVK is refused by DXVK, see spike C).

```sh
brew install vulkan-headers vulkan-loader vulkan-tools glslang     # + meson ninja from above
curl -sSL -o vulkan-sdk.zip https://sdk.lunarg.com/sdk/download/latest/mac/vulkan-sdk.zip
unzip -q vulkan-sdk.zip                                          # vulkansdk-macOS-<ver>.app
V=1.4.357.1                                                      # the version the zip carried
vulkansdk-macOS-$V.app/Contents/MacOS/vulkansdk-macOS-$V --root ~/VulkanSDK/$V \
  --accept-licenses --default-answer --confirm-command install
~/VulkanSDK/$V/MaintenanceTool.app/Contents/MacOS/MaintenanceTool \
  --accept-licenses --default-answer --confirm-command install com.lunarg.vulkan.kosmic
export VK_ICD_FILENAMES=~/VulkanSDK/$V/macOS/share/vulkan/icd.d/libkosmickrisp_icd.json
vulkaninfo --summary | grep -E "driverName|apiVersion"           # KosmicKrisp, 1.4.x
```

The installer refuses to add a component into an existing root, hence the
maintenance tool for the second step. The SDK is self-contained under
`~/VulkanSDK`; nothing goes into `/usr/local` unless the `usr` component is
chosen. Then `scripts/prepare-dxvk.sh && scripts/configure-dxvk.sh && ninja
-C build/dxvk` and the run lines in `tools/dxvk-d3d9-test.cpp` with that
`VK_ICD_FILENAMES` (2026-09-03: both harnesses pass on the Air, see
`patches/dxvk/README.md`). `scripts/test.sh` sets that environment
itself on Darwin (Homebrew's loader on `DYLD_LIBRARY_PATH`, and the SDK's
KosmicKrisp ICD unless `VK_ICD_FILENAMES` is already set): a `DYLD_*`
variable exported to the script is stripped by SIP at its
`#!/usr/bin/env bash` exec, which made
the two native DXVK checks fail with `Direct3DCreate9 failed` (2026-09-04).

## Clone

```sh
git clone --recurse-submodules --shallow-submodules git@github.com:davidrios/2ksbox.git
cd 2ksbox
```

## Rust side (player, libdisc, launcher) — ~1 min

```sh
cargo build --release
cargo test --workspace
target/release/player          # window with the test pattern, rendered via wgpu → Metal
```

What to look for: color bars with a 1-px white border, a white line sweeping
down (one pass ≈ 8 s), sharp edges (nearest sampling, integer-scaled 4:3),
no tearing. Resize the window: bars stay 4:3 and pixel-aligned.

## QEMU side (patched with qemu-3dfx) — ~10–15 min on an M1 Air

```sh
scripts/prepare-qemu.sh        # overlay hw/3dfx + hw/mesa, apply 3dfx patch + our queue, sign_commit
scripts/configure-qemu.sh      # uv-managed Python 3.12, --disable-werror
ninja -C build/qemu qemu-system-i386 qemu-system-x86_64
```

`configure-qemu.sh` sets `MACOSX_DEPLOYMENT_TARGET` to the running macOS
version (unless you preset it). Without that, the 15.4+ SDK's availability
annotations (`strchrnul is only available on macOS 15.4 or newer`) produce a
wall of `-Wunguarded-availability-new` warnings, because QEMU's configure
detects the function and uses it unguarded. Local builds therefore target the
machine they're built on; release packaging will choose its own floor.

Order matters: if you re-run `prepare-qemu.sh` later (e.g. after pulling a
patch-queue change), run `configure-qemu.sh` again before `ninja` — a
refreshed overlay can make ninja regenerate the build with default options
(notably `werror` back on).

Smoke tests:

```sh
build/qemu/qemu-system-i386 --version                     # 9.2.4
printf 'info mtree\nquit\n' | build/qemu/qemu-system-i386 -machine pc -display none \
    -monitor stdio -net none 2>/dev/null | grep -E 'glidept|glidelfb|glideshm|mesapt'
# expect the four pass-through MMIO regions
build/qemu/qemu-system-i386 -machine pc -cpu max -m 256 -display vnc=:0
# ^ QEMU has no local display any more (see below); the BIOS screen is at
#   vnc://localhost:5900 — Screen Sharing opens it
```

Notes:
- **`-cpu max`** is what qemu-3dfx recommends for TCG on Apple Silicon
  (x86-64-v2 feature level); our machine definitions will pin `pentium2` /
  `pentium3` models for guest compatibility — both are TCG-only here.
- TCG needs JIT. Locally built, ad-hoc-signed binaries can `MAP_JIT` fine;
  the `com.apple.security.cs.allow-jit` entitlement is what keeps that true
  under the hardened runtime a notarized `.app` must use — it is in
  `packaging/macos/2ksbox.entitlements`, see "The app" below.
- If configure complains about Python, it means uv isn't on PATH — the
  script never uses the system interpreter.

## The Glide wrapper — seconds

```sh
scripts/prepare-openglide.sh && scripts/build-glide.sh   # build/glide/libglide2x.dylib
```

qemu-3dfx ships no host-side Glide library, so this is ours: OpenGLide with
the window-less platform layer in `glidept/host/` (doc 12 §5,
`patches/openglide/README.md`). QEMU finds it through
`QEMU_GLIDE_LIB=build/glide/libglide2x.dylib`.

The Mac needs one thing Linux does not, and it is a header problem, not a
library one. OpenGLide says `<GL/gl.h>` and `<GL/glext.h>`; macOS has no
`GL/` directory, because the framework keeps its headers under `OpenGL/`.
The one `GL/` that *does* exist here is `/opt/X11/include/GL`, XQuartz's
Mesa — pointing the build at it compiles and then binds the wrapper to a GLX
library that will never see a CGL context, the same trap the embed backend's
`dlsym` rule exists to avoid. So `glidept/host/macos/GL/` holds a forwarding
`gl.h` and `glext.h`, on the include path on Darwin only, and the `glext.h`
supplies what Apple's stops short of (it is `GL_GLEXT_VERSION 8`, from 2003,
written before `PFNGL…PROC`): the seventeen typedefs OpenGLide names,
`APIENTRY`, and four `EXT_paletted_texture` / `EXT_packed_pixels` enums that
only have to exist for `PGTexture.cpp` to compile — neither extension is on
a Mac, and OpenGLide asks the driver before it uses either.

Check what it bound to; `OpenGL.framework` and `libSystem` are the only two
lines that belong there:

```sh
otool -L build/glide/libglide2x.dylib
nm -gU build/glide/libglide2x.dylib | grep -c '_gr\|_gu'   # 120
```

`scripts/test.sh`'s `glide-host` check does **not** run here: it drives the
embed backend through EGL. The wrapper is built on the Air, not proven on
it.

## Spike A, step 1: Win98 + guest wrappers (hand-run)

Goal: prove qemu-3dfx accelerates a guest on this Mac. You need your own
Win98 SE install ISO; everything else comes from the repo.

```sh
# guest wrappers, built from the exact qemu-3dfx commit the host is signed with
brew install mingw-w64 xorriso && guest-tools/build-wrappers.sh
#   → guest-tools/out/guest-tools-3dfx-<rev>.iso   (or one built on Linux — same commit)
#   The script aborts (set -e) and leaves the OLD ISO in place if any stage
#   fails; check the "==> .iso" line at the end. Homebrew's mingw is symlinked
#   into /opt/homebrew/bin, so build-driver.sh asks the compiler for its
#   sysroot to find the DDK headers (fixed 2026-09-04; before that the ISO
#   built on the Air silently lacked DRIVER\ and everything added after it).

# 1. install Win98 (cirrus = in-box high-color driver, no extra guest driver needed)
#    IMPORTANT: the install must come out ACPI, or QEMU's PCI bus is left
#    un-enumerated ("Plug and Play BIOS" with a yellow ! in Device Manager) and
#    any PCI device added later (USB controller, AC'97, NIC) is never detected.
#    A plain SETUP does that since 2026-09-06: prepare-qemu.sh stamps the
#    firmware's BIOS date past the 12/01/99 setup compares against (doc 06),
#    confirmed by an install 2026-09-07. On a build from before the stamp it
#    took  D:\WIN98\SETUP /p j  from "Start computer with CD-ROM support".
qemu-img create -f qcow2 ~/vms/win98.qcow2 4G
build/qemu/qemu-system-i386 -machine pc -cpu pentium3 -m 256 \
  -hda ~/vms/win98.qcow2 -cdrom ~/isos/Win98SE.iso -boot d \
  -vga cirrus -display vnc=:0 -net none \
  -audiodev none,id=snd -device sb16,audiodev=snd   # no host audio backend is built
# 2. after install, boot with the guest-tools ISO attached
build/qemu/qemu-system-i386 -machine pc -cpu pentium3 -m 256 \
  -hda ~/vms/win98.qcow2 -cdrom guest-tools/out/guest-tools-3dfx-*.iso \
  -vga cirrus -display vnc=:0 -net none \
  -audiodev none,id=snd -device sb16,audiodev=snd   # no host audio backend is built
```

**Standalone `qemu-system-i386` has no display and no 3D, by decision
(2026-09-07).** QEMU is configured with no user interface at all —
`--disable-sdl --disable-gtk --disable-cocoa --disable-curses
--disable-spice`, and no host audio backend either (`--disable-coreaudio`
and the rest) — because the player is the front end: it embeds QEMU,
the embed library appends `-display none` itself, and it brings its own 3D
context provider (patch 30) and audio backend (patch 20). Carrying SDL2
(plus the SDL3 that Homebrew's sdl2-compat loads behind it) and Cocoa into
the `.app` to keep a debugging path alive was not worth it.

So a guest activating pass-through on a bare `qemu-system-i386` gets its
context refused and keeps running (patches 04 and 30), and **to see a
guest by hand, use VNC** — which QEMU does for you: with no local display
compiled in and no `-display` given, `qemu_setup_display()` starts a VNC
server on `localhost:5900` (`system/vl.c`), so `open vnc://localhost:5900`
in Screen Sharing is the window. The lines above say `-display vnc=:0`
outright, and `-audiodev none` because CoreAudio is not built either (the
player's sound is patch 20's `embed` audiodev; `wav` is the other one that
always exists, and is how `tools/xp-cdimage-test.sh` captures CD-DA).
Anything scripted passes `-display none` and gets neither.
**3D on macOS is the player**, which registers the embed library's
window-less backend.

The history, because the symptom is memorable: upstream's macOS 3D path is
GLX-on-XQuartz, and on a Cocoa SDL window `ui/sdl2.c` handed the `NSWindow*`
to the GLX backend, which used it as an X11 window id — `X Error …
BadDrawable, Major opcode 129 (Apple-DRI)` (Sequoia 15.7). Patch
`02-mesa-sdlgl-on-darwin` worked around it by building qemu-3dfx's
SDL/native-OpenGL backend (`mglcntx_sdlgl.c`) instead; that patch is gone
with SDL, and Darwin now builds the same GLX file Linux does, which nothing
calls. XQuartz stays a *build* dependency all the same: the qemu-3dfx meson
overlay hardcodes `-L/opt/X11/lib -lX11 -lXxf86vm -lGL` into every
emulator's link line.

**Tuning knobs — `mesagl.cfg`:** qemu-3dfx reads `mesagl.cfg` from the
*current working directory* at startup. Keys: `ExtensionsYear`,
`ExtensionsLength`, `VertexCacheMB`, `DispTimerMS`, `BufOAccelEN`,
`ContextMSAA`, `ContextSRGB`, `ContextVsyncOff`, `RenderScalerOff`,
`FpsLimit`, `DumpShader`, `CheckError`, `FifoTrace`, `FuncTrace` (one
`Key,value` per line). On macOS `DispTimerMS` (default 0) also selects the
GL profile: 0 → core profile, non-zero → compatibility. Frame presentation
is the embed backend's swap from the device handler; if it looks janky
unless the mouse moves, try `DispTimerMS,16` and/or `ContextVsyncOff,1` /
`FpsLimit,60` and report.

**Grab workaround for desktop use:** add `-usb -device usb-tablet`. With an
absolute pointer the frontend never grabs mouse or keyboard, so no release
hotkey is needed (Win98 SE drives a USB HID tablet with in-box drivers).
Relative (PS/2) mode is only needed for mouselook games. If `info usb` shows the
tablet but Win98 shows nothing, the guest is a PnP-BIOS install (see the
ACPI note above; an image installed before the BIOS-date stamp is one).
Repair without reinstalling: copy the CD's `WIN98`
folder to `C:\` first, then Device Manager → System devices → "Plug and
Play BIOS" → Update Driver → "Display a list…" → Show all hardware → "PCI
Bus"; Windows then re-detects every device (that's why the folder copy is
needed — the CD driver goes away mid-way) and finally finds the QEMU USB
Tablet.

**Mouse/keyboard grab** is the player's business (doc 03); QEMU's own
displays are not used. The SDL hotkey lore that used to live here went with
`--disable-sdl` (2026-09-07) — including the Caps-Lock→Control remap that
made SDL report left Control as *right* Control and killed every Ctrl+Alt
hotkey, which patch `03-sdl-darwin-either-ctrl` used to fix.

In the guest: run `D:\SETUP.EXE` — it installs the device mapper and the
Glide wrappers for whichever Windows this is, and its "Test programs"
component puts `WGLGEARS.EXE` in `C:\2KSBOX`. Then copy `D:\OPENGL\OPENGL32.DLL`
next to it (`SETUP /GAME 3 C:\2KSBOX`), reboot, and run
`C:\2KSBOX\WGLGEARS.EXE`.
Accelerated = a smooth gears window and a Mesa/host renderer string (not
"GDI Generic"); qemu-3dfx also prints context messages on the host
terminal (`mesapt: DLL loaded`, `glcntx: …`). For Glide, any Glide title
works once `GLIDE2X.DLL` is in `SYSTEM`; for OpenGL games drop
`OPENGL32.DLL` next to the game EXE (Quake 2 is the classic check).

Known trap (fixed in our queue since 2026-09-02): stock QEMU 9.2.4 TCG
blue-screens Win98 SE with `exception 0D` on the first boot after setup
(upstream issue 2987); `prepare-qemu.sh` applies the upstream fix.

Notes: `-cpu pentium3` is the compatibility-safe choice for Win9x under
TCG and the floor our wrappers are built for (SSE1); qemu-3dfx's own
`-cpu max` advice exists because *their* wrappers target x86-64-v2. Keep
RAM ≤ 512 MB (Win9x VCache limit). Warm reboot currently freezes on the
Air (under investigation — shut down and cold start instead). Record
renderer string + fps under "Result" in `docs/spikes/spike-a-macos.md`.

## The player on the Mac (M1)

```sh
export MACOSX_DEPLOYMENT_TARGET=$(sw_vers -productVersion | cut -d. -f1,2)  # same as configure-qemu.sh
ninja -C build/qemu libqemu-embed-i386.dylib && cargo build --release
PLAYER_LATENCY=1 target/release/player --shader third_party/slang-shaders/crt/crt-lottes.slangp -- \
  -L $PWD/qemu/pc-bios -machine pc -cpu pentium3 -m 256 -hda ~/vms/win98.qcow2 \
  -vga cirrus -net none -usb -device usb-tablet -device sb16,audiodev=embed0
# XP: -m 512 -vga cirrus -device AC97,audiodev=embed0   (std VGA has no XP driver: 640x480x16)
```
After `git pull`, always `scripts/prepare-qemu.sh && scripts/configure-qemu.sh`
before that `ninja`: `qemu/embed/` is a copy of `embed/`, so a pull that
changes the embed API (e.g. v3 added `qemu_embed_set_refresh_ms`) leaves the
old dylib in place and the player link fails with
`Undefined symbols for architecture arm64: _qemu_embed_set_refresh_ms`.
`qemu-embed/build.rs` prints a warning when the copy is stale.

The deployment-target export silences ld's
`object file … was built for newer macOS version than being linked` /
`building for macOS 11 but linking with dylib built for 15.0` warnings:
rustc links for 11.0 by default, while libqemu (configure-qemu.sh) and the
cmake-built C++ deps (glslang, spirv-cross) target the running OS. They are
warnings only; the link is fine either way.

## 3D inside the player (M3, macOS backend — verified 2026-09-02 on the Air)

The embed library carries a window-less Mesa backend for macOS
(`embed/mglcntx_embed.c`, CGL section): a CGL context with no drawable, an
FBO over shared renderbuffers standing in for the window's default
framebuffer (framebuffer binding 0 is redirected to it, patch 32), and a
`glReadPixels` of that FBO on every guest swap handed to the player.
Verified with Win98 wglgears: `GL 2.1 Metal - 89.4 / Apple M1`, FBO
complete, gears in the player, Esc returns the desktop. Every GL/CGL call
the backend makes is resolved with `dlsym` on the OpenGL.framework handle:
the macOS QEMU build also links XQuartz's Mesa libGL, and a plainly linked
`gl*` symbol binds to that (a GLX library that sees no CGL context and
silently does nothing — the first two runs failed exactly that way).

```sh
git pull
scripts/prepare-qemu.sh && scripts/configure-qemu.sh     # patches 30/31/32 + meson changes
ninja -C build/qemu qemu-system-i386 libqemu-embed-i386.dylib
cargo build --release
target/release/player -- -L $PWD/qemu/pc-bios -machine pc -cpu pentium3 -m 256 \
  -hda ~/vms/win98.qcow2 -vga cirrus -net none -usb -device usb-tablet \
  -device sb16,audiodev=embed0
# in the guest: C:\WINDOWS\Desktop\GAMEDIR\WGLGEARS.EXE
```

Expected stderr: `glcntx: CGL (window-less)`, `drawable 800x600`,
`renderbuffers 1/2 800x600`, `GL 2.1 Metal … / Apple M1`,
`default FBO 1 (bound 1) … complete`, `[3d] pass-through on`, then
wglgears' own `N frames in 5.0 seconds, X FPS` lines; Esc →
`[3d] pass-through off` and the desktop back. If the FBO is reported
incomplete, frames are not published (desktop stays frozen) and the
`glcntx:` lines name the failing call. The native GLX backend is still
linked (weak) into `qemu-system-i386`, but nothing registers a provider for
it, so standalone QEMU refuses 3D rather than drawing it.

### Zero-copy (IOSurface) — verified 2026-09-03 on the Air

The macOS backend now offers a ring of three IOSurfaces (BGRA8) bound to
`GL_TEXTURE_RECTANGLE` textures via `CGLTexImageIOSurface2D`; every swap
blits the stand-in FBO into the next one (flipped) and the player wraps the
same IOSurface in a Metal texture (`player/src/iosurface.rs`, wgpu-hal
`texture_from_raw`). Embed API v6 (`on_3d_iosurface`). Readback stays the
fallback: if anything in the ring fails the backend logs
`zero-copy off: <reason>` and keeps rendering the old way.

```sh
git pull
scripts/prepare-qemu.sh
ninja -C build/qemu libqemu-embed-i386.dylib
cargo build --release      # new deps: objc2, objc2-metal, objc2-io-surface
```

Stderr on wglgears: `glcntx: zero-copy: IOSurface ring`,
`zero-copy slot 0: 800x600 IOSurface 0x…`, `[3d] slot 0: imported
800x600` (then slots 1 and 2), gears in the window, higher fps than the
readback run. Failure modes to send back: a Rust compile error in
`iosurface.rs` (objc2 names), `zero-copy off: …` from the backend, or
`[3d] slot N: zero-copy import failed: …` from the player.

Verified 2026-09-02: Win98 desktop with sound, keyboard, tablet mouse;
sRGB-correct. `PLAYER_LATENCY=1` prints publish→present percentiles; on the
Air with Win98 + crt-lottes: p50 6–10 ms, p95 15–17 ms, max 18 ms (the
vsync-phase floor; earlier ~32 ms readings were the saturated-FIFO bug fixed
2026-09-02). Guest power-off closes the player cleanly. 3D inside the player: not until M3
(a GL app is refused cleanly; a Glide app still exits QEMU).

## The app — `scripts/package-macos.sh`

Doc 07's "signed .app, JIT entitlement, notarized", for handing to someone
who has none of this checked out:

```sh
scripts/package-macos.sh                       # build, stage, check, sign, notarize, dmg
scripts/package-macos.sh --no-sign --no-dmg    # the staging and its checks alone, ~20 s
```

Notarization credentials are stored once, by hand, and the script never
invents them:

```sh
xcrun notarytool store-credentials 2ksbox-notary \
    --apple-id <you@example.com> --team-id <TEAMID> --password <app-specific-password>
```

**The bundle is the prefix.** `Contents` has the same
`lib` / `libexec` / `share` shape doc 07's install layout has, so
`launcher_core::paths` finds it by the same `share/2ksbox` marker with no
macOS special case — except one: `MacOS/` does `bin/`'s job, because it is
the only directory Launch Services will start a program from.
`paths::bin_dir()` is where that single difference lives, decided from the
running executable's own parent directory rather than the prefix's shape,
since a plain tarball extracted on a Mac is still an ordinary Unix prefix
with a real `bin`.

**Nothing may come from outside the bundle**, and that is most of the
script. A Linux package leans on the distribution for glib, pixman, zstd
and the rest; the Mac that will run this has no Homebrew, no XQuartz and
no Vulkan at all. So the whole non-system dylib closure — 20-odd
libraries, ~14 MB — is copied into `Contents/lib/2ksbox`, every install
name rewritten to `@rpath`, and **every `LC_RPATH` pointing out of the app
deleted**. That last one is the trap: meson gives `libqemu-embed` one
`LC_RPATH` per Homebrew prefix it linked against, they are searched before
the `@loader_path` the packaging adds, and a bundle that keeps them loads
*this* machine's Homebrew — passing every check that only looks at load
commands, and failing on the first machine that has no Homebrew.

**Qt is a third thing again, and `macdeployqt` is what brings it**
(ADR-015, 2026-09-07: the launcher is `launcher-qt`). It runs *before*
the closure above, on a bundle that already has its `Info.plist` — the
tool reads `CFBundleExecutable` to know what to follow — and it copies
the Qt frameworks, the cocoa platform plugin and the QtQuick QML module
tree into `Contents/Frameworks`, `PlugIns` and `Resources/qml`. Two
things about that:

- **`-qmldir=launcher-qt/qml` is not optional.** Our QML is compiled into
  the binary as a Qt resource (`build.rs`'s `QmlModule`), so
  `macdeployqt`'s import scanner — which reads *source* — finds no
  imports at all without being pointed at them, deploys no modules, and
  the app dies on `module "QtQuick" is not installed` after starting
  perfectly.
- **The re-sign afterwards is not cosmetic.** `macdeployqt` rewrites load
  commands, and on arm64 a binary whose signature no longer matches is
  killed by the kernel with no message. The staging's ad-hoc re-sign pass
  therefore covers every Mach-O in the bundle, and it can no longer be
  found by "executable files": a QML plugin can arrive mode 644.

The offscreen platform plugin is copied by hand beside the cocoa one,
because the window check below needs a window that does not appear on the
packager's screen.

One dependency is not in that closure and had to be found by other means:

- **The Vulkan driver.** DXVK `dlopen`s a loader that macOS does not have,
  so the app carries the LunarG SDK's loader and the KosmicKrisp ICD, with
  an ICD manifest of its own (the SDK's points into `~/VulkanSDK`).
  Finding them needed DXVK patch 06 (`@loader_path` ahead of the bare leaf
  names) and, on the QEMU side, `player/src/companions.rs`, which fills in
  `QEMU_GLIDE_LIB`, `D3DPT_EXEC_LIB`, `D3DPT_DXVK_LIB` and
  `VK_DRIVER_FILES` when an installed player finds them unset — each of
  those `dlopen` searches starts at a `build/` directory a package does
  not have.

**The checks are the point of the script**, and they are the macOS form of
`package-linux.sh`'s: the staged launcher is asked `--paths` with `env -i`
from `/` and every companion must resolve inside the app; it creates a
machine with the packaged `qemu-img` and `--print-args` must point `-L` at
the packaged firmware; and the packaged player is run under
`DYLD_PRINT_LIBRARIES=1`, where **every image the loader touches** must be
inside the app, `/usr/lib` or `/System`. That last check is what caught
both of the above.

Since the launcher is Qt there is one more, and it is the only one that
can see a whole class of failure: **the staged launcher has to open a
real window**. `QT_QPA_PLATFORM=offscreen` with `LAUNCHER_QT_SHOT=<png>`
(the launcher's own headless grab, doc 07) must produce a PNG, and the
same run is watched with `DYLD_PRINT_LIBRARIES=1` so the images the QML
engine pulls in have to be the app's own copies as well. Qt finds its
platform plugin and its QML modules by name at run time, out of
directories that appear in no load command — so without this check a
bundle with no QtQuick in it passes everything and opens nothing.

`LSMinimumSystemVersion` is **measured, not chosen**: the highest
`LC_BUILD_VERSION` `minos` of everything the bundle carries. A bundled
Homebrew or Vulkan SDK dylib built on a newer system sets the real floor
whatever we would prefer to claim, and as of 2026-09-06 that was macOS 26.6
(libslirp and sdl2-compat were the 26.0 ones; sdl2-compat is gone since
2026-09-07, so re-measure. QEMU's own build targets the running OS unless
`MACOSX_DEPLOYMENT_TARGET` says otherwise). To lower it,
build QEMU and those dependencies against the floor first — the script
will then report it.

Signing is inside-out, every nested Mach-O before the bundle that seals
it, `--options runtime` with `packaging/macos/2ksbox.entitlements`
(`com.apple.security.cs.allow-jit`, without which TCG dies on its first
translated block). `--timestamp` is required by notarization and Apple's
timestamp service does go away for seconds at a time, so that one call
retries; nothing else does.
