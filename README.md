# 2ksbox

Run Windows 98, Windows XP and DOS the way the machines of the era did:
period 3D games accelerated on your GPU, the picture on a CRT-shaded
display instead of a blurry stretched rectangle, and a CD-ROM drive
faithful enough to run raw dumps of the discs you own, copy protection
included.

Built on a patched QEMU. Runs on Linux, Windows and macOS (Apple Silicon).
Free software, GPL-2.0.

## What it does

- **A machine library, not a command line.** The launcher creates a
  Windows 98, Windows XP, DOS or "Other" (BeOS, a period Linux, OS/2)
  machine from a short wizard, with sane defaults for each family. Each
  machine opens in its own player window.
- **Real 3D in the guest.** DirectX 1 up to 9 through our own paravirtual
  device and driver, Glide through a host-side wrapper, OpenGL
  pass-through, and an emulated 3dfx Voodoo 2 running 3dfx's own driver
  for the games nothing else covers. Quake II, Unreal Tournament, GTA
  Vice City, Max Payne and Need for Speed: Porsche Unleashed all run.
- **A CRT on your monitor.** The guest's own framebuffer, at its native
  resolution and aspect (320×200 included), through a libretro slang
  shader chain. Shader profiles are managed in the launcher with a live
  preview.
- **Your discs.** cue/bin, CloneCD, Alcohol, ISO, and any folder on your
  disk served as a CD. A shared disc shelf: swap discs while a machine
  runs, from the launcher or from inside the guest. CD audio plays.
- **Music.** A Sound Blaster 16 with a real OPL3, and an MPU-401 with
  General MIDI (a bank is included) or a Roland MT-32 (bring your own
  ROMs).
- **Gamepad support.** Your PS/Xbox/Switch controller reaches the guest as a
  generic USB controller or a DOS gamepad.
- **Snapshots and clones.** Named snapshots of a machine ("fresh
  install", "drivers in", "before game X") and one-click copies of a
  whole machine.
- **DOS at period speed.** A DOS machine's processor is throttled to a
  chosen rate, so speed-sensitive games run as they were meant to. (Not fully tested!)

Not the goal: cycle-accurate emulation of specific chipsets (that is
86Box and PCem), modern guests, or condone piracy. Use only your own install
media, licences and disc dumps!

## What you need

| Host | Requirements |
|---|---|
| Linux | An x86-64 machine. KVM for near-native XP (optional; Windows 98 is emulated on purpose). A GPU with Vulkan 1.3 for the fast Direct3D path; without it, Direct3D goes through OpenGL and WineD3D inside the guest, which still works. |
| macOS | Apple Silicon, macOS 14 or newer. Guests are emulated (there is no x86 virtualization on these Macs) and still run comfortably faster than a period PC. The fast Direct3D path needs macOS 26; older releases use WineD3D inside the guest. |
| Windows | 64-bit Windows 10 or 11. WHPX (the Windows Hypervisor Platform) accelerates XP when it is enabled. |

You also need install media for the guest operating system (your own
Windows 98 / XP CD image, a DOS floppy or CD) and, for games, dumps of
your own discs.

## Getting 2ksbox

There are no downloadable packages, so the way to get 2ksbox today is
to build it from source. The build is one command once the tools are
installed, and it produces the same launcher, player and guest-tools disc
that are in the packaged releases.

## Building from source

### 1. Install the tools

**Linux** (Arch is what the project is developed on; the Debian/Ubuntu
column was checked to install on Debian 12 and 13 and on Ubuntu 24.04 and
26.04):

| Purpose | Arch | Debian / Ubuntu |
|---|---|---|
| Compilers and build tools | `base-devel git ninja meson pkgconf` | `build-essential git ninja-build meson pkg-config` |
| QEMU's libraries | `glib2 pixman zlib libslirp mesa libx11` | `libglib2.0-dev libpixman-1-dev zlib1g-dev libslirp-dev libgl-dev libx11-dev` |
| The launcher (Qt 6) | `qt6-base qt6-declarative` | `qt6-base-dev qt6-declarative-dev qml6-module-qtquick-controls qml6-module-qtquick-layouts qml6-module-qtquick-dialogs` |
| Direct3D executor (optional) | `vulkan-headers vulkan-icd-loader glslang` | `libvulkan-dev glslang-tools` |
| Guest tools disc (optional) | `mingw-w64-gcc xorriso` | `gcc-mingw-w64-i686 xorriso` |

Then Rust and uv, from their own installers (uv provides the Python
version QEMU's build wants; any system Python 3.8 to 3.13 works too if
you set `QEMU_PYTHON` to it, but Ubuntu 26.04's system Python is 3.14,
which QEMU's build refuses, so there uv is not optional):

```sh
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
curl -LsSf https://astral.sh/uv/install.sh | sh
```

**macOS** (Apple Silicon):

```sh
xcode-select --install
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
brew install ninja meson pkg-config glib pixman gnu-sed uv libslirp
brew install qt                              # the launcher
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

Optional on macOS: `brew install mingw-w64 xorriso` for the guest tools
disc, and the Vulkan SDK with KosmicKrisp for the Direct3D executor on
macOS 26 (the recipe is in [docs/build-macos.md](docs/build-macos.md)).

**Windows:** the Windows build is made *on a Linux machine* with podman
or docker installed, and copied over as a zip. See step 5.

### 2. Get the source

```sh
git clone --recurse-submodules --shallow-submodules https://github.com/davidrios/2ksbox
cd 2ksbox
```

Already cloned without submodules? Run
`git submodule update --init --depth 1`.

### 3. Build

```sh
scripts/build.sh
```

That builds everything this machine has the tools for, in order: QEMU,
the Rust programs, the launcher, the Direct3D executor, the Glide wrapper
and the guest-tools disc. The first build takes about fifteen minutes and
several gigabytes; later ones redo only what changed. The summary at the
end lists every stage as built or skipped and, for a skipped one, which
tool was missing. A skipped optional stage means a missing feature (no
Direct3D executor, no guest tools disc), not a broken build; install the
tool and run the command again.

Two things to check in the summary:

- `qt` must be built, or there is no launcher.
- `guest` should be built: without the guest-tools disc there are no
  guest drivers, and a machine has plain VGA and no 3D.

After every `git pull`, run `scripts/build.sh` again.

### 4. Run it

```sh
launcher-qt/target/release/launcher-qt
```

Nothing has to be installed: the launcher finds the player, QEMU, the
firmware and the guest-tools disc in the checkout it was built from.
Machines, discs and profiles go under your user data directory
(`~/.local/share/2ksbox` on Linux, `~/Library/Application Support/2ksbox`
on macOS).

## First run

1. **Shader presets.** The launcher offers to download the libretro
   shader collection the first time it starts with none. Say yes: the
   CRT look is the point, and the starter profiles are made from it. (A
   source checkout already has the collection, so it will not ask.)
2. **Create a machine.** *New machine* walks through family (Windows 98,
   Windows XP, DOS, Other), name, memory, processor, acceleration,
   networking, pointer, disk size and install media. The defaults are
   what the family wants; you can change everything later from the
   machine's settings.
3. **Install the operating system.** Point the install media at your
   Windows CD image and start the machine. Windows installs as it would
   on a PC of the time.
4. **Install the guest tools.** Open the disc shelf, press *Add
   guest-tools ISO*, and put it in the machine's CD drive. Inside the
   guest, run `SETUP.EXE` from that drive (`D:\SETUP.EXE /ALL` from the
   Run box installs everything this Windows can use), then restart. This
   brings the display driver, the Glide wrapper, the OpenGL pass-through
   and the disc-shelf program into the machine. `SETUP /LIST` shows what
   is on the disc; the disc's `README.TXT` explains every folder.
5. **Take a snapshot.** *Snapshots…* on the machine: "fresh install" is
   the one you will keep coming back to, especially on Windows 98.

## Playing games

- **Install from your dumps.** Add the disc image (or a folder) to the
  disc shelf, put it in the drive, install in the guest. Multi-disc
  installs swap discs from the launcher, or from inside the guest with
  `CDSHELF.EXE` (a DOS box has `CDSHELF.COM`), which the guest tools
  install.
- **DirectX 1 up to 8 games** run through our device once the display driver
  is installed. Nothing to copy per game on XP. Where the host has no
  Vulkan 1.3 (a Mac before macOS 26, an older GPU), copy the WineD3D set
  next to the game instead: `SETUP /GAME` on the guest-tools disc, or
  copy the `WINED3D\D3D8-9\` folder from Explorer.
- **DirectX 9 games** need to have the d3d9.dll copied to their folder.
- **3dfx/Glide games.** add the Voodoo 2 device in the machine settings and
  install 3dfx's own Voodoo 2 driver in the guest.
- **OpenGL games** (Quake II and friends) get `OPENGL32.DLL` from the
  guest-tools disc copied next to the game's EXE.
- **Speed.** On an M1 Mac or Ryzen 5700X the emulated machine is roughly
  equivalent to a 1.7GHz Pentium 4 (circa 2001). The graphics performance was
  tested with the M1 and an RX 9060 XT on the Ryzen, both running era games at
  confortable FPS. The launcher's *Emulation optimizations* switches ship at
  the settings that measured best and exist for troubleshooting, not tuning.
- **Too fast, too slow, or wrong colours** usually means the game wants
  something the machine's settings offer: a slower processor on a DOS
  machine, a different display adapter, a sound card the game knows. The
  wizard explains each choice next to it.

## Day to day

Keys in the player window:

| Keys | What |
|---|---|
| Ctrl+Alt+G | release the mouse grab (a click grabs it again) |
| Ctrl+Alt+K | hand the host's own shortcuts (the Windows key, Alt+Tab) back to the host, or to the guest again |
| Ctrl+Alt+Shift+D | Ctrl+Alt+Del in the guest |
| Ctrl+Alt+Shift+F | windowed full screen on and off |
| Ctrl+Alt+S | save the guest's own frame as a PNG |
| Alt+F4 | asks before stopping the machine; the window's close button does not |

Windows machines use a "seamless" mouse by default (the host pointer is
the guest's cursor and the window never grabs); turn it off in the
machine's settings for games that want a real PS/2 mouse.

- **Shader profiles…** names a preset plus your parameter overrides, with
  a live preview against a screenshot. A machine picks a profile by name.
- **Clone…** on a machine copies it whole, its disk and snapshots
  included, under a new name.
- **Gamepads.** A machine's settings choose whether a pad appears in the
  guest as a USB controller (Windows 98 SE, Me and XP see it with no
  driver) or a DOS gamepad.
- **Music.** A machine's settings choose its sound card and its MIDI
  port: General MIDI plays through the included bank or one of your own;
  the MT-32 needs your own ROMs, which you point the launcher at.

## When something goes wrong

- **"Something is missing."** `2ksbox --paths` (`launcher-qt --paths` in
  a checkout) prints where this build looks for each companion program
  and file. `2ksbox --diagnose` prints
  the same plus what it found of this host's 3D, and writes it to
  `launcher.log` beside the machine library. That log is what to attach
  to a bug report. On Windows, `2ksbox-debug.bat` in the package does
  both from a console window.
- **"3D goes through OpenGL instead."** The launcher says this in the
  wizard when the host has no Vulkan 1.3. It is not an error: Direct3D
  games then use WineD3D inside the guest (see Playing games). Keep the
  2ksbox adapter anyway: only its Direct3D needs Vulkan, and everything
  else it does still works, which the Cirrus cannot match. "In
  software (slow)" means a software Vulkan driver was found; a game may
  be faster through WineD3D, so try both.
- **The guest shows a black desktop or stops after a display-adapter
  change.** Windows wants a driver for the new adapter. If the guest
  tools were installed before the change, Windows finds it on the next
  boot; otherwise switch back, run `SETUP /ALL` in the guest, and switch
  again.

## Documentation

- [docs/06-guest-machines.md](docs/06-guest-machines.md) — what each
  machine family is, its defaults, and what to expect from it.
- [docs/07-frontend.md](docs/07-frontend.md) — the launcher and the
  player in detail: the library, the wizard, snapshots, the disc shelf.
- [docs/development.md](docs/development.md) — the developer guide: the
  architecture and design documents, the build stage by stage, every
  player option and environment variable, the launcher's front ends,
  packaging and diagnostics.
- [docs/build-macos.md](docs/build-macos.md) and
  [docs/build-windows.md](docs/build-windows.md) — the platform
  specifics.

## License

GPL-2.0. The player links QEMU in-process and is GPL-2.0-only; the
launcher is GPL-2.0-or-later. The licence text is in [COPYING](COPYING)
and every third-party component is listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

If you package or redistribute the player, read the licensing section of
[docs/development.md](docs/development.md) first: its dependency tree
contains Apache-2.0 crates that GPLv2 cannot formally combine with.
