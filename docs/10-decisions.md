# 10. Decision records

The project's architecture decisions, oldest first. Each record keeps
its date, its status, the decision, why, and what it cost; amendments
stay inside the record they change, dated. The other docs describe the
current state; this one keeps the *why*. The roadmap is doc 08.

| ADR | Decision | Status |
|---|---|---|
| 001 | QEMU is the base | accepted |
| 002 | QEMU runs in-process with the display | accepted |
| 003 | A libretro core is the front end | **superseded by 005** |
| 004 | Rust where possible | accepted |
| 005 | A standalone player; RetroArch dropped | accepted |
| 006 | Direct3D 8/9 through our own paravirtual device | accepted |
| 007 | The executor is DXVK everywhere; macOS on KosmicKrisp | accepted, amended 2026-09-17 and 2026-09-21 (Windows) |
| 008 | A real guest display driver, staged after the DLL device | accepted |
| 009 | The launcher and `shader-chain` are GPL-2.0-or-later | accepted, with an addendum |
| 010 | The player ships despite the GPLv2 / Apache-2.0 conflict | accepted |
| 011 | The product is 2ksbox, app ID `com._2ksbox.Launcher` | accepted, amended 2026-09-06 |
| 012 | Win98 gets the XP driver over a shared core | accepted |
| 013 | Below the Vulkan 1.3 floor, no DXVK device; no second executor | accepted, amended; points 1 and 3 **superseded by 018** |
| 014 | One launcher library, front ends draw it | accepted; "two front ends" **ended by 017** |
| 015 | The Qt front end is the shipped one | accepted; "keep `launcher/`" **reversed by 017** |
| 016 | The Voodoo 2 is emulated beside the Glide pass-through | accepted |
| 017 | The egui front end is retired | accepted |
| 018 | Below the Vulkan floor, the executor runs on Wine on the host; WineD3D-in-guest retired | accepted, retirement pending (M15 step 6) |
| 019 | Two macOS builds: App Store 26+, community at Homebrew's floor | accepted |

## ADR-001: QEMU as the base (2026-08-31)

The only open-source option covering Linux, Windows and macOS on Apple
Silicon with a working guest-3D path for both Win98 and XP (qemu-3dfx).
VMware is closed source with no ARM Mac. VirtualBox removed 3D for
pre-Win7 guests in 6.1. 86Box already does authentic-hardware emulation
and we don't duplicate it (ADR-016 later borrows its Voodoo 2 as a
device).

## ADR-002: QEMU runs in-process with the display (2026-08-31)

For gaming latency, framebuffer, input and audio must not cross a
process boundary, so QEMU gets UTM-style embed patches. The consequences
are one VM per hosting process and GPL-2.0 for anything that links QEMU.
ADR-010 measured the latency premise later and found a process boundary
affordable; the decision stands for the work a split would take.

## ADR-003: A libretro core is the front end (2026-08-31), superseded by ADR-005

A RetroArch core would have supplied CRT shaders, frame pacing, input,
audio, disc control and packaging, with the custom app demoted to a
companion launcher (machines, snapshots, disc shelf). The embed layer
was kept front-end-agnostic so a standalone player stayed possible.

## ADR-004: Rust where possible (2026-08-31)

All original code is Rust unless a hard boundary forces C:

| Component | Language | Why |
|---|---|---|
| Embed-API bindings, player, launcher | Rust | greenfield, consumer side |
| Disc model and format parsers (`libdisc`) | Rust | parser-heavy; QEMU accepts Rust (experimental since 9.2), so upstreaming is plausible |
| Tooling | Rust | greenfield |
| QEMU patches: embed API, ATAPI glue | C | inside QEMU; thin shims over libdisc |
| qemu-3dfx host patches | C | third-party, version-coupled |
| Guest wrappers and drivers | C (era toolchains) | Rust cannot target Win9x/XP |

As a consequence libmirage is a behavioural reference (doc 05), not a
linked dependency, which also drops its glib dependency.

## ADR-005: Standalone player is the front end; RetroArch dropped (2026-09-02)

Supersedes ADR-003. After hands-on time the user judged RetroArch too
buggy to build on ("the thing is so full of bugs it's not even worth
it"). The front end returns to a **standalone Rust player** (winit,
wgpu, librashader, cpal) with QEMU in-process (ADR-002), plus a
separate **launcher**.

- We own the presentation pipeline (doc 03): CRT shaders through
  librashader's wgpu runtime, frame pacing, input grab, audio.
- qemu-3dfx's target becomes "guest GL output → wgpu texture" (Spike A,
  `docs/spikes/spike-a-macos.md`) instead of a libretro hw-render
  context.
- Spike B (`docs/spikes/spike-b-libretro-crate.md`) is void, and the M0
  core is deleted. The embed API stays front-end-agnostic on principle;
  we will not maintain a libretro shell.

## ADR-006: Direct3D 8/9 for XP through our own paravirtual device (2026-09-03)

**Decision.** Thin guest `d3d9.dll` / `d3d8.dll` serialize the API into
a shared-memory command stream; a host executor runs the same D3D9
semantics natively. Design in doc 14. Guest-side WineD3D (JHRobotics'
wine9x) stays as the fallback and the DirectDraw / D3D ≤ 7 path.

**Why.** After two days of WineD3D-in-guest on XP (FIFA 2000), every fix
was in a 2015 fork with no upstream, and the structural cost is
unfixable. WineD3D does its state tracking and D3D → GL translation
inside the emulated guest at TCG speed, then ships GL calls through the
FIFO one by one. A paravirtual device moves the translation to native
host code. On Apple Silicon that is the difference between "works" and
"plays". No community D3D9 paravirt device exists (VirtualBox's is
Vista+, VMware's SVGA3D closed and Win7+).

**Rejected.** Proton (a different product: no Windows guest, no
macOS). DXVK in the guest (no Vulkan driver for XP). Fixing WineD3D
1.7.55 further.

**Licensing.** Wine is LGPL-2.1+, so copying its D3D behaviour *and
code* into the guest DLLs and the executor is allowed (those parts carry
the LGPL, ship with source). DXVK is zlib, d3d8to9 BSD-2; all combine
with GPL-2.0 QEMU. Nothing from Microsoft's SDK ships. Headers come
from mingw-w64.

**Consequences.** M4 becomes the device (doc 08). qemu-3dfx's FIFO/MMIO
model, the FXPTL/MAPMEM guest driver, the embed frame path and the
player's 3D layer are reused. C++ enters the host side behind a C shim,
and guest DLLs are C. The x87 work is unaffected (games still set PC=24
through our `CreateDevice`).

## ADR-007: The host executor is DXVK everywhere; macOS runs it over KosmicKrisp (2026-09-03, amended 2026-09-17 and 2026-09-21)

**Decision.** The executor is DXVK's d3d9 built natively as a library
beside QEMU. On macOS the Vulkan under it is Mesa's **KosmicKrisp**
(Vulkan on Metal 4, in the LunarG SDK), which needs **macOS 26**;
MoltenVK is not supported. DXVK is a submodule (`third_party/dxvk`)
plus a patch queue (`patches/dxvk/`), the same discipline as QEMU.

**Why.** Spike C (`docs/spikes/spike-c-dxvk-native-macos.md`) found that
DXVK refuses MoltenVK outright, with five required features missing, two
of them (robustBufferAccess2, nullDescriptor) unimplementable on Metal.
KosmicKrisp has all but geometry shaders, which d3d9 never uses, so a
one-line "required → optional" patch is enough. Verified 2026-09-03 on
the Air (one more optional feature, `fillModeNonSolid`, patch 05): the
reference frame matches the rig as closely as on RADV.

**Rejected.** Implementing the features in MoltenVK (harder). DXVK on
MoltenVK with dummy resources (fights DXVK's descriptor design on every
rebase). A native D3D9-on-Metal executor or a wgpu translator (~40 k
lines, and a second executor to keep bug-for-bug equal).

**Consequences.** The Direct3D path needs macOS 26; everything else
runs on older macOS (ADR-013, ADR-018, ADR-019 cover what a pre-26 Mac
gets). A window-less WSI backend (patch 04) replaces SDL2; the device
renders into the existing IOSurface / dma-buf frame path. The rig
goldens (`reference/d3d`) are the acceptance test on every driver.

**Amendment, 2026-09-17: Windows runs DXVK too** (user decision,
reversing M11's choice of the system d3d9). The executor's first run on
Windows' own d3d9 drew black frames, and a second rasteriser is what
this ADR rejected for macOS. The Windows package carries DXVK's d3d9 as
`dxvk_d3d9.dll` (patch 08: the headless WSI on Windows).

**Second amendment, 2026-09-21: Windows' own Direct3D 9 as the
fallback** (user decision: "the wine path is just not very good"). DXVK
stays the rasteriser. It is the default on every host, the one goldens
are taken with, the only one a frame is compared against. A **Windows
host below the Vulkan 1.3 floor** (ADR-013's table) gets the same
executor on `system32\d3d9.dll` instead of WineD3D in the guest, because
those cards all run D3D9 natively at full speed.

- *Why it drew black before.* Nothing had been adapted to it. The
  system d3d9 refuses four things DXVK allows, each now handled in
  `Exec::native` (`d3dpt/exec/d3dpt_exec.cpp`): a device with no window
  (a hidden 1x1 popup), a draw outside a scene (the DP2 stream has no
  BeginScene), a backbuffer read after Present (undefined under
  SWAPEFFECT_DISCARD, which is where the frames went), and a lost device.
  Two more are retried: hardware vertex processing, and a windowed
  backbuffer format other than the desktop's.
- *What picks it.* The machine form's **Direct3D** row (`bundle::D3d9`)
  writes `-device d3dpt-vga,d3d9=…`, and the launcher's Vulkan probe
  resolves `auto`, since it is the only side that can tell software
  Vulkan from real (a card's own d3d9 beats lavapipe). The executor has
  its own `auto` behind it (DXVK, then the system library), and
  `D3DPT_D3D9=dxvk|system` forces either.
- *What it costs.* A second rasteriser's driver gaps are ours (NVIDIA's
  d3d9 draws D3DFMT_L6V5U5 with no luminance, so this backend sends it
  as X8L8V8U8). What makes it defensible is an **oracle**.
  `d3dpt-dp2-test.exe` and `d3dpt-exec-test.exe` run on either backend,
  and on the user's PC both pass on both and the frames are byte-identical
  between DXVK and NVIDIA's Direct3D 9.

ADR-018 is the same answer for Linux and macOS, which have no system
d3d9.

## ADR-008: A real guest display driver is the long-term shape; staged after the DLL device (2026-09-04)

**Decision.** Keep ADR-006's DLL device, and grow a **real Windows
display driver** on the same transport and executor as milestone M7.
The driver replaces the per-game DLLs and the emulated Cirrus, and is
the only road to Vista+ acceleration should it ever be wanted. The
shared window, doorbell, record protocol (`d3dpt/d3dpt_proto.h`) and
`libd3dpt_exec` are its back end unchanged.

**Why.** Once the DLL device matched the rig, the question became how
games reach it. DLLs have structural limits:

- **Per-game installs.** A `D3D9.DLL` must sit beside every EXE
  (Windows File Protection fights a system-wide one). Games that load
  `system32\d3d9.dll` by path, go through DirectDraw 7, or check the
  DirectX version never see it.
- **Re-implementing the runtime.** State shadowing, the managed pool,
  lost devices, state blocks, software vertex processing. With a driver,
  Microsoft's runtime stays in the guest and talks to us through the
  **Direct3D DDI**, which on 2000/XP is already a serialized stream:
  `D3dDrawPrimitives2` hands over DP2 token buffers (`d3dhal.h`),
  surfaces arrive through the DirectDraw DDI, GDI through `winddi.h`.
- **The desktop becomes ours.** Any mode the CRT shader wants, no Cirrus
  limits (XP has no driver for QEMU's standard VGA), DirectDraw and D3D 7
  for free. VMware and VirtualBox prove the shape, and its cost.

**Kept in place.** The DLL device (Win98's path until ADR-012, and the
executor's harness). WineD3D in the guest as the DX7 fallback. WDDM
(Vista/7/10) is out of scope, since a D3D11 UMD is VBoxDX-sized.

**Stages.** M7a framebuffer driver (miniport + display DLL, GDI in
software on the shared window, a mode table the CRT presets pick
from). M7b DirectDraw DDI (surfaces, blits, flips; overlays refused).
M7c Direct3D DDI (the DP2 consumer, caps from the executor), the doc 04
matrix through Microsoft's own d3d8/d3d9 with no DLL in the game
folder. M7d WDDM, if ever.

**Toolchain and licensing.** mingw-w64 ships the DDI headers; display
DLLs and miniports are plain PE files built with the same cross
toolchain. ReactOS (GPL-2.0) is the reference for DDI behaviour. Nothing
from Microsoft's DDK ships.

**Consequences.** The record protocol stays driver-neutral (guest-chosen
handles, host-object resources, nothing about COM). The executor grows
DDI-shaped operations when a stage needs them, not before.

## ADR-009: The launcher and `shader-chain` are GPL-2.0-or-later (2026-09-05)

**Decision.** The launcher crates and `shader-chain` declare
`GPL-2.0-or-later`; everything that links QEMU (`player`, `qemu-embed`)
and `libdisc`, which is compiled into QEMU, stay `GPL-2.0-only`.

**Why.** The launcher's dependency tree has **Apache-2.0-only** crates
(then egui's `ab_glyph` and `glutin`, winit's `dpi`, `ring` under
`ureq`'s rustls). Apache-2.0 is incompatible with GPLv2 (the
patent-termination clause) and fine with GPLv3, so "or later" resolves
it. The launcher can do this because it **links no QEMU code**: it
writes bundles, spawns the player and talks QMP over a socket.
`shader-chain` moves with it because it is linked into both binaries.

**Rejected.** Moving the preset download out of process (leaves the
other crates). `aws-lc-rs` or `native-tls` for `ring` (OpenSSL's
licence instead). Shipping the 80 MB preset collection in packages.

**Addendum, 2026-09-05: permissive dual-licensing rejected.** `MIT OR
Apache-2.0` for our own code was tried and reverted the same day, and
everything of ours stays GPL. It fixes nothing (the player is GPLv2
because QEMU is), a permissive label on player-side code would be true
and meaningless, and the most reusable piece, the guest serializer
DLLs, is transient once M7c lands. Copyleft keeps the novel work (the
D3D protocol, the display drivers, the CD model) open, and its likeliest
reusers (86Box, DOSBox-X) are GPL already. Permissive release is a
one-way door.

## ADR-010: The player ships as a binary despite the GPLv2 / Apache-2.0 conflict (2026-09-05)

**Decision.** The player links GPL-2.0-only QEMU and has Apache-2.0-only
code in its tree, which on the FSF's reading cannot combine. **We ship
player binaries anyway**, state the position in the README and
`THIRD-PARTY-NOTICES.md` (with `COPYING`), and distribute complete
source and build scripts.

**Why the player cannot follow the launcher.**

1. *QEMU pins it to v2.* Of what an i386 softmmu build compiles, 411
   files are v2-or-later, 605 have no header, and **35 are v2-only**
   (`util/bitmap.c`, `util/qemu-sockets.c`, `hw/audio/ac97.c`, …).
   Re-check with `tools/gpl-scan.py` after a QEMU bump.
2. *The crates are not swappable.* `codespan-reporting` comes with naga
   and `rspirv` with librashader, so a clean tree means dropping wgpu
   and librashader, and the CRT chain is the product. Partial removal
   changes nothing legally.

**Why it is defensible.** Every GPLv2 program on the modern Rust GUI
stack is in this position (`winit` alone settles it). The source we
distribute is complete and buildable, which is what GPL enforcement
protects. The residual risk is a strict distribution declining to
package the player, or a QEMU copyright holder objecting.

**Rejected: `dlopen` instead of linking.** The FSF treats static and
dynamic linking alike, and our coupling is deep (one address space,
callbacks on QEMU's vCPU threads under the BQL, a shared audio ring).
The contrary reading is held in good faith and settled nowhere, and an
"everything open source" project should not lean on it.

**Kept on the table: QEMU in its own process**, the structurally clean
answer. ADR-002's latency premise, measured
(`tools/ipc-latency-spike.c`, x86-64 rig, 16 cores):

| | idle | all cores busy |
|---|---|---|
| frame notify round trip, hot loop | p50 8 µs, p99 11 µs | p50 11 µs, p99 15 µs |
| frame notify @60 Hz, cold receiver | p50 18 µs, p99 226 µs, max 459 µs | p50 17 µs, p99 35 µs, max 2.0 ms |
| buffer fd over `SCM_RIGHTS`, once per ring slot | 12–43 µs | 17–2800 µs |
| in-process callback (today) | 0.02 µs | 0.02 µs |

That is 0.1 % of a frame typically and ~1.4 % at p99, so **latency is
not what stops it**. The work does: the VGA surface through shared
memory, IOSurface handles over a mach port on macOS, and lifecycle
rules, headless dumps and the guest harnesses that assume one process.
`player/src/qemu_vm.rs` is the only module touching the embed API, so it
would be a track, not a rewrite. Re-run the spike on the Air first. A
distribution refusing the player is the signal to open that track.

## ADR-011: The product is 2ksbox; the application ID is `com._2ksbox.Launcher` (2026-09-05, amended 2026-09-06)

**Decision.** The project is **2ksbox** (the user registered
`2ksbox.com`), and `win98-xp-virt` was a working name. At first only the
packaged identity moved. **Amended 2026-09-06**: the working name is
gone everywhere, including the repository
(`github.com/davidrios/2ksbox`), the checkout, the docs and the user's
data directory.

**Why the underscore.** No segment of a D-Bus-style name may start with
a digit, and `flatpak build-init` refuses `com.2ksbox.Launcher` ("Name
segment can't start with 2"). Escaping the digit is the established fix
(`org._7zip`).

**Which name goes where.** `2ksbox` (`launcher-core/src/paths.rs::NAME`)
is the product name, used for the commands `2ksbox` and `2ksbox-player`,
the resource directories (`share/2ksbox`, `lib/2ksbox`,
`libexec/2ksbox`), the tarball, the window title. `com._2ksbox.Launcher`
(`paths.rs::APP_ID`) is the application ID, used for the desktop entry's
file name, the icon, the Wayland `app_id` that matches the two, the
Flatpak and AppStream ID.

**The data directory.** The library is `~/.local/share/2ksbox`, runtime
files under `$XDG_RUNTIME_DIR/2ksbox`. `paths.rs::data_dir()` migrates
the old directory once, with a **rename in the same parent** (atomic, never
half a library in each place), **only when the new name is absent** (two
versions side by side get a stderr line and neither is touched), and
**never fatal** (a failed move is a warning and an empty library, fixed
with one `mv`). The runtime directory is not migrated.

## ADR-012: Win98 gets the same display driver as XP, over one shared core (2026-09-06)

**Decision.** Win98/Me gets a native display driver for `d3dpt-vga`,
with desktop modes from our table, a DirectDraw HAL and a Direct3D HAL on
the same protocol and executor. Before it is written, XP's driver is split
into an **OS-independent core plus a thin per-OS layer**, XP rebuilt on
it first. Track M10, design in doc 19.

**Why a driver.** The Glide + WineD3D stack needs per-game files and
cannot accelerate the desktop. The driver needs nothing beside a game,
because 98's own `ddraw.dll` / `d3dim.dll` drive it. And the DirectX
3–7 behaviour 98 titles depend on (execute buffers, colour keys,
palettized textures, 8 bpp, the flip chain's vertical blank) is what M7
already built.

**Confirmed by step 0**, reading `vmdisp9x` / `vmhal9x`. The per-call
DDI structures match NT's field for field and the DP2 opcodes agree, so
the walker ports as it is. The DirectDraw *object* structures differ, so
a neutral surface descriptor is required. DDI 8 works on 9x, so all of
M7c is in scope. On 9x the HAL is a **ring-3 DLL in the game's
process**, which is why the core may call no OS service at all.

**Why the core, and first.** About three quarters of `d3dptdisp.c`
(3,708 lines) states facts about our adapter and protocol, not NT. A
second copy would double every protocol bump and drift at the first
one-sided fix. The split lands alone and is proven a refactor by the M7
suite being byte-identical across it. Only then does the 9x layer start
("a 9x bug on top of an unproven refactor is two bugs wearing one
coat").

**Unchanged.** The M4 DLL device stays the path wherever the driver is
not installed. The adapter should need no change for 9x. If it does,
that is a finding, not a licence to fork the register set.

## ADR-013: Below the Vulkan 1.3 floor, no DXVK device; no second executor (2026-09-06, amended the same day, 2026-09-21 and 2026-09-22)

**Status.** Amended three times. Read it with the amendments:

- *2026-09-06.* Software Vulkan counts as available (below).
- *2026-09-21.* A **Windows** host below the floor runs the same
  executor on its own `d3d9.dll` (ADR-007's second amendment).
- *2026-09-22.* A **Linux or macOS** host below the floor runs it on
  Wine on the host, and WineD3D-in-guest is retired in M15's last step
  (ADR-018). That supersedes points 1 and 3 below. Point 2 stands.

Until M15's last step, what this ADR says about the guest-side stack is
still what a below-floor host with no Wine runs.

**Decision as written.** The paravirtual device needs a **Vulkan 1.3
device** on the host, because DXVK asks for exactly that. Hosts below
it are still supported and get the path that predates the device,
qemu-3dfx's OpenGL pass-through with the Glide wrappers and
**WineD3D-in-guest**, which needs no Vulkan.

1. **WineD3D is not retired by M10.** The ISO's `WINED3D\` and `SETUP
   /GAME`'s renames stay. *(Superseded by ADR-018.)*
2. **The launcher probes and says so**, rather than a machine silently
   having no 3D.
3. **No second executor is built.** Doc 14 P0b's escape hatch (host
   WineD3D over GL, or a wgpu translator) stays open and unbuilt.
   *(ADR-018 takes the hatch with the one executor on Wine's d3d9, not a
   second executor.)*

**The bar exactly.** `third_party/dxvk` (v3.1) sets
`DxvkVulkanApiVersion = VK_API_VERSION_1_3` and enforces it at instance
creation (a pre-1.3 loader answers `ERROR_INCOMPATIBLE_DRIVER`) and per
adapter (`dxvk_device_info.cpp`: "No adapters found … A Vulkan 1.3
capable setup is required").

**Who misses it.** Good hosts for this project, not antiques. A
2012-era x86 laptop runs KVM at the right speed for these guests:

| Host | Vulkan | Why it misses |
|---|---|---|
| Intel pre-Broadwell (HD 3000/4000) | none | Mesa's `anv` starts at Gen8 |
| Nvidia Kepler (GTX 600/700) | 1.2 | the 470 legacy branch; NVK starts at Turing |
| Nvidia Fermi, AMD TeraScale (HD 5000/6000) | none | no driver |
| macOS before 26; every Intel Mac | none usable | MoltenVK unsupported, KosmicKrisp needs 26 on Apple Silicon (ADR-007) |

All have OpenGL 2.1 or better, which is all the GL pass-through wants.

**Software Vulkan is used, and warned about** (the first amendment).
The first version refused lavapipe on the user's behalf, a judgement
about someone else's hardware. DXVK ranks a `CPU` device last but never
excludes it, so the probe counts it, reports "available, in software
(slow)", and says the other path may be faster. Which wins on a box is
that box's measurement.

**Rejected.** Pinning DXVK 1.10.3 (the last Vulkan 1.1 release) for old
hosts, which means a second DXVK branch and patch queue for fewer
features. Lowering DXVK's bar, since the 1.3 features are load-bearing.
Building a GL executor now, a second implementation of D3D9 semantics
on a guess.

**Consequences.** `launcher-core/src/host_gpu.rs` is the probe. It loads
the Vulkan loader dynamically (no libvulkan is a report, not a crash),
creates an instance at `min(loader, 1.3)` with
`VK_KHR_portability_enumeration` when offered, and classifies every
device by type and `apiVersion`. `launcherx --host-check`
(`launcher_core::cli`) prints the report and exits non-zero only when
there is no Direct3D at all. Software Vulkan, and later Wine, count as
yes. The form's sentence is shared-model text, never a front end's own:
`wizard::Form::d3d9_note()` under the Direct3D picker, with
`graphics_note()` kept for a front end without that picker. It is orange
only for the software case. The `host-check` check in `scripts/test.sh`
holds it. QEMU's backstop is unchanged: with no executor the device
reports it and the machine boots normally.

## ADR-014: One launcher library; front ends draw it (2026-09-06)

**Status.** The line between core and front end stands. "Two maintained
front ends" was ended by ADR-017; which one ships is ADR-015.

**Decision.** The launcher is **`launcher-core`** plus front ends that
draw it (then `launcher/` on egui and `launcher-qt/` on Qt 6 / QML via
cxx-qt), and `launcher-capi/` is the same core as a C ABI. A front end
owns the widgets, when to redraw, the file dialog and how it confirms
something destructive. **Everything else is the core**: each window's
state machine, its derived labels, and the sentences it shows.

**Why at behaviour, not at file formats.** For a few hours the Qt build
shared only the toolkit-free modules, and four divergences had already
appeared that no compiler could see. Its wizard had no processor field,
so a DOS machine came out unthrottled. Its network checkbox did not
follow the family. The note under it said "Windows won't see a card" on
DOS machines. And a new shader profile kept its overrides in one build
and dropped them in the other. Each was a rule in a `show()` function,
copied once. The fix is having nowhere to put the second copy.

**What it buys.** Every toolkit-free debug verb is `launcher_core::cli`,
so all callers answer `--paths`, `--discs`, `--wizard-new`,
`--preview-shader` and the rest with one implementation. `launcherx`
(`launcher-core/src/bin/launcherx.rs`, 2026-09-07) is `cli::run` alone,
and the test suite drives the models through it with no toolkit built.

**Rejected.** A Rust-only core. `launcher-capi` is a thin C ABI (opaque
handles, index-addressed rows, caller-owned strings) that Swift can
import directly; `launcher-capi/examples/smoke.c` (the `capi` check) is
a working miniature front end that fails when a model's default
changes.

**Consequences.** Nothing a second front end could get differently goes
in a front end: not a default that follows the family, not a note under
a checkbox, not a combo box's labels. `launcher-qt` has its own cargo
workspace, so a root `cargo build` never needs Qt 6.

## ADR-015: The Qt front end is the one the packages ship (2026-09-07)

**Status.** Accepted. Its "keep `launcher/` as an unshipped second
front end" was reversed by ADR-017.

**Decision.** `launcher-qt` is **the** launcher. Every packager (Linux,
the Flatpak, macOS, Windows) installs it as `2ksbox`.

**Why one, and why Qt.** A product has one launcher, one set of
screenshots, one name in a bug report. Shipping both asks users a
question they cannot answer. The front ends were equal on behaviour by
construction, so the tie went to what a shipped one needs beyond
drawing. Qt gives real windows and the platform's own file dialog, and
makes decorations, HiDPI, colour scheme, accessibility and input methods
its job. It already ran as the Windows launcher (M11, after the
`std::call_once` emutls fix). And it idles, where egui drew every 16 ms.

**Costs.** The Linux tarball depends on the host's `qt6-base` +
`qt6-declarative` (bundling ~38 MB of Qt that every distribution ships
was rejected; the Flatpak is for a host without Qt). The Flatpak moved
to `org.kde.Platform` 6.10. macOS and Windows carry Qt themselves. The
shader preview does a CPU readback through a second wgpu device (doc
07; a `QQuickRhiItem` is the fix).

**Consequences.**

- `scripts/build.sh` has a `qt` stage in its default set, over its own
  workspace. A host with no Qt 6 builds everything else and can roll no
  package (the summary and `scripts/test.sh` say so).
- Every packager opens a **real window offscreen**
  (`QT_QPA_PLATFORM=offscreen` + `LAUNCHER_QT_SHOT`) and requires a PNG,
  because Qt resolves its platform plugin and QML modules by name at run
  time, invisible to every import-table check.
- On macOS `macdeployqt` runs before our dylib closure, with
  `-qmldir=launcher-qt/qml` (our QML is a Qt resource, so the scanner
  sees nothing without it).
- The Flatpak's offline `cargo-sources.json` covers both lock files
  (`scripts/gen-flatpak-cargo-sources.sh`).

## ADR-016: The Voodoo 2 is emulated, beside the Glide pass-through, not instead of it (2026-09-12)

**Decision** (user decision). 2ksbox carries a real 3dfx Voodoo 2 on the
PCI bus: 86Box's emulation (PCem's rasterizer, 86Box's x86-64 and ARM64
recompilers; GPL-2.0+), vendored **verbatim** under `voodoo/` and
wrapped as `-device voodoo2` through a shim of 86Box's platform headers,
with no fork and no edits to the vendored files (doc 21). The planned
first step (measure Diablo II in 86Box) was overtaken by doing the port
and measuring our own device.

**What it is for.** Completeness. The guest runs 3dfx's own drivers and
the game's own Glide, so Glide 3, LFB tricks and statically linked
Glide 2 come for free, and there is no translation for a game to find a
hole in.

**What it does not replace.** qemu-3dfx's Glide half (`hw/3dfx` +
OpenGLide) stays as the fast path, because the host GPU draws, where the
chip is a software rasterizer on host cores. The machine form picks.
Its OpenGL half (`hw/mesa`) is what GLQuake, Quake II, Half-Life GL and
wglgears use, and a Voodoo 2 reaches those only through 3dfx's MiniGL at
rasterizer speed. Never propose retiring `hw/3dfx`, `hw/mesa` or
OpenGLide on the strength of this device. Direct3D (docs 14/15) is a
third thing.

**Costs accepted.** Every register write is an MMIO trap with a BQL
round trip. The mitigations were to be taken if the profile asked. The
command FIFO as guest RAM has been (`ramfifo=on`, doc 21 §9). The
BQL-free region has not, since what still traps is mostly status polls.
86Box's `fatal()` returns in our shim rather than aborting. SLI and the
Voodoo Graphics type are not offered.

## ADR-017: The egui front end is retired (2026-09-13)

**Decision** (user decision). `launcher/`, the egui/eframe front end, is
deleted. `launcher-qt` is the only front end, and `launcher-core` keeps
every rule. `launcher-capi` (with `smoke.c`) and `launcherx` remain as
the core's other callers. This reverses ADR-015's "keep `launcher/`" and
ends ADR-014's two front ends. ADR-014's line is unchanged.

**Why.** Installed by nothing, opened by no check and kept alive only by
`cargo check`, it no longer proved the boundary, while costing ~2,300
lines, ~70 crates only it used (eframe, accesskit, harfrust, icu) and a
Flatpak vendoring all of them. `launcherx` through `scripts/test.sh` and
`smoke.c` as a C front end keep the boundary checked.

**What went with it.** The `--diag-*-frame` and `--pick-file` /
`--pick-folder` verbs (the Qt build's offscreen screens,
`LAUNCHER_QT_SCREEN`, are the headless frame grabs now), and the core
API only egui called.

**Consequences.** `launcher-capi` is the root workspace's one
non-default member, and `scripts/build.sh`'s `cargo check --workspace`
exists for it. `Cargo.lock`, `cargo-sources.json` and
`THIRD-PARTY-NOTICES.md` were regenerated. A second native front end
goes over `launcher-capi` or `launcher-core`, never a revived
`launcher/`.

## ADR-018: Below the Vulkan floor, the executor runs on Wine on the host; WineD3D-in-guest is retired (2026-09-22)

**Decision** (user decision: "running an old unsupported Wine version in
the guest is bad UX and doesn't make sense"). On a **Linux or macOS**
host below DXVK's Vulkan 1.3 bar (ADR-013's table), the paravirtual
device keeps running (same guest driver, same protocol, same executor)
with the D3D9 supplied by **Wine's `d3d9.dll` (WineD3D over OpenGL) on
the host**. Wine's d3d9 exists only inside a Wine process, so the
executor runs out of process. The Windows build of `d3dpt_exec.dll` runs
in a small host program under Wine, VRAM and the command window are
shared with QEMU as one file-backed mapping, and the five calls of
`d3dpt_exec.h` go over the child's stdio. The order is in-process DXVK
wherever Vulkan 1.3 exists, then Wine, then `D3DPT_STATUS_NO_EXEC` for a
host with neither. A Windows host's second choice is its own
`system32\d3d9.dll` in process (ADR-007's second amendment, written a
day earlier without knowledge of this one), so the two platforms get
one answer. The design is doc 14 §"The executor on Wine, in another
process", and the steps are in `docs/tracks/m15-wine-executor.md`.

**WineD3D-in-guest is retired.** That covers the ISO's `WINED3D\`
folders, `SETUP /GAME 4`/`5`, `/I 7` with `D3DPRE.EXE` and the
`DDRAWME`/`DDSYS` switcher, the wine9x build, and the launcher's advice
pointing at them. The retirement is **sequenced, not immediate**. It is
M15's last step, in one commit, once the host path has drawn the
reference scene within the rig budget and run a real game (measure the
replacement, then delete). Until then a below-floor host keeps what it
has.

**Why.** UX, entirely. The fallback asked the user to copy a 2015 Wine
(1.7.55) beside every game from a CD folder, with a README naming which
folder. That is an unsupported copy of what the host runs current and
maintained. Doc 19 §42–§43's per-session and system-wide workarounds
exist only because the DLLs were in the guest. On the host, whether the
executor renders through Vulkan or GL is the host's business. It was
cheap enough to decide without ADR-013's "measure the users first":

- the executor already ran on foreign d3d9s (Microsoft's, then
  `dxvk_d3d9.dll`), and `package-windows.sh` already ran its host test
  under Wine;
- its API was built for a process boundary: five calls, four callbacks,
  every pointer the window or VRAM;
- Wine's d3d9 does not refuse what Windows' did (draws outside a scene,
  a device with no window).

**Supersedes** ADR-013's point 1 (by the sequenced retirement) and
point 3 (this takes doc 14 P0b's hatch with the one executor on a second
D3D9, not a second executor, so "a second implementation of D3D9
semantics" does not apply). ADR-013's point 2 gains a third answer,
"Direct3D through Wine on the host", and "install Wine" where there is
none. The Vulkan 1.3 floor stays the floor of the DXVK back end; the
GL and Glide paths are untouched.

**Rejected.** A native wined3d `.so` (there is none; it is a PE module).
An executor of our own over GL/Metal/wgpu (a second D3D9 implementation,
which WineD3D is, twenty years in). Native arm64 Wine on macOS
(`winemac.drv` gets no OpenGL there), so the Wine process is x86_64
under Rosetta, running the existing x86_64 cross build.

**Consequences.** Wine is a *runtime* companion found by the player's
own rule and reported by `player --companions` and `launcher --paths`.
No package ships a Wine, the form's note says which to install, and the
Flatpak's shape is decided in M15's last step. mingw-w64 builds the PE
pair (`scripts/build-d3dpt-exec.sh --wine`). The A/B on a host with both
back ends is `-global d3dpt-vga.exec=wine`, and `no-exec=on` still means
no executor at all.

## ADR-019: Two macOS builds, App Store on macOS 26+ Apple Silicon and community at Homebrew's floor with the Wine executor, Intel permitted (2026-09-22)

**Decision.** One `scripts/package-macos.sh` rolls two apps:

1. **App Store.** macOS 26+, Apple Silicon only. DXVK with the
   KosmicKrisp ICD and nothing of Wine (no x86_64 helper, no Rosetta
   prompt), so its sandbox story is QEMU's JIT entitlement alone (UTM
   is the precedent). `LSMinimumSystemVersion` measures out at 26.
2. **Community** (`--community`). Homebrew's floor
   (`scripts/macos-floor.sh`; 14.0 when this was written, **15.0 since
   Homebrew dropped Sonoma on 2026-09-10**; the number follows `brew
   update`), no ICD, the M15 Wine executor pair in. It ships as a
   Developer ID-signed, notarized DMG on the GitHub release, with the
   from-source build as the alternative. It **permits Intel Macs**,
   because the Wine is x86_64 on both architectures and only this build
   starts it.

The App Store never gets a pre-26 version. It keeps one current version
per app, and two listings with different minimums are reviewed as
duplicates (guideline 4.3). The pre-26 path lives on the release page.

*As built*, `package-macos.sh` stages the Vulkan loader and the
KosmicKrisp ICD in both apps (`--community` only adds the Wine pair), so
the community app on a 26 Mac still takes DXVK. Below 26 the ICD does
not load and the probe must answer with the Wine executor (an open
check in `docs/00-status.md`).

**Why.** Mac hosts split exactly on the executor's line. On 26,
KosmicKrisp gives DXVK its Vulkan 1.3. Below it, the executor runs on
Wine under Rosetta, a payload and a prompt the store build should not
carry. One build with a run-time choice would put Wine in every 26
user's download for nothing. Splitting costs one packager flag.

**Intel Macs**, out of scope since the 2026-09-12 floor decision
(Apple Silicon only), are reopened by the user *for the community build
only*, stated plainly as **permitted, untested.** Homebrew 7.0.0
(2026-09-13) put Intel at tier 3 (bottles frozen, support ending
September 2027 as macOS 27 drops Intel), and our build is
Homebrew-based, so Intel is realistic on today's bottles and from source
after. It becomes a state the day an Intel Mac runs the reference
scene. Until then no row claims it.

**What stays.** Doc 07's "signed .app, JIT entitlement, notarized" is
the community build; the store build adds the sandbox and whatever
review asks. The store's licensing question (GPL-2 QEMU and 86Box under
store terms; UTM ships there, and the FSF says it cannot) is the user's
to weigh. The data directory and disc shelf under the sandbox
(security-scoped bookmarks) are M6's work.
