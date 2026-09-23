# Spike A: qemu-3dfx GL output → wgpu texture (macOS first)

**What became of it.** M3 answered it (doc 12). qemu-3dfx's SDL window
was replaced by a window-less context provider in the embed library, and
guest 3D reaches the shader chain zero-copy through an IOSurface ring on
macOS and a dma-buf ring on Linux (doc 12 §4). QEMU is built
`--disable-sdl`, the Mac build needs no XQuartz (patch 70), and the
patches named below as `02-mesa-sdlgl-on-darwin` and `03` (a Caps→Ctrl
grab fix) are gone from the queue. What follows is the record of
2026-09-02.

Go/no-go for the 3D-through-CRT-shader path under ADR-005. Test machines:
the M1 MacBook Air (Metal) and the Arch box (Vulkan).

## Question

qemu-3dfx renders guest Glide/GL with host OpenGL on QEMU's threads. Can
that output land in a **wgpu texture** on the player's render thread, with
no CPU readback, so it goes through librashader like the 2D framebuffer?

## Findings

- **The Unix backend was GLX only.** The overlay's `meson.build` compiled
  only `mglcntx_linux.c`; `mglcntx_sdlgl.c` was dead code. On Darwin that
  backend dlopened `/opt/X11/lib/libGL.dylib`, so guest 3D landed in an
  XQuartz GLX drawable and XQuartz was a hard runtime dependency.
- **3D activation required QEMU's SDL2 display.** `sdl_display_valid()`
  (patched `ui/sdl2.c`) exited unless `sdl2_console` existed, and the SDL
  window was torn down and recreated to host the context
  (`sdl_gui_restart`). Under the player's `-display none` a GL title
  `exit(1)`'d the whole process; patch 04 made GL activation fail cleanly
  (Glide still exited). So M3 was not optional plumbing: the fork had to
  replace the SDL-window dependency (`mesa_prepare_window`,
  `glide_prepare_window`, `sdl_display_valid`, the fullscreen helpers in
  `include/ui/console.h`) with a window-less provider rendering to a
  texture we can import.
- **GLX on macOS failed at activation** with `BadDrawable, Major opcode
  129 (Apple-DRI)`, on Sequoia 15.7 and on Tahoe (XQuartz #446). Root
  cause: on a Cocoa SDL window `ui/sdl2.c` passes `wmi.info.cocoa.window`
  (`NSWindow*`) to the Mesa backend, which uses it as an X11 window id;
  upstream's macOS path is an X11-capable SDL2 on XQuartz. macOS was
  switched to `mglcntx_sdlgl.c` (an SDL GL context on the Cocoa window,
  Apple's OpenGL.framework, no X server), the backend M3 grew from.
- Upstream Darwin support is lightly maintained: the July 2026 sync broke
  the Darwin build (`GL_CONTEXTALPHA`), fixed by
  `patches/qemu/00-3dfx-darwin-contextalpha.patch`.

## Plan as written

1. Does qemu-3dfx run on the M1 at all (startergo's `qemu-3dfx-macos`
   arm64 build, then ours)? Confirm accelerated GL by renderer string.
2. Where does the output live: its own window/context, an FBO, or a blit
   to the VGA surface?
3. Interop per platform: macOS IOSurface-backed GL texture → `MTLTexture`
   → wgpu via `wgpu-hal` Metal; Linux dma-buf ↔ Vulkan external memory;
   Windows `WGL_NV_DX_interop2` or a shared handle ↔ D3D12.
4. Synchronization between QEMU's GL thread and the render thread. A
   texture copy is acceptable; a readback is the failure line.

Fallback ladder, never needed: a GL-side copy into an imported shared
texture → ANGLE (GL on Metal) → Zink (GL on Vulkan) → 3D bypassing the
shader chain in its own window.

## Result

- **Build validated on the M1 Air:** patched QEMU 9.2.4 (the qemu-3dfx
  overlay + patch 00, XQuartz and SDL2 from Homebrew, uv Python)
  compiles, runs, and shows `glidept`/`glidelfb`/`glideshm`/`mesapt` in
  `info mtree`.
- **Step 1 passed (Sequoia 15.7):** a Win98 SE guest with our
  msvcrt/pentium3 wrappers, `-display sdl`, the SDL/native-OpenGL backend:
  `WGLGEARS.EXE` accelerated at **500+ fps**. Presentation was janky
  unless the mouse kept moving (coupled to the SDL event cadence on
  Darwin), and the grab-release hotkey failed because a Caps→Ctrl remap
  reports RCTRL. The guest was a PnP-BIOS install, repaired in place to
  PCI-bus enumeration (doc 06).
- Steps 2–4 became M3's design (doc 12).
