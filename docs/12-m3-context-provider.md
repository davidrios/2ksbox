# 12. Window-less GL context provider for qemu-3dfx (M3)

qemu-3dfx renders guest OpenGL and Glide with host OpenGL, and upstream
assumes an SDL window to do it in. The embed library has no window: the
player owns the screen (doc 03), and QEMU is built `--disable-sdl`. This
doc is the provider that replaces the window — an offscreen context whose
frames reach the player's shader chain — plus the zero-copy ring (§4),
the Glide wrapper (§5) and the Windows WGL rule. The Direct3D path is
doc 14; the Voodoo 2's frames go through the ordinary VGA surface (doc 21).

## State

| | Linux | macOS | Windows |
|---|---|---|---|
| GL context | EGL surfaceless, pbuffer as FBO 0 | CGL, an FBO stands in for FBO 0 | WGL pbuffer |
| Frames to the player | dma-buf ring (zero-copy) | IOSurface ring (zero-copy) | readback |
| Glide wrapper | built, `glide-host` check, games run | builds, never run | no build |

Verified: Win98 wglgears in the player at 575–600 fps on Linux through the
ring (420–450 by readback), `GL 2.1 Metal / Apple M1` on the Air;
`TESTS\GLPROBE.EXE` on a Windows host reads the host's own renderer
through the pass-through. GLQuake on a Windows host has not been tried
yet.

## Facts that shape the design

File:line refs are to `qemu/` as prepared by `scripts/prepare-qemu.sh`.
`ui/sdl2.c` is in the tree but no longer compiled; read what is said about
it as the upstream shape we replaced.

- **All host GL runs on the vCPU thread under the BQL.** The Mesa device
  is an MMIO handler (`mesapt_mm.c`); every GL call, context creation
  (`MGLCreateContext`) and present (`MGLSwapBuffers`) happens there, and
  the main-loop timers that touch GL (`sched_wndproc`,
  `deactivateOneshot`, `dispTimerProc`) hold the BQL too. There is no GL
  thread; the BQL is the mutex.
- **The guest frame lives in FBO 0.** `MesaBlitScale` upscales by
  `glCopyTexImage2D` from FBO 0 and `MesaRenderScaler` rewrites viewports
  only when `framebuffer_binding == 0`, so "FBO 0 = the screen" is kept:
  FBO 0 itself is made offscreen rather than an app FBO bound.
- **Compatibility profile is mandatory** — guests are GL 1.1–2.1
  fixed-function. QEMU's `qemu_egl_init_ctx()` hardcodes core profile, so
  we create our own context. On macOS compat GL is 2.1 only.
- **The UI seam is 11 functions** (`include/ui/console.h`):
  `mesa_{prepare,release}_window`, `mesa_renderer_stat`,
  `mesa_gui_fullscreen`, `mesa_cursor_define`, `mesa_mouse_warp`,
  `glide_{prepare,release}_window`, `glide_window_stat`,
  `glide_gui_fullscreen`, `glide_renderer_stat`. The handshake: the vCPU
  calls `mesa_prepare_window(msaa, alpha, 0, cwnd_fn)`; the provider must
  call `cwnd_fn(swnd, nwnd, opaque)`, which sets `wnd_ready`; the guest
  spins on MMIO `0xFB8` until then. `mesa_gui_fullscreen(sizev)` runs on
  every swap and must be synchronous; returning target size == guest size
  makes the in-QEMU scaler inert (we scale in wgpu).
- **`MGLCreateContext` / `MGLMakeCurrent` / `MGLSwapBuffers` are the only
  window-system-specific parts** of `hw/mesa`; `MGLFuncHandler`, pbuffer
  emulation and `MGLUpdateGuestBufo` are reusable. The native backend
  (`mglcntx_linux.c`, GLX) is linked **weak** (patch 31) so
  `embed/mglcntx_embed.c` overrides it inside the embed library only; on
  macOS it is a backend that only refuses a context (patch 70), so the Mac
  build needs no XQuartz.
- **3D-active signalling** is `graphic_hw_passthrough(con, on)`, which
  makes `graphic_hw_update` skip the VGA device and tells no display
  listener, so the embed API gets an explicit edge.
- **Glide presents inside a third-party wrapper** that makes its own
  context on the window handle — and qemu-3dfx does not contain it:
  `hw/3dfx` `dlopen`s a `libglide2x` and looks up 183 entry points, and
  upstream ships that library to donors only. So §5 is two problems:
  having an open host-side Glide, and making it draw without a window.

## Design

1. **Patch `30-3dfx-ui-vtable`:** the 11 entry points become a vtable
   registered by the display (`QemuFxUiOps`). With no provider Mesa
   refuses contexts cleanly (patch 04) and the VM keeps running — which is
   what a bare `qemu-system-i386` does now.
2. **`embed/mglcntx_embed.c`**, the backend built into the embed library,
   a copy of the GLX backend with the window-system calls replaced:
   - **Linux:** EGL on a DRM render node, our own compatibility-profile
     context, an EGL pbuffer at guest resolution so FBO 0 stays valid.
   - **macOS:** CGL (no main-thread requirement) with no drawable: an FBO
     over shared renderbuffers plays the default framebuffer and
     `glBindFramebuffer(…, 0)` is redirected to it in the dispatch table
     (patch 32's `MesaGLSetFunc`); WGL pbuffers are emulated with FBOs in
     the same context. Legacy 2.1 by default, core 3.2/4.1 when the guest
     asks through `wglCreateContextAttribsARB`. Every GL/CGL/IOSurface
     symbol is `dlsym`'d from the OpenGL.framework handle, never linked:
     the build once also linked XQuartz's Mesa libGL, and a symbol bound
     there silently no-ops with no GLX context.
   - **Windows:** WGL through libepoxy, a pbuffer drawable — see "The WGL
     rule" below.
   - `MGLSwapBuffers` = blit + publish + notify, never blocking the vCPU
     on the consumer.
3. **`embed/embedfx.c`**, the provider: the 11 entry points for the
   embed library. `mesa_prepare_window` creates the drawable and calls
   `cwnd_fn`; `mesa_gui_fullscreen` returns target == guest size;
   `renderer_stat` keeps `graphic_hw_passthrough` and raises the embed
   callback.
4. **The embed API's 3D callbacks and the zero-copy ring** (below).
5. **Glide** through our own OpenGLide build (below).

### 4. Frames to the player

The callbacks (`embed/libqemu_embed.h`): `on_3d_active(bool)` on the
edge; `on_3d_frame(pixels, w, h)` for the readback path; and for
zero-copy `on_3d_dmabuf` (Linux) / `on_3d_iosurface` (macOS), each slot
offered once, then `on_3d_frame_ready(slot)` per frame so only the index
travels. A front end that declines an offer keeps the readback path.

- **Linux:** a ring of three linear ARGB8888 GBM buffers imported into GL
  as EGLImage textures; each swap blits FBO 0 (Y-flipped) into the next.
  The player imports each dma-buf into wgpu through `wgpu-hal` Vulkan
  (`VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`,
  `player/src/dmabuf.rs`). The frontend gets its own `gbm_bo_get_fd`,
  since a Vulkan import takes ownership of the fd, and a declined offer
  is closed.
- **macOS:** an IOSurface ring bound to rectangle textures with
  `CGLTexImageIOSurface2D`; the player wraps each surface in a Metal
  texture (`player/src/iosurface.rs`).
- **Sync** is `glFinish` before the hand-off; a fence is the open
  refinement.

**A ring buffer can stop being written through, and the ring remakes
it.** Seen as a fast flicker in GLQuake on Win98 — every third frame the
same frozen picture. GL reads back every frame blitted into the slot,
while the dma-buf's own memory, which the frontend samples, keeps the
frame it held first. `zc_probe()` catches it: every so often, before the
blit, it writes a known colour into the slot through GL and reads the
buffer's memory back with `gbm_bo_map`; a slot that disagrees is freed
and made again, and the new one holds. The schedule is dense while the
ring is young and one present in 512 after, because a buffer goes bad
early and never recovers (GLQuake: one slot, thirteen presents in; one
repair, then every frame distinct at 72 fps; wglgears never trips it).
Knobs (`EMBED_ZC_PROBE`, `EMBED_ZC_HEAL`, `EMBED_ZC_SLOTS`,
`EMBED_ZC_CHECK`, `EMBED_ZC_MARK`, `PLAYER_ZC_IMPORT`,
`PLAYER_ZERO_COPY=0`) are in `docs/development.md`. `EMBED_ZC_CHECK`,
which compares the GL and CPU views of a *frame*, misled twice: while the
picture does not change, a slot not written through looks like one that
is. The known-value probe has no such blind spot.

The cause is open. What it is **not**, each measured:

- **The frontend.** `PLAYER_ZC_IMPORT=0` takes every offer and imports
  nothing (declining would turn the ring off), and the slot still goes
  bad, 94 of 95 samples: the divergence is GL against `gbm_bo_map` inside
  QEMU's process.
- **The import's parameters or its use.** `tools/zc-vulkan-test.c`
  imports the ring exactly as `dmabuf.rs` does, with no guest and no
  wgpu: clean, with `--use=copy`, `--use=shader` and `--threaded` too.
- **The GL calls the guest makes.** `FuncTrace,1` gives GLQuake 42 calls
  and wglgears 22; the 27 only GLQuake makes are all in `zc-vulkan-test
  --draw=all`, clean.
- **The frame it breaks on.** `FuncTrace,2` puts the break in GLQuake's
  first 3D frame (a level load: 934 `glTexImage2D`, 330 `glDrawBuffer`
  toggles); `--draw=load` does the same volumes, clean. Nor allocation
  pressure (`--draw=alloc`, 256 MB), drawable size or stride, the render
  scaler, the ring's size, fd ownership, or a settling delay.

What is left is mesapt's own host path — the decoder's texture uploads
through shared memory, the vertex-array cache, mapped buffers.

**Driving the backend without a guest.** The `MGL*` backend can be
driven right after `qemu_embed_new`, with the BQL held, in this order:
`InitMesaGL` → `MGLTmpContext` → Choose/SetPixelFormat →
`MGLCreateContext(MESAGL_MAGIC)` → `MGLMakeCurrent(MESAGL_MAGIC, 0)` →
draw → `MGLSwapBuffers`. `tools/embed-3d-test.c` does exactly that,
checks the activation callbacks and orientation, and draws several frames
per slot, requiring each slot's own memory to follow them — one blit per
slot only proves the ring was wired, and a bad slot's first blit does
land, which is how the frozen slot stayed green for months.

### 5. Glide

The wrapper is **OpenGLide** (LGPL, `third_party/openglide`, pinned at
`ad9a3dd`), with a patch queue (`patches/openglide/README.md`) and a
window-less platform layer in `glidept/`, built by `scripts/build-glide.sh`
into `build/glide/libglide2x.so` (`QEMU_GLIDE_LIB`). It provides 121 of
the 183 entry points `hw/3dfx` looks up — all of Glide 2.x. Glide 3 and
the Voodoo3 `Ext` set are not there and will not be: Glide 3 titles run
on the emulated Voodoo 2 (doc 21), and the Glide 3 layer written for this
wrapper was abandoned (tag `m14-glide3-abandoned`). It is the code
upstream's own wrapper derives from, so `glidewnd.c`'s `WRAPPER_FLAG_*`
already means something to it.

- **The handshake is reversed.** Upstream hands the wrapper a window and
  the wrapper makes a context on it; patch 33 instead hands the wrapper
  *our* context through `QemuFxUiOps::glide_host_ops` → `GlideHostOps`
  (`glidept/glide_host.h`: `begin` / `present` / `end` / `get_proc`),
  passed to the wrapper's optional `setHostOps` export at load. A wrapper
  without the symbol, or a display with no `glide_host_ops`, is upstream
  unchanged. OpenGLide's own windowing seam is four functions with four
  call sites, which `glidept/host/window.cpp` replaces.
- The context is `embed_gl_fx_begin`'s, separate from the Mesa `ctx[0]`
  (a guest can hold both), on the same offscreen drawable resized to the
  Glide resolution. `grBufferSwap` reaches `publish_frame` exactly as
  `MGLSwapBuffers` does, so the ring and the shader chain come free.
- `glide_gui_fullscreen` returns 1 on purpose: it stops `glidewnd.c`
  upscaling a 640×480 game to the desktop's width (the CRT presets are
  calibrated for the guest's mode, doc 03) and silences the wrapper's
  stderr fps counter.
- **Two OpenGLide fixes games needed:** `04-lfb-origin` (`grLfbLock`
  never filled `lfbInfo->origin`, which the dispatcher caches for a Glide
  2.11 title's `grLfbBegin`), and `05-lfb-locked-swap`: Carmageddon locks
  the back buffer once and treats the LFB as its frame buffer for the
  whole front end, and OpenGLide drew a write buffer only in
  `grLfbUnlock`, which such a game never calls — black frames and one
  `LFB locked on buffer swap` in the log. `grBufferSwap` draws it now,
  the lock kept.
- **DOS Glide** goes through qemu-3dfx's own `GLIDE2X.OVL`
  (`wrappers/3dfx/ovl`, an LE overlay the game's Glide stub loads by name
  and resolves 126 entry points from). It maps the pass-through device
  itself through DPMI 0x800, so it needs no VxD and serves pure DOS and a
  9x DOS box alike. `guest-tools/build-wrappers.sh` builds it with Open
  Watcom; `SETUP.EXE` copies it to the Windows folder on 9x (on the PATH),
  and a DOS machine puts it next to the game. A Glide 2 game linked
  statically (a few 1996 titles) is the one kind it cannot serve.
- **The wrapper builds on macOS** — a forwarding `<GL/gl.h>` /
  `<GL/glext.h>` in `glidept/host/macos/`, on the include path on Darwin
  only, because the framework's headers live under `OpenGL/` and the only
  `GL/` a Mac may have is XQuartz's — linked against `OpenGL.framework`
  alone, but nothing runs it there: `glide-host` is the EGL path and stays
  Linux-only.
- **It must be in the package, and nothing else says so.** `hw/3dfx`
  finds it by `dlopen` name, so no linker or `ldd` check can see it
  missing, and the guest's `grGlideInit` just finds nothing.
  `package-linux.sh` stages it into `lib/2ksbox/` (the Flatpak builds it
  in the sandbox against the runtime's libGL), the macOS app carries it,
  and the packaged player names it through `QEMU_GLIDE_LIB` in
  `player/src/companions.rs`. `player --companions` is the check, and
  `PACKAGE=<tree> tools/glide-guest-test.sh` runs the guest battery out
  of the package.

Two guest-side stacks were not taken — Glide to GL over the
`OPENGL32.DLL` pass-through, and Glide to Direct3D over docs 14/15 — for
reasons in `patches/openglide/README.md`.

**What has run.** `tools/glide-host-test.cpp` (the wrapper through
`hw/3dfx`'s own dispatcher, a clear, a triangle with the upper-left
origin checked, and a write lock held across a swap) is the `glide-host`
check. `GLIDETEST.EXE` in Win98 in the player checks its own pixels
through `grLfbLock`: `glidetest: 4 cases, 0 failed` (clear, triangle,
re-clear, close/reopen). **Rayman 2** plays from its language menu into
the first level at 640×480 on a `d3dpt-vga` Win98 machine, and the
desktop comes back after each switch between our display driver and the
Glide device. Its `GXSetup.exe` must write `ubi.ini` — the game refuses a
hand-written one (`Graphics Dll not found, run install`), probably over
the `Choose=1` marker, not proved — so the harness drives it by mouse
(`CLICKS=`). A QMP screendump shows only the empty desktop while Glide
presents; `PLAYER_SHOT_EVERY` is how a Glide frame is shot. **Carmageddon's
`3DFX.EXE`** (DOS/4GW) from a Win98 DOS box reaches its 640×480 main menu;
a race was not reached headless because the DOS build spends minutes in
text mode before the menu, and scripted keys landed too early.

## The two moments a frame is presented

`MGLSwapBuffers` is one. The other is a **flush with the front buffer
selected**: a program rendering into `GL_FRONT` never swaps, and on a real
window it does not need to. Wine's ddraw presents the DirectDraw primary
exactly that way, so on our offscreen drawable those frames went nowhere
— a black screen with the guest running behind it (doc 19 §44).

The backend therefore interposes four entry points in the guest's
dispatch table (`MesaGLSetFunc`, patch 32): `glDrawBuffer` and
`glReadBuffer`, which map a `GL_FRONT`/`GL_BACK` selection on the guest's
framebuffer 0 onto whatever plays it here (`GL_COLOR_ATTACHMENT0` for
macOS's FBO, `GL_BACK` — or `GL_FRONT` if single-buffered — for the EGL
and WGL pbuffers), and `glFlush`/`glFinish`, which publish while the front
buffer is selected. Only the guest's calls take the hooks; ours go
straight to GL, so nothing recurses.

## The WGL rule

On Windows the backend's entry points come from **libepoxy**, which
resolves each one lazily at its first call — and WGL answers
`wglGetProcAddress` only while a context is current on the calling
thread. An ARB call with nothing current therefore does not fail, it
**faults**: ACCESS_VIOLATION, the whole process, and a log that ends at
the line before.

`plat_open()` deliberately ends with `wglMakeCurrent(NULL, NULL)` (a WGL
context is current on one thread at a time, and the thread that opens the
backend is not always the one that draws), so the next ARB call,
`wglChoosePixelFormatARB` in `plat_choose`, was made with nothing current.
That is how the first GL guest on Windows, GLQuake, took the player down
with `glcntx: ChoosePixelFormat()` as the last line of `player.log`. A
40-line repro of the three steps — bootstrap context, un-current, one ARB
call through epoxy — dies identically.

1. **Every ARB call in the Windows backend borrows a context** when the
   caller has none (`wgl_borrow_ctx` / `wgl_return_ctx` in
   `embed/mglcntx_embed.c`) and restores exactly what it found —
   including "nothing", which the rest of the file relies on. A new ARB
   call must do the same.
2. **`wgl-probe.exe` (`tools/wgl-probe.c`) cannot catch this.** It resolves the ARB entry
   points by hand with its own context current, so it passes where the
   backend faults. It answers "will this driver give me an offscreen
   pbuffer at all", not "does our dispatch survive".

With the borrow the chain works on the user's PC: `GLPROBE.EXE` in Win98
logs `glcntx: pixel format 12: alpha 8 depth 24 stencil 8` → `MESAGL
drawable ready` → `drawable 800x600`, then the host's strings through the
pass-through (`NVIDIA GeForce RTX 3090/PCIe/SSE2`, `4.6.0 NVIDIA 616.64`)
and a clean `DLL unloaded`.

## Order

vtable patch → embed provider on Linux with readback → dma-buf import →
macOS CGL/IOSurface → Glide → the Windows WGL backend. Open:

- fence-based sync instead of `glFinish`, on both zero-copy platforms;
- the frozen ring slot's cause (§4);
- a Glide wrapper build for Windows (M11's cross build has no stage;
  `glide` is "(not shipped)" in that package), and a macOS `glide-host`
  check plus a Glide guest on the Air;
- hand play of a Glide title (every run above is headless), and a Glide
  game on the **DOS family** proper — a FreeDOS machine with the overlay
  next to the game, where DOS/4GW's DPMI host is not Windows'.
