# Track: M15 — the Direct3D executor on Wine, on the host (ADR-018)

The handoff for a session building the Direct3D fallback for a Linux or
macOS host below DXVK's Vulkan 1.3 floor: the **same** paravirtual device
and the **same** executor as everywhere else, running on **Wine's d3d9
(WineD3D over OpenGL) on the host** — the way the executor ran on
Windows' own d3d9 on a Windows host before 2026-09-17 — instead of a
2015 Wine copied into the guest next to every game. Read
`docs/00-status.md` first for the global picture and the track rules,
then this file, then ADR-018 in doc 10 (why), doc 14 (the protocol and
the executor), and `d3dpt/exec/d3dpt_exec.h` (the five calls this track
has to carry across a process boundary).

Opened 2026-09-22 on the user's decision: "running an old unsupported
Wine version in the guest is bad UX and doesn't make sense". Nothing is
built yet; this doc is the design and the ordered steps.

## Scope and files (this track owns them)

- The Wine-side host program: `d3dpt/exec/d3dpt_exec_host.c` (new; a
  Windows program, cross-built with mingw like `d3dpt_exec.dll`).
- The QEMU side of the transport: `d3dpt/hw/d3dpt_exec_remote.c` (new)
  and the back-end choice in `d3dpt/hw/d3dpt_exec_load.[ch]`.
- The shared-memory plumbing in `d3dpt/hw/d3dpt_vga.c` (VRAM from a
  file when the remote executor is chosen) and `d3dpt/hw/d3dpt_mm.c`
  (its window likewise).
- `scripts/build-d3dpt-exec.sh` (a `--wine` stage: the PE pair for the
  native package), `scripts/build.sh`, the three packagers, and the
  Flatpak manifest.
- `player/src/companions.rs` (finding Wine and the PE pair),
  `launcher-core/src/host_gpu.rs` (the probe's third answer),
  `wizard::Form::graphics_note()`'s sentences.
- Tests: `tools/d3dpt-dp2-test.cpp` and `tools/d3dpt-exec-test.cpp`
  driven through the remote executor, the `exec-wine` check in
  `scripts/test.sh`, `tools/xp-driver-test.sh`'s `EXEC=` knob.
- Docs: this file, ADR-018, doc 14 §"The executor on Wine", doc 04's
  fallback rows, the M15 rows of `docs/00-status.md` and doc 08.
- Shared (rebase first, edit minimally, say so in the commit):
  `d3dpt/exec/d3dpt_exec.cpp` — **which needs nothing for Wine**: the
  hidden window and the executor-owned scene that Windows' own d3d9
  wanted (ADR-007's second amendment, 2026-09-21) are exactly what
  wined3d wants, and `D3DPT_D3D9=system` loads Wine's builtin
  `system32\d3d9.dll` by the same full path. `d3dpt/d3dpt_proto.h`
  untouched.
- **Not this track's to touch until its last step:** everything of
  WineD3D-in-guest — `guest-tools/build-wrappers.sh`'s wine9x build,
  `patches/wine9x/`, `SETUP /GAME 4`/`5`, `/I 7`, `guest-tools/src/d3dpre.c`,
  `tools/xp-wined3d-test.sh`, `tools/wined3d-sys-test.sh`, doc 19 §42–44.
  They keep working for every below-floor host until the replacement
  has passed the reference scene (step 5), and are removed then, in one
  commit, with doc 04 and the launcher's advice.

## The design

### What stays the same

Everything the guest sees. The display driver (XP and 98), the per-game
`D3D8.DLL`/`D3D9.DLL` serializers, the record protocol
(`d3dpt/d3dpt_proto.h`, no version bump), the DP2 path, VRAM as the
texture and vertex-buffer store, readbacks into VRAM, the hardware
cursor, gamma — all of it is a stream of records into a window the host
reads, and the executor that reads it does not change either:
`d3dpt/exec/d3dpt_exec.cpp` + `d3dpt_exec_ddi.cpp`, 3,460 lines over the
`IDirect3D9`/`IDirect3DDevice9` COM interface, already compiled for
Windows by `scripts/build-d3dpt-exec.sh --windows` (mingw, against
`<d3d9.h>`, the d3d9 library loaded by name at run time). That DLL on a
Windows host with DXVK's `dxvk_d3d9.dll` is the shipped Windows package;
**that same DLL on Wine with Wine's builtin `d3d9.dll` is this track**.

### Why it has to be a second process

Wine's `d3d9.dll` exists only inside a Wine process: it is a PE module
that needs `ntdll`, `kernel32`, `user32`, `gdi32`, `opengl32` and the
display driver (`winex11.drv` / `winewayland.drv` / `winemac.drv`)
underneath it, all of which need Wine's loader to be the process's
entry point and a `wineserver` behind it. It cannot be `dlopen`ed into
QEMU, and winelib does not change that (a winelib binary *is* a Wine
process). So on a below-floor host the executor runs **out of process**:

```
QEMU (native)                                  wine d3dpt-exec-host.exe (PE, x86_64)
  d3dpt-vga / d3dpt (SysBus)                     d3dpt_exec_host.c
    d3dpt_exec_load.c: which back end?             ├─ d3dpt_exec.dll   (the executor, unchanged)
    d3dpt_exec_remote.c ── pipe (requests) ──────► │    └─ d3d9.dll     (Wine's, WineD3D → opengl32 → host GL)
                        ◄─ pipe (replies) ─────────┘
    VRAM + command window: one file, MAP_SHARED on both sides
```

In-process (DXVK, `libd3dpt_exec.so`) stays the first choice and the
fast path; the remote executor is taken when DXVK finds no usable Vulkan
device and a Wine is found; otherwise the device answers
`D3DPT_STATUS_NO_EXEC` as today. `D3DPT_EXEC=dxvk|wine|none` in the
environment forces one, and the adapter grows `exec=` beside `no-exec=`
(`-global d3dpt-vga.exec=wine` from the machine form's extra arguments),
so a host that has both — this Mac, the rig — runs the A/B.

### The five calls across the boundary

`d3dpt_exec.h` is the whole surface, and it was designed for exactly
this ("the QEMU device dlopens it, so QEMU stays C and the protocol
evolves without a QEMU rebuild"):

| Call / callback | In process today | Across the boundary |
|---|---|---|
| `version()` | a function | `HELLO`: the host program reports the DLL's protocol version; a mismatch refuses, as `d3dpt_exec_load.c` does today |
| `create(ops)` | loads d3d9, `Direct3DCreate9` | the host program does it at start-up; `HELLO`'s reply says whether a device exists (adapter name, Wine's version, WineD3D's renderer) |
| `attach(0/1)` | releases every object on 0 | `ATTACH` request |
| `submit(shm, size)` | parses the window, runs it, writes `ret_status`/`ret_index` into the header | `SUBMIT` request naming the window's offset in the shared file; the header is in shared memory so the reply is only the return code |
| `set_vram(ptr, size)` | a pointer | `VRAM` request naming the offset and size in the shared file; sent once at realize |
| `log(msg)` | callback | the host program's stderr, one line each, read by QEMU and re-emitted as `d3dpt: wine: …` |
| `active(on)` | callback | a flag in every reply |
| `frame(px, w, h, stride)` | callback, pixels valid during the call | the host program writes the frame into a **frame slot** in the shared file and the reply names it (`off, w, h, stride`); QEMU calls `present_ops->frame` on the mapped pointer, valid until the next `SUBMIT` — the same contract as today |
| `vram_dirty(off, bytes)` | callback, many per submit | the reply carries a bounded list of ranges (say 64); more than that collapses to one range covering them all, which is only more scanout work |

The request is a fixed 32-byte record on the child's stdin, the reply a
fixed record plus the dirty list on its stdout; `submit` is synchronous
with the BQL held, so the vCPU thread writes the request and blocks on
the reply, exactly as it blocks in the in-process call today. Wine hands
inherited Unix fds 0/1/2 to the PE program as its standard handles, so
the host program needs nothing but `ReadFile`/`WriteFile` on
`GetStdHandle` — no port, no socket, no Wine-specific API.

**Latency.** A pipe round trip is ~30–100 µs; a DP2 frame can be
hundreds of submits. Step 2 measures it on the dp2 test (the batch count
is known); if it shows, the wait becomes a **bounded spin on a sequence
counter in the shared file** (the host program spins ~1 ms on the
window's doorbell word before blocking on the pipe; QEMU spins the same
way on the reply word), which brings a busy stream to a few µs per
submit and costs an idle host nothing. Design that in only if the
measurement asks for it.

### The shared memory

Two regions have to be the same bytes in both processes: the command
window (`D3DPT_SHM_SIZE`; the top of VRAM for `d3dpt-vga`, its own RAM
region for the SysBus device) and VRAM (the adapter's BAR 0, 128 MB),
which the executor reads texels and vertices from and writes readbacks
into. QEMU gets both from a file: `memory_region_init_ram_from_fd()`
over a temporary file the device creates (`$XDG_RUNTIME_DIR` /
`$TMPDIR`, unlinked at exit; `memfd` is Linux-only and a Wine program
cannot map an fd, only a path, so it is a path on both platforms). The
host program opens the same path through Wine's `Z:` drive
(`CreateFileA` + `CreateFileMappingA` + `MapViewOfFile`, which Wine
implements as `mmap(MAP_SHARED)` of the same file), so a guest store
into VRAM is visible to the executor with no copy and a readback lands
in VRAM with none. A third, small region in the same file is the frame
slot (the largest mode's frame, 12 MB at 2048×1536×32). The file is
created only when the remote executor is the choice; the in-process path
keeps `memory_region_init_ram` and pays nothing.

On macOS the Wine process is x86_64 under Rosetta and QEMU is
arm64; a `MAP_SHARED` file is the same pages either way.

### The Wine process

- Started by QEMU at the device's realize (not at the first
  `CreateDevice`: a Wine start is 1–3 s, a first `wineboot` of a prefix
  10–20 s), with our own prefix under the data dir
  (`~/.local/share/2ksbox/wine`), `WINEDEBUG=-all`,
  `WINEDLLOVERRIDES=d3d9=b` (Wine's builtin d3d9, never a DXVK someone
  put into a prefix — that one needs the Vulkan this host lacks), the
  renderer pinned to GL in the prefix's registry
  (`HKCU\Software\Wine\Direct3D\renderer=gl`), and `D3DPT_D3D9=system`
  for the DLL's own loader: the "this host's own Direct3D 9" branch,
  which under Wine is Wine's builtin d3d9 at `C:\windows\system32\d3d9.dll`
  (the executor logs it as such).
- The executor's `system` branch already gives the device a **hidden
  window** and opens a scene for a draw made outside one (2026-09-21,
  for Windows' own d3d9), and wined3d wants exactly those two things:
  on the NULL windows the DXVK branch passes, wined3d's GL renderer
  cannot make a context current for the swap chain's back buffer
  ("Failed to set pixel format … does not belong to window
  0000000000000000") and the DLL path's frame comes out black, while
  the DDI path, rendering into its own VRAM-registered targets, is fine
  either way. Nothing is ever shown: the executor reads the back buffer
  back with `GetRenderTargetData` around every `Present`.
- Wine's d3d9 accepts what Windows' own refused (the reason the Windows
  package moved to DXVK on 2026-09-17): a draw outside
  `BeginScene`/`EndScene` is not checked (`in_scene` guards only
  depth-stencil blits and `EndScene` nesting in current `d3d9/device.c`),
  and a device on a hidden window is an ordinary device. Whether it
  *draws the same frame* is step 2's measurement, not an assumption.
- The pair `d3dpt_exec.dll` + `d3dpt-exec-host.exe` is **cross-built
  with mingw on the native host** (`scripts/build-d3dpt-exec.sh --wine`,
  into `build/d3dpt/wine/`): Homebrew's `mingw-w64` builds both on this
  Mac today (checked 2026-09-22: `d3dpt_exec.dll` 2.5 MB,
  `d3dpt-dp2-test.exe` 2.6 MB, static), Debian/Fedora have the package,
  and the Flatpak SDK does not — the two files are the same bytes on
  every host, so the Flatpak takes them as a checked-in build (the
  precedent is `firmware/vgabios-*.bin`, built by `scripts/build-vgabios.sh`
  and committed because nothing that needs them can build them) or as a
  release asset with a checksum in the manifest. Decide at step 6.
- **Which Wine.** Linux: the distro's (`wine` / `wine64` on `PATH`; Debian
  13, Ubuntu 26.04 and Fedora 43 all ship Wine 10), found by
  `player/src/companions.rs`' rule and printed by `player --companions`
  and `launcher --paths` like the Glide wrapper is. macOS: Homebrew's
  `wine-stable` cask (10.0.x, x86_64, needs Rosetta 2 — every Wine on
  Apple Silicon does today: native arm64 Wine's `winemac.drv` gets no
  OpenGL, because macOS hands the GL compatibility renderer only to
  Rosetta-translated processes; CrossOver's own arm64 preview of 2026-07
  runs x86 code through FEX for the same reason), looked for at
  `/Applications/Wine Stable.app/Contents/Resources/wine/bin/wine64`,
  `/opt/homebrew/bin/wine64`, and a Kegworks/Sikarugir engine; WineD3D
  over macOS's OpenGL 4.1 is the path CrossOver ran D3D9 on for a decade.
  **The packages bundle no Wine at first**: the launcher finds one or
  says, in the wizard's graphics note, what to install (one line, with
  the command). Bundling a trimmed Wine (the ~40 DLLs d3d9 pulls in,
  ~80 MB, against the cask's 600) is a later step with its own
  measurement; the Flatpak's is an extension point
  (`org.winehq.Wine` is on Flathub and ships a wow64 build since
  its 26.08 branch) or a from-source Wine in the manifest, which Bottles
  and Heroic both do.

### What the launcher says

`host_gpu.rs` gains a third verdict beside "accelerated" and "software":
**"Direct3D through Wine on the host"** — after the Vulkan probe says
below-floor, the probe looks for a Wine by the player's own rule and
reports its version. `graphics_note()` then reads, on a below-floor host
with Wine: "3D: Direct3D runs through Wine on this host (Wine 10.0,
OpenGL). Expect it to be slower than on a Vulkan GPU." — and without
one: "3D: no Vulkan 1.3 here. Install Wine to get Direct3D (…command…);
OpenGL and Glide games run either way." `launcher --host-check` exits
zero in the Wine case (the device is available) and non-zero only when
neither back end can run. The `host-check` check in `scripts/test.sh`
holds all three answers to what is true on the host it runs on.

### What `no-exec=on` means afterwards

Today it stands in for a below-floor host (the guest gets no Direct3D
and takes WineD3D-in-guest). After this track a below-floor host *has*
Direct3D, so `no-exec=on` becomes plainly "a host with no executor at
all" — still a valid state (no Wine installed) and still the knob that
proves the display driver survives it; `exec=wine` is the knob for the
new state. Doc 15's paragraph on `NO_EXEC` and CLAUDE.md's are updated
at step 4.

### Rejected alternatives (so they are not re-proposed)

- **Winelib / wined3d in-process.** Not possible: see above. Wine's
  PE/Unix split moved `opengl32`, `winevulkan` and the display drivers'
  Unix halves into `.so` unixlibs, but `wined3d.dll` and `d3d9.dll` are
  PE modules over `opengl32.dll`; there is no `libwined3d.so` to link.
- **A second executor of our own over OpenGL, Metal or wgpu.** ADR-013
  named it and refused it: a second implementation of D3D9 semantics.
  WineD3D *is* that implementation, twenty years in, and this track
  takes it as a running program rather than as source.
- **Native arm64 Wine on macOS, an aarch64 PE executor.** No OpenGL in
  `winemac.drv` under native arm64 (above), and Rosetta translating a
  Wine process that spends its time inside Apple's GL driver costs
  little. Revisit when Wine's macOS driver has Metal-backed GL or when
  the project's floor is a macOS with Vulkan.
- **Copying VRAM over the pipe instead of sharing it.** The DDI path
  reads vertex buffers out of VRAM per draw (protocol v9) and the
  executor writes a readback per frame; a copy per draw is a round trip
  per draw.
- **A socket instead of stdio.** Nothing here needs more than the
  child's two pipes; Wine's AF_UNIX support is recent and its winsock is
  a dependency for nothing.

### The two macOS builds (ADR-019, 2026-09-22)

Only the **community** build of the Mac app carries this path: the
14.0 floor, no KosmicKrisp ICD, the Wine executor in, a Developer ID
DMG from `scripts/package-macos.sh --community`. The **App Store** build
is macOS 26+ on Apple Silicon and never starts Wine. The Wine the
executor runs on is x86_64 on both Mac architectures — native on an
Intel Mac (the best case: no Rosetta, WineD3D on the machine's own GL),
under Rosetta on Apple Silicon — and Intel is *permitted, untested*
until an Intel Mac has run step 3. Homebrew's Wine casks are disabled
since 2026-09-01 (not notarized), so the Wine at hand on a Mac is
WineHQ's tarball from Gcenx's releases (11.17, x86_64) or CrossOver; a
native arm64 Wine exists only as CrossOver's preview (macOS 26.5+, FEX,
unpolished) and has no OpenGL in `winemac.drv`, so it is no help below
26 and not needed at or above it.

## Test loop

A pre-26 macOS **guest** on this Mac is the community build's user
(ADR-019): a UTM virtual machine of macOS 15 (Apple's Virtualization
framework, the IPSW from Apple's CDN into `build/macvm/`, created in
UTM's wizard — its AppleScript dictionary has no IPSW install — with
Remote Login on and Rosetta installed once by hand), and
`tools/macvm-wine-spike.sh <user>@<ip>` (`utmctl ip-address <vm>`) copies
WineHQ's tarball and the PE pair in, runs both host tests on Wine's d3d9
there over ssh — in the guest's GUI session through `launchctl asuser`,
which is why the guest account needs passwordless sudo: over plain ssh
there is no window server and wined3d cannot make even its probe window
— brings the frames back and diffs them against this host's DXVK frames.

**What the guest answered (2026-09-22, macOS 15.6.1, WineHQ 11.17): a
VM cannot test the GL path.** Apple's paravirtual GPU has Metal and no
accelerated OpenGL: CGL offers "Apple Software Renderer" (2.1 legacy /
4.1 core) and nothing else, to arm64 and x86_64 processes alike, and
Wine's Mac driver demands `kCGLPFAAccelerated` for its bootstrap context
(`winemac.drv/opengl.c` `init_context`; `AllowSoftwareRendering` only
widens the formats it enumerates afterwards), so Wine has **no OpenGL at
all** in such a guest and wined3d's GL renderer never starts. Wined3d's
Vulkan renderer over the bundled MoltenVK does start there
(`RENDERER=vulkan`): the DDI frame differs from DXVK's in 8 % of pixels
(max 255) and the DLL path's device creation fails on the depth format
MoltenVK lacks — a data point, not a path (ADR-007). So the community
build's GL path is tested on a **real** pre-26 macOS: on this Mac, a
second APFS volume with macOS 15 (`diskutil apfs addVolume disk3 APFS
"macOS 15"`, the installer app onto it, boot it) and, from a Terminal
there — one machine runs one macOS at a time, so not over ssh —
`tools/macos-wine-spike-local.sh` in the other volume's checkout under
`/Volumes/Macintosh HD - Data`: the host tests, the remote library, the
PE pair and the Wine tarball link nothing of Homebrew, so they run on a
bare install, and it prints the real GPU's `GL_RENDERER` for the Rosetta
process and the fps. QEMU links Homebrew and is not for that round; the
guest there comes with the packaged community build (step 4). The VM
keeps its use for the launcher and package flow, where the GPU does not
matter. Logs of the VM run in `build/macvm/spike/`.

```sh
scripts/build.sh                                  # the native stack, libd3dpt_exec_remote, and with mingw the PE pair in build/d3dpt/wine/
scripts/test.sh host                              # exec-wine: the two host tests through the remote executor (SKIP without a Wine)
build/qemu/qemu-img create -f qcow2 -b ~/vms/winxp-m7.qcow2 -F qcow2 build/xp.qcow2   # an overlay, never the image
tools/xp-driver-test.sh build/xp.qcow2 install    # once: the image's driver must accept this adapter (a driver older than
                                                  # 2026-09-12 refuses a newer one and XP falls back to VGA, silently)
EXEC=wine D3DPT_WINE=<wine> tools/xp-driver-test.sh build/xp.qcow2 d3dgame8   # the reference scene in the guest, on Wine
EXEC=dxvk tools/xp-driver-test.sh build/xp.qcow2 d3dgame8                     # the control, in process
EXEC=wine tools/xp-driver-test.sh build/xp.qcow2 probes                       # the ten DX8 DDI probes in one boot, on Wine
# the Win98 display driver's DX7 HAL on the same executor (D3D7TEST, the frame against
# build/test/dp2-test.bmp); base98-us carries a Voodoo 2 -- in the launcher's slot, addr=0x05
# (`launcherx --print-args <its machine.toml>`): without the card 3dfx's login helper fails in
# an "Error" box that takes exclusive mode from the test, and in another slot Windows finds
# new hardware and asks for a restart before the shell
STAGE=guest-tools/out/driver9x/d3d7test.exe PULL='2KSBOX\D3D7TEST.LOG 2KSBOX\D3D7TEST.BMP' \
  GUEST_CMD=$'start /w C:\\D3D7TEST.EXE 640 480 32 300' \
  EXTRA='-device voodoo2,addr=0x05 -global voodoo2.texmem=2 -global d3dpt-vga.exec=wine' \
  tools/win98-game-test.sh "$HOME/Library/Application Support/2ksbox/machines/base98-us/disk.qcow2" d3d7wine
python3 tools/bmpdiff.py build/test/dp2-test.bmp build/w98game/d3d7wine/D3D7TEST.BMP --tolerance 8
```

`D3DPT_WINE`, `D3DPT_WINEPREFIX` and `D3DPT_EXEC_REMOTE_LIB` in the
environment name the Wine, the prefix (`build/wine-prefix`, the one the
`exec-wine` check keeps) and the library, for both harnesses alike.

On this Mac, which has Vulkan, `exec=wine` is the A/B; `auto` takes Wine
only where DXVK's probe finds no device — and on such a host QEMU must
survive the probe: it did not at first (DXVK with no Vulkan loader calls
through a null pointer in `Direct3DCreate9`, and unloading the library
after a failed probe crashed too), so the executor now asks for
`libvulkan` before it tries DXVK and no probed library is ever closed.

Until the remote executor exists, **the spike is one command on a host
that has Wine and a GL** (the Linux rig, or this Mac after
`brew install --cask wine-stable`): the Windows build of the executor on
Wine's own d3d9, drawing the display driver's host-test frame —

```sh
scripts/build-d3dpt-exec.sh --windows                       # build/win/d3dpt/d3dpt_exec.dll
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -o build/win/d3dpt-dp2-test.exe tools/d3dpt-dp2-test.cpp
export WINEPREFIX=build/wine-spike WINEDEBUG=-all WINEDLLOVERRIDES=d3d9=b
D3DPT_EXEC_LIB=build/win/d3dpt/d3dpt_exec.dll D3DPT_DXVK_LIB=d3d9.dll \
  wine build/win/d3dpt-dp2-test.exe build/wine-spike/dp2.bmp           # macOS: arch -x86_64 wine64 …
tools/bmpdiff.py build/test/dp2-test.bmp build/wine-spike/dp2.bmp --tolerance 8
```

`package-windows.sh` already runs this test under Wine in the cross
container — against `dxvk_d3d9.dll`, through winevulkan; the spike is
the same run with the library name changed.

**Run on the Air, 2026-09-22** (WineHQ 11.17 staging under Rosetta,
unpacked into `build/wine/` from Gcenx's release tarball — Homebrew's
casks are disabled; prefix `build/wine-spike/prefix`; the PE pair from
Homebrew's mingw plus its `libwinpthread-1.dll`, which that toolchain
links dynamically where the Fedora cross image's does not):
- `d3dpt-dp2-test.exe` (the display driver's records): **PASSED**, the
  frame **0 of 307,200 pixels different** from `build/test/dp2-test.bmp`
  (DXVK on KosmicKrisp) — with wined3d's default renderer, which on
  WineHQ's Mac build is *Vulkan over the bundled MoltenVK*, and again
  with `renderer=gl` pinned in the prefix (`HKCU\Software\Wine\Direct3D`),
  which is the below-floor case. The prefix pins GL; the MoltenVK
  result is a data point, not a plan.
- `d3dpt-exec-test.exe` (the DLL path): with `D3DPT_D3D9=system`, so
  the executor's hidden window and scene handling are in play, every
  batch status equal to the DXVK run's, 120 frames at 863 fps under
  Rosetta against 929 for DXVK on KosmicKrisp in process, and the frame
  **0 of 307,200 pixels different** from DXVK's. (The DXVK branch on
  the NULL window draws it black under wined3d's GL renderer — above.)
  Along the way the test itself was wrong: it enabled an auto depth
  buffer and never cleared it, DXVK over KosmicKrisp starts one at 0.0,
  so the DXVK frame on the Air had been the clear colour alone all
  along, and the test checks no pixels; it clears Z now. **Step 1 is
  done on the Air; the same two commands on the rig (Linux Wine on a
  real GL, `scripts/win-cross.sh`'s wine or the distro's) close it.**

## Next steps, in order

1. **The spike** — **done on the Air 2026-09-22** (above): both host
   tests pass through `d3dpt_exec.dll` on Wine's own d3d9 with frames
   byte-identical to DXVK's, no executor change, no WineD3D refusal.
   Left: the same two commands on the rig (Linux Wine, a real GL),
   which is a run and not a question.
2. **The transport** — **landed 2026-09-22 on the host tests**:
   `d3dpt/exec/d3dpt_remote.h` (the wire), `d3dpt_exec_host.c` (the
   child), `d3dpt_exec_remote.c` (QEMU's side, `libd3dpt_exec_remote`),
   `d3dpt_exec_probe` in the executor, the loader's two-library choice
   (`D3DPT_EXEC`, the adapter's `exec=`), the shared file behind the
   adapter's VRAM (QEMU patch 73) and the SysBus window, the `exec-wine`
   check. Both host tests through the child draw frames byte-identical
   to DXVK's; the exec test's 120 frames at 553 fps through the pipe in
   copy mode against 929 in process, so the spin is not needed for the
   host tests — measure again with a guest before deciding. Left: the
   guest (step 3) is the first run of the shared-file path, since the
   host tests copy.
3. **The guest** — **XP landed 2026-09-22 on the Air**: `EXEC=wine
   tools/xp-driver-test.sh <overlay> d3dgame8` (the knob added to the
   harness) boots XP on the adapter with VRAM as a 128 MiB region of
   the shared file, the display driver draws D3DGAME8 through XP's own
   d3d8.dll, the DX8 DDI and the executor in the Wine process, and
   **frame 300 is within the rig budget** (max channel difference 8
   against the native frame, as the in-process run's is; the two guest
   frames differ from each other by at most 8 per channel, a GL against
   a Vulkan rasteriser). 600 frames in 3.4 s on Wine under Rosetta
   against 2.1 s in process on KosmicKrisp, the first second at 6 fps
   while wined3d compiles its shaders. The first two runs said nothing
   of the kind: the image's driver (`winxp-m7.qcow2`, 2026-09-08) was
   older than the version-acceptance change and refused the adapter, XP
   ran on its VGA fallback, D3DGAME8 died in a modal box and the killed
   guest lost its unflushed files — **run `install` on the overlay
   first** (its proof is the mode-set line after the restart), which
   is in the test loop above now. **Finished 2026-09-22, later the same
   day**, and it took four fixes to run a second time:
   - `install` itself blocked for good three runs out of four: setupapi
     shows one "has not passed Windows Logo testing" dialog *per unsigned
     file* (the miniport, then the display DLL after its copy), and
     DRVINST's watcher pressed for a fixed two minutes — under TCG the
     second dialog came later than that. The watcher now lives as long as
     the install call (`drvinst.c`), the harness sends DRVINST's own
     lines to COM1 and takes a screendump every 20 s while it waits, and
     `KEEP=1` leaves a machine up for a look (the install's proof was
     `tasklist /v` and `setupapi.log` typed to COM1 from a second Run
     dialog on the kept guest).
   - **The DDI probes on Wine: nine PASS, PATCHTST NOT OFFERED** — the
     same ten verdicts as the in-process run on KosmicKrisp (cube,
     stream, volume, format, bump, sprite, anisotropy, MSAA and managed
     probes all pass through the child).
   - **The Win98 machine reset itself at D3D7TEST's first read of the
     status register.** The Wine child made its Direct3D device lazily,
     at the first `create()` — inside that MMIO read, 3.5 s under Rosetta
     with the vCPU stopped — and the machine came back to the Startup
     Menu's "Windows did not finish loading on the previous attempt"
     (XP had taken the same stall the day before). The device is made at
     the library's probe now, which the adapter's realize runs before the
     guest boots, and `create()` collects it; the first cut of that made
     two devices, because the child numbers executors from 0 and 0 read
     as "none".
   - With the reset gone D3D7TEST lost exclusive mode at its first
     `BeginScene` (`DDERR_NOEXCLUSIVEMODE`, every surface lost) to a
     dialog titled "Error" — 3dfx's Glide failing at login on
     `base98-us`, whose bundle has a Voodoo 2 the harness run had left
     off the bus; the in-process control had merely won that race. The
     test logs the runtime's verdicts and the foreground window when
     `BeginScene` fails now, and the run carries the card in the
     launcher's own slot (`-device voodoo2,addr=0x05`; any other slot is
     new hardware and a restart prompt before the shell).
   - **Then the machine blue-screened at the first batch** — a fatal
     exception 0D in the VMM, half a second after the executor logged the
     batch's dropped render states. That first batch stalls the vCPU inside
     the doorbell write for as long as wined3d takes to compile its
     shaders, and the reset at the status read was the same thing with a
     longer stall: **patch 65's tick reinjection** raised a quarter second
     of owed 1 kHz ticks back to back the moment the guest could take one,
     and Windows 98's timer handler (early acknowledge, interrupts on)
     nests them until the VMM dies. `-global isa-pit.reinject=off` was the
     A/B: dead with the burst, **300 frames at 57.3 fps and the frame
     byte-identical to `dp2-test.bmp`** without. The fix is in the patch
     (paced catch-up: an acknowledge raises an owed edge only half a period
     after the last owed one, so a catch-up runs at twice the rate and
     nests one deep at most — not through a timer of its own, which the
     main loop's wakeup merges with the regular edge and left a 1 kHz
     clock at 66 % in `pit-guest-test.py`'s rate phase), and the run
     below is with it. It is not a Wine problem: any
     executor slow enough to hold a batch for hundreds of milliseconds
     would have done it, which is exactly what a below-floor host is.
   Found beside it, open: **XP with its display driver refused shows a
   black screen on the adapter** — the VGA core in a chained 256-colour
   800×600 mode (`sr4=0a gr5=50`) that renders nothing, every screendump
   of every failed install run included, while the BIOS text and DOS
   mode 13h (`tools/vga-dirty-guest-test.py d3dpt 13h`, PASS) render
   fine. Who programs that mode and what it draws is the question; it
   blinds every headless look at a guest on the fallback.
4. **The launcher and the packages** — **done 2026-09-22**: `host_gpu.rs`
   finds a Wine by the remote library's rule (`D3DPT_WINE` — a missing
   path meaning *none*, so a test can take it away — the Mac apps, the
   spike's tarball in a checkout, `PATH`), asks it `--version`, and
   looks for the PE pair (`lib/2ksbox/wine/d3dpt-exec-host.exe`,
   `build/d3dpt/wine/` in a checkout); `D3dBackend::Wine` is the third
   verdict, taken in the loader's own order (DXVK for any Vulkan device,
   software included; Wine when there is none), with its headline,
   advice and `verdict_word`, and `--host-check` exits zero through it
   (`pass_through_available`). Without a Wine the WineD3D sentences stay
   until step 6, plus the one that says which Wine to install; software
   Vulkan beside a Wine advises trying `exec=wine` too. `--paths` and
   `player --companions` print the Wine and the pair (`wine`,
   `wine-host`, `d3dpt-remote`); `companions.rs` sets
   `D3DPT_EXEC_REMOTE_LIB` and `D3DPT_EXEC_HOST` in a package. The Linux
   package stages the library and the pair (all three or none, the .exe
   kept from `ldd`), the macOS one only with `--community` (ADR-019; the
   flag is new and gates just this until the rest of the split lands),
   the Flatpak's pair waits for step 6 with the Wine question. The remote
   library's default prefix follows the data directory on macOS
   (`~/Library/Application Support/2ksbox/wine`). Doc 15's and CLAUDE.md's
   `no-exec` paragraphs read "no executor at all" now; the `host-check`
   check holds the no-Vulkan-no-Wine and the no-Vulkan-with-Wine answers
   (the second when the box has both halves), and the `capi` smoke runs
   with no Wine so the "keep the adapter" line is the one it sees.
5. **A real game** on the Wine executor on a below-floor host: the
   only such machine at hand is a Mac before 26 — the Air's own
   macOS 26 can run the A/B (`exec=wine` against KosmicKrisp) and
   measure, but the *user* of this path is on macOS 14/15, so a run
   on one of those (a VM of macOS 15 on the Air, or a borrowed machine)
   is the acceptance, with Moto Racer and FIFA 2000 as the titles.
6. **Retire WineD3D-in-guest**, in one commit, once 3 and 5 pass:
   the ISO's `WINED3D\` folders and README, `SETUP /GAME 4`/`5`, `/I 7`
   with `D3DPRE.EXE` and the `DDRAWME`/`DDSYS` switcher,
   `guest-tools/build-wrappers.sh`'s wine9x build and `patches/wine9x/`,
   `tools/xp-wined3d-test.sh` and `wined3d-sys-test.sh`, the launcher's
   advice strings, doc 04's rows, the status doc's open thread of
   2026-09-20, and CLAUDE.md's "never propose deleting the `WINED3D\`
   ISO folder" sentence, which ADR-018 supersedes. The Flatpak's Wine
   (bundle, extension, or "install it") is decided here too, from
   what step 5 measured.

## Rules

- The executor's decoder is one file set for three back ends (DXVK
  native, DXVK on Windows, Wine); nothing Wine-specific goes into
  `d3dpt_exec.cpp` beyond the hidden window. A WineD3D quirk is
  answered in the host program or documented as a row, never by
  forking the decoder.
- Every claim about a frame comes from a BMP in `build/` diffed with
  `tools/bmpdiff.py`; "WineD3D should accept it" is not a state.
- **The shared-file path is POSIX-only in QEMU, and is compiled out of
  the Windows build.** `memory_region_init_ram_from_fd` — the call that
  makes the adapter's VRAM and the SysBus window regions of the
  executor's file — lives inside `#ifdef CONFIG_POSIX` in QEMU's
  `include/exec/memory.h`, so both call sites (`d3dpt/hw/d3dpt_vga.c`'s
  realize and `d3dpt_mm.c`'s `exec_load`) carry the same guard: without
  it the Windows cross/native build fails with `call to undeclared
  function 'memory_region_init_ram_from_fd'` (2026-09-22). Nothing is
  lost there — `libd3dpt_exec_remote` is built only on Linux and macOS
  (`scripts/build-d3dpt-exec.sh`), and a Windows host below the Vulkan
  floor runs the executor in process on the system's own Direct3D 9
  (ADR-007's second amendment), so `lib->shared_alloc` is never
  resolved on Windows. A future Windows out-of-process executor needs a
  new mapping call, not this one.
- The guest-side WineD3D stack is not touched before step 6, and step
  6 is one commit.
- One TCG guest at a time on the box; end scripted Win98 runs with the
  ACPI power button (CLAUDE.md).
