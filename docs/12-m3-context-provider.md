# 12. M3 design: window-less GL context provider for qemu-3dfx

Status 2026-09-02: steps 1–2 done on Linux — patch 30 (vtable), patch 31
(weak native backend), `embed/mglcntx_embed.c` (EGL surfaceless, pbuffer =
FBO 0, glReadPixels on swap), `embed/embedfx.c` (provider), embed API v4
(`on_3d_active`, `on_3d_frame`), player publishes 3D frames on swap.
Verified without a guest by `tools/embed-3d-test.c` (drives the backend in
mesapt_mm.c's order: activation callbacks, 640×480 frame, correct
orientation) and with the real thing: Win98 wglgears in the player on the
Linux dev box — 420 fps at 800×600 through the readback, VGA desktop back
on exit. macOS backend done and verified on the Air (CGL context without a
drawable + FBO stand-in for the default framebuffer, binding 0 redirected
via patch 32's `MesaGLSetFunc`; WGL pbuffers emulated with FBOs in the same
context; all GL/CGL resolved through the framework handle because the
build also links XQuartz's Mesa libGL): Win98 wglgears in the player,
`GL 2.1 Metal / Apple M1`. **Steps 1–2 complete on both platforms.**
**Zero-copy on Linux done (2026-09-03):** the backend allocates a ring of
three linear ARGB8888 GBM buffers, imports them into GL as EGLImage
textures and blits FBO 0 into the next one (Y-flipped) on every swap;
each buffer's dma-buf is offered once (embed API v5 `on_3d_dmabuf`) and
the player imports it into wgpu through `wgpu-hal` Vulkan
(`VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`,
`player/src/dmabuf.rs`); per frame only the slot index travels
(`on_3d_frame_ready`). Frames are sampled straight from the guest's
buffers: Win98 wglgears 575–600 fps (was 420–450 with readback). Sync is
`glFinish` before the hand-off for now (a fence fd is the refinement).
`tools/embed-3d-test.c` checks the ring by mmap'ing the dma-bufs; the
readback path stays as the fallback (macOS, or no Vulkan extensions).
**macOS zero-copy done, verified on the Air (2026-09-03):** IOSurface ring
bound to rectangle textures with `CGLTexImageIOSurface2D`, flipped blit
from the stand-in FBO, embed API v6 `on_3d_iosurface`; the player wraps
the surface in a Metal texture (`player/src/iosurface.rs`). **§4 complete
on both platforms.**
**Glide (§5) done on Linux, 2026-09-06:** the host-side wrapper is ours now
(OpenGLide, `third_party/openglide` + `patches/openglide`, built by
`scripts/build-glide.sh`), it renders into the same window-less context as
the Mesa path, and `tools/glide-host-test.cpp` drives the whole thing
without a guest. **A Glide guest ran the same day:** `GLIDETEST.EXE`
(`guest-tools/src/glidetest.c`, on the guest-tools ISO) in Win98 in the
player, headless through `tools/glide-guest-test.sh` — the whole chain, and
the guest checks its own pixels back through `grLfbLock` rather than
trusting the host to look at them: `glidetest: 4 cases, 0 failed` (clear,
triangle, re-clear, close/reopen). It paid for itself immediately with
patch `04-lfb-origin`: OpenGLide's `grLfbLock` never filled the caller's
`lfbInfo->origin`, which the dispatcher caches and answers a Glide 2.11
title's `grLfbBegin` from. **The wrapper compiles on macOS too** (a
forwarding `<GL/gl.h>` / `<GL/glext.h>` in `glidept/host/macos/`, on the
include path on Darwin only, because the framework's headers live under
`OpenGL/` and the only `GL/` on a Mac is XQuartz's Mesa) — built and linked
against `OpenGL.framework` alone, but run by nothing there: `glide-host` is
the EGL path and stays Linux-only. **A Glide title plays, 2026-09-10:
Rayman 2** (its own `GliVd1vf.dll` Voodoo renderer over the guest's
`GLIDE2X.DLL`, on a copy of the `claude98` machine — Win98 on `d3dpt-vga`,
DirectX 9.0c, the game hand-staged off the user's disc with the disc in the
drive) in the player through `PLAYER=1 tools/win98-game-test.sh`: the
game's own `GXSetup.exe` lists "0 Glide2 (1.0.0) Voodoo Graphics Glide 2
Driver" and probes all eight resolutions of `glidewnd.c`'s table through
`grSstWinOpen` on the host, and the game then runs at 640×480 from the
language menu through New Game, the intro cutscene and the first level's
opening — textured, fogged, subtitled, on the zero-copy path, 233 frames
shot at one per 300 presented, no complaint in the wrapper's log or the
dispatcher's. Two things the run taught: the game refuses a hand-written
`ubi.ini` (`Graphics Dll not found, run install`, and it resets the section
to `Choose = 1`) even with the same five `GLI_*` keys `GXSetup` writes —
probably the `Choose=1` marker `GXSetup` leaves, not proved — so the setup
program is driven by mouse (`CLICKS=`, the harness's PS/2 walk with the
cursor read back from the adapter); and a QMP screendump shows only the
game's empty desktop window while Glide presents, which is what the
player's `PLAYER_SHOT_EVERY` shot exists for. Rayman 2 is also the first
title on a `d3dpt-vga` machine to switch between our display driver and
the Glide device: the desktop comes back after every close. **And a DOS Glide game, the same day: Carmageddon's `3DFX.EXE`** (the
Max Pack's 3dfx build, a DOS/4GW program) from a Win98 DOS box on the same
copy of `claude98`. The DOS binding is qemu-3dfx's own `GLIDE2X.OVL`
(`wrappers/3dfx/ovl`, an LE overlay: the game's Glide stub loads it by
name and resolves 126 upper-case entry points from it, and the overlay
maps the pass-through device itself through DPMI 0x800, so it needs no
VxD and serves pure DOS and a 9x DOS box alike). It was never built here
before — `build-wrappers.sh` skipped it — and now is, with the Open Watcom
the 98 display driver already needs; `SETUP.EXE` copies it to the Windows
folder on 9x (upstream's own instruction, and the folder is on the PATH),
and a DOS machine puts it next to the game. The first run drew every
frame black and left one `LFB locked on buffer swap` in the QEMU log:
Carmageddon locks the back buffer for writing once and treats the LFB as
its frame buffer for its whole front end, the dispatcher copies the
guest's shared LFB into the wrapper's write buffer on every swap for
exactly that case — and OpenGLide drew a write buffer only in
`grLfbUnlock`, which such a game never calls. Patch
`05-lfb-locked-swap` makes `grBufferSwap` draw it too (the lock kept), the
`glide-host` check gained a locked-write case that fails without it, and
the game's main menu came up at 640×480 (`build/w98game/c3dfx2/shots`).
Not reached headless: a race. The DOS build spends minutes in text mode
first (a password prompt, then loading), so every scripted key landed
before the menu existed; a run that waits for the first 640×480 frame
before pressing anything is the next step, or a hand.
Refinement still open: fence-based sync on both platforms instead of
`glFinish`; a macOS `glide-host` check and the Windows build of the
wrapper; hand play (both runs above are headless, so nothing has *played*
a level yet); a Glide game on the **DOS family** proper (a FreeDOS machine
with the overlay next to the game — nothing has tried it, and DOS/4GW's
DPMI host is not Windows').

Source survey of the patched tree (hw/mesa, hw/3dfx, ui/sdl2.c); file:line
refs are to `qemu/` as prepared by `scripts/prepare-qemu.sh`. **Read the
`ui/sdl2.c` half as history:** the seam this doc designed is in place, and
since 2026-09-07 QEMU is built `--disable-sdl`, so that file is present in
the tree but never compiled and the embed library is the only provider.

## Facts that shape the design

- **All host GL runs on the vCPU thread under the BQL.** The Mesa device is
  an MMIO handler (`mesapt_mm.c:2213`); every GL call, context creation
  (`MGLCreateContext`) and present (`MGLSwapBuffers`) happens there. The
  main-loop timer callbacks that touch GL (`sched_wndproc`,
  `deactivateOneshot`, `dispTimerProc`) also hold the BQL. There is no GL
  thread; the BQL is the mutex. The SDL glue creates the context on the main
  loop and *unbinds* it (`ui/sdl2.c:931`) so the vCPU can claim it.
- **The guest frame lives in FBO 0 of the window's default framebuffer.**
  `MesaBlitScale` (`mesagl_blit.c:228-321`) upscales by `glCopyTexImage2D`
  from FBO 0 and redraws; `MesaRenderScaler` rewrites viewports only when
  `framebuffer_binding == 0`. Keep "FBO 0 = the screen" semantics: make FBO
  0 itself offscreen (pbuffer / CGL pbuffer or IOSurface drawable) rather
  than binding an app FBO.
- **Compatibility profile is mandatory** — guests are GL 1.1–2.1
  fixed-function. QEMU's own `qemu_egl_init_ctx()` hardcodes the core
  profile (`ui/egl-helpers.c:613`), so we create our own context. On macOS
  compat GL is 2.1 only (`ui/sdl2.c:877-880` core/compat switch via
  `DispTimerMS`).
- **The UI seam is exactly 11 functions** declared in
  `include/ui/console.h:481-494` and defined only in `ui/sdl2.c`:
  `mesa_{prepare,release}_window`, `mesa_renderer_stat`,
  `mesa_gui_fullscreen`, `mesa_cursor_define`, `mesa_mouse_warp`,
  `glide_{prepare,release}_window`, `glide_window_stat`,
  `glide_gui_fullscreen`, `glide_renderer_stat`. The handshake: vCPU calls
  `mesa_prepare_window(msaa, alpha, 0, cwnd_fn)`; the provider must
  eventually call `cwnd_fn(swnd, nwnd, opaque)`, which sets `wnd_ready`;
  the guest spins on MMIO `0xFB8` (`glwnd_ready()`) until then.
  `mesa_gui_fullscreen(sizev)` is called on every swap and must be
  synchronous: `sizev[0..1]` = guest 2D surface size, `sizev[2..3]` = target
  size; making them equal disables the in-QEMU scaler (we scale in wgpu).
- **Backends are GLX (`mglcntx_linux.c`) on Linux and — since patch 02 was
  dropped with SDL — macOS as well; `mglcntx_sdlgl.c` is built by nothing**; `MGLCreateContext/MGLMakeCurrent/
  MGLSwapBuffers` are the only window-system-specific parts; the rest
  (`MGLFuncHandler`, pbuffer emulation, `MGLUpdateGuestBufo`) is reusable.
  `MesaGLGetProc` resolves extensions (`glXGetProcAddress` /
  `SDL_GL_GetProcAddress`); the base table comes from `dlopen(libGL)`.
- **3D-active signalling** is `graphic_hw_passthrough(con, on)`
  (`ui/console.c:129`), which makes `graphic_hw_update` skip the VGA
  device. Nothing tells a display listener; `con->ui_info.passthrough` is
  private. We add an explicit edge notification to the embed API.
- **Glide presents inside a third-party wrapper** (`libglide2x.so` creates
  its own context on the window handle, `glide2x_impl.c:796-806`) -- and
  qemu-3dfx does not contain that wrapper at all: `hw/3dfx` is a dispatcher
  that `dlopen`s one and looks up 183 entry points in it, and upstream ships
  the library to donors only. So section 5 is two problems, not one: *having*
  an open host-side Glide implementation, and making it draw without a
  window.

## Design

1. **Patch `30-3dfx-ui-vtable`:** turn the 11 entry points into a vtable
   registered by the active display (`ui/sdl2.c` registers the SDL one at
   init; default = "no provider": Mesa refuses contexts as in patch 04,
   Glide reports). Fixes the latent NULL derefs in `sdl_gui_fullscreen` /
   `sdl_renderer_stat` too.
2. **`embed/mglcntx_embed.c`** (selected by meson instead of the GLX/SDL
   backend when building the embed library): copy of the GLX backend with
   the window-system calls replaced —
   - Linux: EGL on a DRM render node (reuse `egl_rendernode_init`), own
     context with `EGL_OPENGL_API` + compatibility profile, **EGL pbuffer**
     drawable at guest resolution so FBO 0 stays valid; after
     `MesaBlitScale`, blit into an exportable texture and
     `egl_get_fd_for_texture()` → dma-buf.
   - macOS: CGL (not NSOpenGL — no main-thread requirement). Implemented
     without a drawable at all: an FBO over shared renderbuffers plays the
     default framebuffer and `glBindFramebuffer(…, 0)` is redirected to it
     in the dispatch table (CGL pbuffers are deprecated; `FBO 0 = the
     screen` semantics survive because the scaler is inert at equal sizes).
     Legacy (2.1 compatibility) profile by default; core 3.2/4.1 when the
     guest asks via `wglCreateContextAttribsARB`. The IOSurface export
     attaches an IOSurface-backed texture to that FBO later.
   - `MGLSwapBuffers` = flush + fence + publish handle + notify; never block
     the vCPU on the consumer. 2–3 buffer ring with fences.
   - Delete `XOpenDisplay(NULL)`; `MesaGLGetProc` → `eglGetProcAddress` /
     CGL symbol lookup.
3. **`embed/ui_embed_ctx.c`:** the 11 entry points for the embed provider:
   `mesa_prepare_window` creates the drawable on the main loop (BH), calls
   `cwnd_fn`, unbinds; `mesa_gui_fullscreen` returns target == guest size
   at first (scaler inert), `mesa_cursor_define`/`mouse_warp` unchanged but
   sourced from `qemu_console_lookup_by_index(0)`; `renderer_stat` keeps
   `graphic_hw_passthrough` and raises the new embed callback.
4. **Embed API v4:** `on_3d_active(bool)` and `on_3d_frame(handle, w, h,
   fence)` display callbacks; handle = dma-buf fd (Linux) / IOSurface id
   (macOS). Player: import into wgpu via `wgpu-hal` (Vulkan external memory
   / Metal IOSurface) and feed the librashader chain instead of the VGA
   texture while 3D is active. Step 0 for bring-up: a CPU readback path
   (`glReadPixels` into the staging buffer) to validate the whole handshake
   before the zero-copy import — explicitly a stepping stone (doc 03's
   "readback is the failure line" is about the shipped design).
5. **Glide (done on Linux, 2026-09-06).** The wrapper is **OpenGLide**
   (LGPL, `third_party/openglide` at `ad9a3dd`), pinned as a submodule with
   a two-patch queue (`patches/openglide/README.md`) and built by
   `scripts/build-glide.sh` -- 121 of the 183 entry points `hw/3dfx` looks
   up, which is all of Glide 2.x. Glide 3 is our own layer over it since
   2026-09-12 (`glidept/host/glide3.cpp`, M14, `docs/tracks/m14-glide3.md`):
   the same library, reached through the `wrap3x_` names hw/3dfx prefers
   for a `glide3x.dll` guest; the Voodoo3 `Ext` set is still not
   there. It is the implementation upstream's own wrapper is derived from,
   so `glidewnd.c`'s `WRAPPER_FLAG_*` word already means something to it.

   The handshake is **reversed** rather than extended. Upstream hands the
   wrapper a window (or an `SDL_Window*`, if it signed itself `'SDL2'`) and
   the wrapper makes a context on it; instead, patch 33 gives the wrapper
   *our* context through a new `QemuFxUiOps::glide_host_ops` ->
   `GlideHostOps` table (`glidept/glide_host.h`: `begin` / `present` /
   `end` / `get_proc`), passed to the wrapper's optional `setHostOps`
   export at load time. A wrapper without the symbol, or a display that
   registers no `glide_host_ops` (`ui/sdl2.c`), is upstream unchanged -- so
   no new signature word was needed and `cwnd_glide2x` is untouched: the
   provider passes the same pointer as both the SDL and the native handle,
   and the wrapper ignores it.

   OpenGLide's own windowing seam is four functions with four call sites,
   which is why this is small: `glidept/host/window.cpp` replaces it with
   "the host made a context current" and "the frame is done". The context
   is `embed_gl_fx_begin`'s -- separate from the Mesa `ctx[0]`, since a
   guest can hold both, on the same offscreen drawable, resized to the
   Glide resolution rather than the guest's 2D mode. `grBufferSwap` reaches
   `publish_frame` exactly as `MGLSwapBuffers` does, so the dma-buf ring
   and the shader chain come for free.

   `glide_gui_fullscreen` returns 1 deliberately: it stops `glidewnd.c`
   upscaling a 640x480 game to the desktop's width, which would hand the
   player a frame the CRT presets are not calibrated for (doc 03), and it
   silences the wrapper's stderr fps counter.

   Two stacks deliberately not taken -- guest-side Glide to GL over the
   `OPENGL32.DLL` pass-through, and guest-side Glide to Direct3D over doc
   14/15 -- are argued in `patches/openglide/README.md`.

   **The wrapper has to be in the package, and nothing else says so**
   (2026-09-07). `hw/3dfx` finds it by `dlopen`ing a name, not through an
   import table, so a package that ships everything else and not this one
   file has a guest whose `grGlideInit` finds nothing -- and no linker,
   `ldd` or loader check anywhere can notice. `package-linux.sh` stages it
   into `lib/2ksbox/` (the Flatpak builds it in the sandbox first, against
   the runtime's libGL, since the tarball's copy is linked to the host's),
   the macOS app has carried it since 2026-09-06, and the packaged player
   names it to QEMU through `QEMU_GLIDE_LIB` in `player/src/companions.rs`.
   The check is the staged binary's own answer -- `player --companions`,
   which prints what that rule resolved -- because a script that restates
   the layout is exactly what disagrees with it later. `PACKAGE=<tree>
   tools/glide-guest-test.sh` then runs the guest battery out of the
   package with nothing pointed at the wrapper by hand.

## Order

vtable patch -> embed provider on Linux with readback -> dma-buf import ->
macOS CGL/IOSurface -> Glide. All done on Linux, guest included
(`tools/glide-guest-test.sh`), and a Glide *title* plays (Rayman 2,
2026-09-10, above); the wrapper builds on macOS but nothing has run it
there, and it has no Windows build.
