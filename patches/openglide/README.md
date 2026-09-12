# OpenGLide patch queue

Applied by `scripts/prepare-openglide.sh` on top of the pinned OpenGLide
submodule (`third_party/openglide`, the CVS mirror at `ad9a3dd`) in filename
order; the script restores every tracked file a patch touches and re-applies
the queue on each run, like `prepare-qemu.sh` and `prepare-dxvk.sh`.

## Why OpenGLide is here at all

qemu-3dfx's Glide pass-through (`hw/3dfx`) is a *dispatcher*, not an
implementation: at `grGlideInit` it `dlopen`s a host-side `libglide2x` and
looks up 183 `gr…`/`gu…` entry points in it. That library is not part of
qemu-3dfx — upstream ships it to donors only ("QEMU-enhanced OpenGLide
host-side wrappers"). Without one, Glide has never worked here in any
configuration, the player and `-display sdl` alike.

OpenGLide is the open implementation, and the one upstream's is derived from
(its `setConfig(FxU32)` with the `WRAPPER_FLAG_*` bits `glidewnd.c` sets is
already here; kjliew's fork widened it to `_setConfig@8`). Of the 183 entry
points it defines **121**, which is every one Glide 2.x needs —
`grSstWinOpen`, `grBufferSwap`, `grDrawTriangle`, `grTexDownloadMipMap`,
`grLfbLock`/`Unlock`, `guTexAllocateMemory`. The 62 it lacked were Glide 3
(`grDrawVertexArray`, `grVertexLayout`, `grGet`…), the Voodoo3/Napalm `…Ext`
extensions, and the Glide 2.11 LFB API. **Glide 3 is ours since
2026-09-12** — `glidept/host/glide3.cpp`, not a patch, because nothing
inside OpenGLide had to change for it (M14, `docs/tracks/m14-glide3.md`,
and the section at the end of this file); the `…Ext` set is still missing.

The build is `scripts/build-glide.sh`, which compiles the sources directly
rather than carrying OpenGLide's autotools: its `configure` looks for SDL
1.2, X11 and GLU, all three of which this build exists to do without. The
window-less platform layer, the entry points `hw/3dfx` looks for and the two
GLU replacements live in `glidept/host/`, outside the submodule; only what
had to change *inside* OpenGLide is a patch.

**macOS builds the same sources** (2026-09-06). OpenGLide says `<GL/gl.h>`
and `<GL/glext.h>` outright -- `platform/window.h` reaches for the framework
only under `__MACOSX__`, an SDL-era define nothing sets -- and macOS has no
`GL/` directory: the framework keeps its headers under `OpenGL/`, and the
only `GL/` on the box belongs to XQuartz's Mesa, the one implementation this
must not bind to (`docs/build-macos.md`). So `glidept/host/macos/GL/` holds
a forwarding `gl.h` and `glext.h`, and `build-glide.sh` puts that directory
on the include path **on Darwin only** -- a Linux build still finds the real
headers. The `glext.h` carries what Apple's copy lacks: it stopped at
`GL_GLEXT_VERSION 8` (2003), before the `PFNGL…PROC` convention, so the
seventeen typedefs OpenGLide names (it resolves every extension through the
platform's `GetProcAddress`), `APIENTRY`, and the four
`EXT_paletted_texture` / `EXT_packed_pixels` enums `PGTexture.cpp` names
behind a runtime check for extensions macOS does not have. Nothing inside
the submodule changed for it, so it is a directory rather than a patch. The
result links against `OpenGL.framework` and `libSystem` alone -- 120
`gr…`/`gu…` exports plus `setConfig` -- but **nothing exercises it there**:
`glide-host` drives the embed backend's EGL path and stays Linux-only, and
the guest run below is a Linux one, so the macOS wrapper is built, not
proven.

| Patch | What / why | Drop when |
|---|---|---|
| `01-no-glu` | drop `<GL/glu.h>` and the two GLU calls: `gluErrorString` (one log line) and `gluBuild2DMipmaps` (behind the off-by-default `BuildMipMaps`). Replaced by `ogl_error_string` / `ogl_build_2d_mipmaps` in `glidept/host/glu_shim.cpp` — a switch statement and `GL_GENERATE_MIPMAP`, which is the driver's own downsample rather than GLU's box filter and correct here because Glide textures are already power-of-two. GLU is deprecated and absent from runtimes we ship into (`org.freedesktop.Sdk`) | never, unless GLU comes back |
| `02-host-entry-points` | the wrapper must be safe to load *inside QEMU*. Three things: (a) `GlideMsg`/`Error`/`ClearAndGenerateLogFile`/`GenerateErrorFile` write through one switch, `GLIDE_HOST_LOG=<path>` (`-` = stderr) — upstream writes `OpenGLid.log` and `OpenGLid.err` into the working directory from a **static constructor** and returns a failure the caller answers with `exit(0)`, which inside a VM process is neither wanted nor survivable, and re-`fopen`s the log per message; (b) `GetOptions` no longer *writes* an `OpenGLid.ini` when none is found — the working directory is the player's, not a game folder — it just keeps its defaults, and still reads one that exists; (c) upstream's one-argument `setConfig` is removed and its declaration widened to qemu-3dfx's `_setConfig@8` shape, because the replacement (with `setConfigRes` and `setHostOps`) is in `glidept/host/hostops.cpp` | upstream grows a library-friendly logger |
| `03-sdk-header-in-c` | `sdk2_3dfx.h` is the public Glide SDK header, and a **C** program includes it too (`guest-tools/src/glidetest.c`, the guest-side test): `#include <cstdint>` and `#define FX_ENTRY extern "C"` are both C++-only, and the second is a syntax error on every declaration in the file. Both now branch on `__cplusplus` | upstream notices |
| `04-lfb-origin` | `grLfbLock` fills `lfbPtr`, `writeMode` and `strideInBytes` of the caller's `GrLfbInfo_t` but never `origin`, which is an out field too: the caller reads back whatever was already in its own struct. It costs qemu-3dfx a warning per lock (`LFB origin mismatch` in the QEMU log, found by the first Glide guest run, 2026-09-06) and it is not only cosmetic — the dispatcher caches the value in `lfbDev->origin`, and a Glide **2.11** title's `grLfbBegin` is answered from that cache, so one lock would leave an old game reading its buffer upside down. The rows are already laid out for the origin that was asked for, so the fix is to say so | upstream notices |
| `05-lfb-locked-swap` | `grBufferSwap` draws a write-locked LFB before it swaps. A Glide 2 title may hold `grLfbLock(GR_LFB_WRITE_ONLY, …)` for the life of the program and treat the buffer as its frame buffer — Carmageddon's 3dfx build does, for its whole front end — and upstream uploaded a write buffer only in `grLfbUnlock`, which such a game never calls: every frame black, one `LFB locked on buffer swap` in the QEMU log (the dispatcher already copies the guest's shared LFB into the wrapper's buffer on every swap for exactly this case; the wrapper then dropped it on the floor). The unlock's upload half is now `LfbFlushWriteBuffer(release)`, called with `release = false` from the swap so the lock survives. Found by the first DOS Glide game through `GLIDE2X.OVL`, 2026-09-10; the `glide-host` check's locked-write case guards it | upstream notices |

## What has run on it

`tools/glide-host-test.cpp` (no guest), `GLIDETEST.EXE` (our own program in
a Win98 guest, `tools/glide-guest-test.sh`), and since 2026-09-10 a real
title: **Rayman 2**'s Voodoo renderer (`GliVd1vf.dll` over the guest's
`GLIDE2X.DLL`) at 640×480, language menu through the intro into the first
level, in the player through `PLAYER=1 tools/win98-game-test.sh` — doc 12
§5 has the run and the recipe. No `Glide Calls` complaint in the wrapper's
own log across the run.

## Regenerating

Edit inside `third_party/openglide`, then `git -C third_party/openglide diff
-- <files>` for the patch's own hunks, and prove it forward-applies from
pristine by running `scripts/prepare-openglide.sh` twice and rebuilding.
Patches 01 and 02 both touch `GLutil.cpp`, in disjoint hunks; that is why the GLU one
is first. Patch 05 is a diff against the tree *after* 04 (`grguLfb.cpp`), made
from before/after copies with `git diff --no-index --no-prefix a b`.

## The stacks we did not take

Two other ways a guest's Glide call could reach the GPU, recorded so they
are not re-argued:

- **A guest-side Glide→OpenGL wrapper** (OpenGLide's own Win32 build) on top
  of qemu-3dfx's `OPENGL32.DLL` pass-through. Needs no host code at all and
  would work today — but every Glide call is translated by guest code under
  TCG before it becomes a GL call that then crosses the MMIO boundary,
  instead of crossing once as a Glide call and being translated at host
  speed. Avoiding exactly that is why qemu-3dfx has a Glide device.
- **A guest-side Glide→Direct3D wrapper** (nGlide, dgVoodoo2) on top of our
  own paravirtual D3D (doc 14) or the XP driver's DX8 DDI (doc 15). The
  architecture is sound and the translation would run at host speed in
  DXVK — but both are closed-source freeware, so neither can go on the
  guest-tools ISO or into a Flatpak. Secondary: dgVoodoo2 wants D3D11 and
  nGlide's D3D path is XP-shaped, which misses Win98, where the Glide
  titles are. (As a *user's own* experiment on an XP machine it costs no
  code: nGlide 1.05's `glide3x.dll` beside the game and our `D3DPT\D3D9.DLL`
  beside that — Diablo II with `-3dfx` is the title it was made for. It
  just cannot ship.)

## Glide 3 — where it would come from (2026-09-10)

OpenGLide stops at Glide 2.x; the 62 entry points `hw/3dfx` looks up and
does not find are Glide 3 and the Voodoo3/Napalm `…Ext` set. No title in
hand is *blocked* on them — every Glide 3 game also has a Direct3D or
OpenGL path (Unreal, Unreal Tournament, Descent 3, Homeworld, Tribes, NFS
Porsche, Diablo II) — but several are worse there, and **Diablo II is the
one that decides it**: its Glide renderer is the one Blizzard tuned, and the
user remembers it as "a lot better" than its Direct3D one under an emulated
Glide (Sven's `glide3x` wrapper, in all likelihood). What an open Glide 3
would be built from, surveyed so it is not re-surveyed:

- **3dfx's own Glide 3 source** (released 1999 under the "3dfx Glide Source
  Code General Public License", glide.sourceforge.net, and the root of
  Mesa's old tdfx driver). The hardware driver, not a wrapper, so nothing
  to run — but the authoritative spec: every header, structure and
  semantic. The licence is not GPL-compatible; read it, don't paste it.
- **dgVoodoo 1** — source released, LGPL, **Glide 2 only** (user, 2026-09-10).
  A second reference implementation of what OpenGLide already has, not of
  what it lacks. dgVoodoo 2 stays closed.
- **psVoodoo** (GPL, Glide→D3D9, mid-2000s) may cover part of Glide 3;
  unverified. **Zeckensack's**, **Sven's** and **nGlide** are closed.
- **kjliew's donor fork of OpenGLide** implements Glide 3 — which is why the
  dispatcher already exposes all 183 names — and is not published.

**Built 2026-09-12 (M14):** the user took the wrapper route without the
86Box measurement below. It came out as a layer beside OpenGLide rather
than a patch series inside it — hw/3dfx already looks up `wrap3x_<name>`
before `<name>` for a Glide 3 guest, so `glidept/host/glide3.cpp` exports
the translated calls under those names and OpenGLide's Glide 2 exports
serve the rest, one library for both APIs. `docs/tracks/m14-glide3.md` has
the design and what is left. The survey as written before it:

So it would be **our extension of OpenGLide**, a patch series like this one,
written from the 3dfx source. The shape is smaller than "62 functions"
suggests: Glide 3 kept Glide 2's texture management, combine units, LFB and
frame-buffer semantics, all of which OpenGLide has, and added an API surface
around them — `grVertexLayout` + the `grDrawVertexArray` family (the game
describes its own vertex structure; the wrapper walks it instead of a
fixed `GrVertex`), `grGet`/`grReset` in place of the old query globals, a
context handle from `grSstWinOpen`, and the 3x variants of the texture-table
and LFB calls. Diablo II and the Unreal engine use a modest subset. A track
of its own, test title Diablo II (the user's copy is a Battle.net download,
`Downloader_Diablo2_enUS.exe`, so the data has to be fetched first), Unreal
Tournament second.

## Emulating the chip instead (recorded, not planned)

PCem's Voodoo 1/2 emulation (GPL, inherited by 86Box) is the one route that
is complete by construction: the game's own `glide2x.dll`, `glide3x.dll`,
the DOS overlay or a statically linked Glide runs unmodified against the
registers it expects, so Glide 3, every LFB trick and the odd 1996 title
with Glide compiled in all work with no wrapper to write (patch 05 could
not have happened). It is a software rasterizer with a dynamic recompiler
— the per-pixel pipeline state JIT-compiled to host SIMD — split over two
to four render threads; a Voodoo 1 was 50 Mpixel/s and a Voodoo 2 90, and
a 640×480 game needs 20–30 for 30 fps, which a modern core does, so
Voodoo 1/2 titles run at their original rates on a good machine and stop
scaling around Voodoo 3 resolutions. Two costs, and the cores are the
smaller one: our guests are one vCPU, so under TCG the guest is one host
thread and the render threads would land on cores that are idle today
(the Air's four efficiency cores included). The real cost is on the vCPU
thread — a Voodoo is programmed by MMIO register writes, dozens per
triangle, tens of thousands a frame, and under TCG each one leaves
generated code for the softmmu slow path, an address-space walk and, by
default, a round trip through the Big QEMU Lock, where PCem's CPU core
calls the handler directly in the same thread. The mechanism is the same;
QEMU's is the heavier by an order of magnitude, and two things close most
of the gap: `memory_region_clear_global_locking` on the device's regions
(the lock round trip is the largest piece), and the **Voodoo 2's command
FIFO mode**, where Glide writes commands into a memory-mapped FIFO that
the device can back with plain RAM — no trap at all, the render thread
drains it, one doorbell register per batch, the same trick qemu-3dfx's
own FIFO uses. Voodoo 1 has no such mode, so if the chip is ever emulated
it is the Voodoo 2. A `voodoo` PCI device wrapping that code, fed into the
player's frame path, is a plausible track someday and would sit *beside*
the pass-through — the wrapper for speed where it covers the game, the
chip for fidelity where it doesn't — but it is filed as "later, if a game
demands it" — **or first, if the measurement says so (2026-09-10
discussion)**. The two Glide 3 routes trade against each other: the
wrapper renders on the host GPU and is the smaller port, but every game
can find a hole in it (patch 05); the chip renders on host CPU cores at a
software rasterizer's speed — an ARM64 code generator exists in 86Box,
not in PCem — and is exact and complete, Glide 2 stragglers included,
with 3dfx's released Glide source as an open guest driver. What decides
it is one number nobody has yet: **Diablo II in 86Box's Voodoo 2 on the
M1 Air**, an evening's experiment with nothing to build. If the
rasterizer keeps up on those cores, the Voodoo 2 device is the Glide 3
track and the OpenGLide extension is shelved; if it does not, the wrapper
is the only route that uses the GPU and the sixty functions are worth
writing. Not "TCG makes it fast": the guest's FIFO writes are cheap
either way, the frame rate is the rasterizer's.
