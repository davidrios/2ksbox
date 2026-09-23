# OpenGLide patch queue

The host-side Glide wrapper qemu-3dfx's Glide pass-through needs, and
why it is OpenGLide. How the wrapper renders into the player's context
is doc 12 §5; the emulated Voodoo 2 that serves what the wrapper cannot
is doc 21.

`scripts/prepare-openglide.sh` applies these patches in filename order to
the pinned submodule `third_party/openglide` (the CVS mirror at
`ad9a3dd`). Like `prepare-qemu.sh` it restores every tracked file a patch
touches and re-applies the queue on each run.

## Why OpenGLide

qemu-3dfx's `hw/3dfx` is a *dispatcher*, not an implementation. At
`grGlideInit` it `dlopen`s a host `libglide2x` and looks up 183 `gr…`/
`gu…` entry points. Upstream ships that library to donors only, so
without one Glide does not work at all. OpenGLide (LGPL) is the open
implementation, and the one upstream's is derived from. It defines 121
of the 183, which is all of Glide 2.x. The other 62 are Glide 3, the
Voodoo3/Napalm `…Ext` set and the Glide 2.11 LFB API; Glide 3 titles run
on the Voodoo 2 device instead (doc 21).

`scripts/build-glide.sh` compiles the sources directly rather than
through OpenGLide's autotools, whose `configure` wants SDL 1.2, X11 and
GLU, the three things this build exists to do without. The window-less
platform layer, the entry points `hw/3dfx` looks for and the GLU
replacements live in `glidept/host/`, outside the submodule; only what
had to change inside OpenGLide is a patch. macOS builds the same sources
with forwarding `GL/` headers from `glidept/host/macos/GL/`
(`docs/build-macos.md`); the macOS wrapper is built but not exercised by
any check.

## The patches

| Patch | What / why | Drop when |
|---|---|---|
| `01-no-glu` | drops `<GL/glu.h>` and its two calls: `gluErrorString` (a log line) and `gluBuild2DMipmaps` (behind the off-by-default `BuildMipMaps`), replaced in `glidept/host/glu_shim.cpp` by a switch and `GL_GENERATE_MIPMAP`, which is enough because Glide textures are power-of-two. The runtimes we ship into have no GLU | GLU comes back |
| `02-host-entry-points` | safe to load inside QEMU. All logging goes through `GLIDE_HOST_LOG=<path>` (`-` = stderr) instead of files in the working directory written from a static constructor that answers failure with `exit(0)`; no `OpenGLid.ini` is written when none is found (one that exists is still read); the one-argument `setConfig` is removed for qemu-3dfx's `_setConfig@8` shape, implemented with `setConfigRes` and `setHostOps` in `glidept/host/hostops.cpp` | upstream grows a library-friendly logger |
| `03-sdk-header-in-c` | `sdk2_3dfx.h` compiles as C (`#include <cstdint>` and `extern "C"` branch on `__cplusplus`), for the guest test `guest-tools/src/glidetest.c` | upstream notices |
| `04-lfb-origin` | `grLfbLock` fills the `origin` out field. It left the caller's old value, and qemu-3dfx warned `LFB origin mismatch` per lock and caches the value, so a Glide 2.11 title's `grLfbBegin` could read its buffer upside down | upstream notices |
| `05-lfb-locked-swap` | `grBufferSwap` uploads a write-locked LFB before it swaps (`LfbFlushWriteBuffer(release = false)`, so the lock survives). A Glide 2 game may hold `grLfbLock(GR_LFB_WRITE_ONLY)` for its whole life (Carmageddon's 3dfx build does for its front end), and upstream uploaded only on the unlock that never comes. Without it every frame is black and the QEMU log says `LFB locked on buffer swap` | upstream notices |

## Testing

Three levels, all in `docs/testing.md`:

- the `glide-host` check (`tools/glide-host-test.cpp`), the wrapper under
  `hw/3dfx`'s own dispatcher with no guest, a locked-LFB swap included;
- `GLIDETEST.EXE` in a Win98 guest (`tools/glide-guest-test.sh`);
- real titles through `PLAYER=1 tools/win98-game-test.sh`: Rayman 2's
  Voodoo renderer, and Carmageddon's DOS 3dfx build through
  `GLIDE2X.OVL` (doc 12 §5).

## Regenerating

Run `prepare-openglide.sh`, edit inside `third_party/openglide`, then
`git -C third_party/openglide diff -- <files>` for the patch. Patches 01
and 02 both touch `GLutil.cpp` in disjoint hunks (hence the order); patch
05 is a diff against the tree after 04 (`grguLfb.cpp`), made from
before/after copies with `git diff --no-index --no-prefix a b`. Prove it
from pristine: `prepare-openglide.sh` twice, then `build-glide.sh`.

## The stacks we did not take

Kept here so they are not argued again.

- **A guest-side Glide→OpenGL wrapper** (OpenGLide's Win32 build) over
  qemu-3dfx's `OPENGL32.DLL` pass-through. It needs no host code, but
  guest code under TCG translates every Glide call, which then crosses
  the MMIO boundary as GL. qemu-3dfx has a Glide device to avoid exactly
  that.
- **A guest-side Glide→Direct3D wrapper** (nGlide, dgVoodoo2) over our
  Direct3D (docs 14, 15). It works and would translate at host speed in
  DXVK, but both are closed-source freeware and cannot go on the ISO or
  into a Flatpak; dgVoodoo2 also wants D3D11 and nGlide's D3D path misses
  Win98. As a user's own experiment on XP it costs no code (nGlide 1.05's
  `glide3x.dll` beside the game, our `D3DPT\D3D9.DLL` beside that).
- **A Glide 3 layer in OpenGLide.** Abandoned by user decision once the
  Voodoo 2 device ran 3dfx's own `glide3x.dll`. The unmerged start (the
  `wrap3x_` layer, synthetic scenes only) is the tag
  `m14-glide3-abandoned`. The sources surveyed for it:
  - 3dfx's released Glide 3 source, the authoritative spec, under a
    licence that is not GPL-compatible (read it, don't paste it);
  - dgVoodoo 1 (LGPL, Glide 2 only);
  - psVoodoo (GPL, Glide→D3D9, coverage unverified);
  - Zeckensack's, Sven's, nGlide and kjliew's donor fork, which are
    closed or unpublished.

## Emulating the chip instead

The other route is a chip emulation, which M14 took beside the wrapper
(ADR-016). The wrapper draws on the host GPU and every game can find a
hole in it; the chip is exact and complete at a software rasterizer's
speed. Doc 21 §1 weighs the two and §9 has the rasterizer's numbers.
