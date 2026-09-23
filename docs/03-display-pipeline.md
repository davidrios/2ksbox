# 3. Display pipeline: pixel accuracy, CRT shaders, latency, input

The player shades and shows exactly what the guest's video card outputs,
at native guest resolution and correct geometry, with a CRT look close
enough that a 1998 screenshot is hard to tell from a photo of the tube.
We own this pipeline end to end (ADR-005) with a winit window, wgpu and
librashader. This doc covers the stages, the pixel rules, mode analysis
and geometry, latency and input. How 3D frames reach the player is doc
12. The reference CRT and its photo protocol are doc 09. The player's
flags and env knobs are in `docs/development.md`, and shader profiles are
doc 07.

## Stages

```
guest VGA/SVGA device (or a 3D frame: doc 12, doc 14)
  → QEMU DisplaySurface (raw framebuffer + dirty rects, in-process)
  → [render thread] texture upload (dirty-rect aware)
  → mode analysis (display aspect, scanline count)       player/src/mode.rs
  → librashader filter chain (slang preset)              shader-chain/src/lib.rs
  → geometry stage (aspect, integer scaling)
  → wgpu present (Metal / Vulkan / D3D12)
```

The chain follows RetroArch shader semantics (original-resolution input,
final viewport params, per-pass scaling), so the libretro slang presets
(`third_party/slang-shaders`: `crt-guest-advanced`, `crt-lottes`,
`crt-royale`, …) work unmodified and any `.slangp` is accepted. Our own
presets live in `shaders/`. The one calibrated against the reference CRT
is a **shadow-mask** preset, `shaders/syncmaster-753dfx.slangp`. The
rig's monitor is a Samsung SyncMaster 753DFX with a delta dot trio
at ≈0.20 mm (doc 09), so it has no aperture grille to imitate. It is
derived from the tube's geometry and waits on doc 09's photo pass; a
Trinitron preset is still worth having as a *style*.

Open (M2): overscan crop, the curated preset pack calibrated against the
rig's photographs, and an answer for presets with no scanline-count
parameter (below).

## Pixel accuracy rules (all testable)

1. **Never scale before the shader.** The shader input is the exact guest
   framebuffer (640×480, 800×600, 320×200…), never a pre-stretched
   surface. (QEMU's VGA breaks this upstream for mode 13h; see below.)
2. **Non-square pixels.** 320×200 is a 4:3 picture with 1:1.2 pixels. The
   geometry stage applies the mode's display aspect, never
   width/height. Same for 640×400, 720×400 text (9-dot characters),
   360×240 and friends.
3. **Double-scan awareness.** Real VGA double-scans low-res modes (320×200
   is scanned as 400 lines). Scanline shaders need the *scanline count*,
   not the framebuffer height, and mode analysis states it through the
   preset's parameters.
4. **Integer scaling.** The picture's height is a whole multiple of its
   scanlines; the width follows from the aspect. Optional overscan crop,
   off by default.
5. **Mode-change fidelity.** Mode analysis re-runs on every surface
   change; no garbage frames or stretched leftovers during a transition.
6. **Colour fidelity.** Palettized modes are expanded by the guest device;
   the surface is sRGB end to end (XRGB8888 uploads as BGRA8, no swizzle).

## Mode analysis

`player/src/mode.rs` turns a framebuffer size into what it meant on a
monitor of the era: the display aspect, the number of lines the CRT
scanned, and whether the CRTC double-scanned. It prints one line per mode
change:

```
[display] mode 320x200 VGA 320x200 (mode 13h) — 4:3 picture, pixel aspect 0.833, 400 scanlines (double-scanned)
[shader] mode parameters vga_mode=1 inter=800
```

The table is an exception list, not a lookup. An unlisted size has
square pixels, which is right for every SVGA mode and for a modern
widescreen one, and identical to 4:3 for every square-pixel 4:3 mode.
The table carries the VGA's 200-, 240-, 350- and 400-line modes, whose
pixels are not square; the other entries are there for their names, for
the sweep, and as the place a correction measured against the rig goes.

Two things are rules, not entries:

- **Double-scanning.** The CRTC sets its bit below ~300 lines.
- **The VGA raster** (`vga_raster`). A size at one of the four VGA widths
  (320, 360, 640, 720) and at most 480 lines is a 4:3 picture whatever its
  line count. QEMU's text path reports `rows × cheight` with the last
  partial row dropped, so a 400-line raster with a 12-line cell (XP's
  text-mode setup) arrives as **720×396**. Without the rule it is drawn
  as an unlisted 1.818:1 picture, the XP installer stretched across the
  window.

### Geometry

The viewport is the largest rect of the mode's display aspect that fits,
with its height an integer multiple of the mode's **scanlines**, not its
rows. 320×200 in a 2400-line surface is 6 pixels per scanline, and would
be 5.5 per row. Every whole scale of the scanlines is a whole scale of the
rows too.

The rect is rounded to **whole pixels**, size and origin. The aspect
correction makes widths fractional (320×200 at 1x is 533.33 wide), and a
fractional viewport samples on a grid that moves with the window, so the
picture crawls while the window is dragged. Rounding costs at most half a
pixel of aspect, far inside the sweep's 0.5 %.

Below 1x there is no whole scale left and the fit would shrink the
guest's pixels, so the window's minimum inner size is the 1x picture in
physical pixels, the displayed size (320×200 → 534×400). It is
re-applied on every mode change and clamped to the monitor.

### The geometry stage's one moment

Everything above is decided when a decider changes and only read while a
frame is drawn. `Gpu::guest_surface_changed` answers a new picture size.
It re-runs mode analysis, re-applies the minimum size, hands the preset
its scanline count and re-fits the rect. `Gpu::resize` answers a new host
surface and redoes only the fit. Both write one held rect (`Gpu::geom`);
`viewport()` reads it and the draw derives nothing.

The trigger is the surface's own texture, not QEMU's `on_switch`. QEMU
announces a switch up to one refresh tick before the first frame of the
new mode exists, and re-fitting there draws the old pixels in the new box
for a tick, the stretched leftover rule 5 forbids. The texture is
(re)created by exactly the three things that change what is on screen:
the framebuffer's upload (`ensure_texture`), a 3D slot taken or dropped
(`use_slot`), and a slot re-imported at another size (`import_slot`). So
analysis, fit, the chain's output size and the preset's parameters move
together with the pixels they belong to.

### Scanline count

A preset derives its scanline count from the input height or guesses
from a threshold, and both are wrong here. Mode analysis sets
crt-guest-advanced's own parameters. `vga_mode` ("VGA Single/Double Scan
mode") switches it from its console interlace guess to the two VGA cases,
and `inter` picks between them (double-scan when `inter` is above the
source height). Its default of 375 is a guess; mode analysis knows.

Measured through the real chain (`--mode-sweep`, scanlines counted in the
drawn frame; `PLAYER_MODE_PARAMS=0` is the control):

| Mode | Tube | Mode analysis | Control |
|---|---|---|---|
| 320×200 | 400 | **400** | 200 |
| 320×240 | 480 | **480** | 240 |
| 640×200 | 400 | **400** | 200 |
| 640×350 | 350 | **350** | 350 |
| 640×400 | 400 | **400** | 200 |
| 720×400 | 400 | **400** | 200 |
| 640×480 | 480 | **480** | 240 |
| 800×600 | 600 | **600** | 300 |
| 1024×768 | 768 | **768** | 384 |

The control is wrong two ways. Below 375 lines it draws one scanline per
guest row (200 where the tube scanned 400); above, it decides the source
is interlaced and draws one field's worth (640×480 at 240). Only 640×350
is right, by accident.

**QEMU has usually double-scanned the mode before we see it.** On
`-vga cirrus` a mode 13h screen arrives as a **640×400 surface** (QEMU's
VGA doubles both axes; measured with `TEXTCAL.COM`, doc 09). Vertically
and in aspect the answer is the same (400 scanlines, pixel aspect 0.833),
but the shader sees 640 columns where the tube had 320, so `h_sharp`
and the mask act on half-width pixels. Undoing that means detecting the
doubling or patching QEMU's VGA. Neither is done, and neither should be
without a photograph of the real tube to say it matters. The double-scan
branch still earns its place for a genuinely short surface (`d3dpt-vga`
at a 200- or 240-line mode, a 3D path) and the sweep exercises it.
**720×400 text is not doubled.**

**Presets without the parameters.** crt-lottes and crt-royale derive
their scanline period from `OriginalSize` and expose no override, and
rule 1 forbids handing them a pre-doubled surface. The player says so
once and leaves them at their defaults:

```
[shader] this preset exposes no scanline-count parameter (vga_mode, inter): a double-scanned
mode will be drawn with one scanline per guest row instead of the two per row the tube drew
```

Per-mode `.slangp` variants, or a pack limited to presets that can be
told, are open along with the pack.

### Screenshots

**Ctrl+Alt+S** shoots the *guest's* frame. It reads back the texture QEMU
published, at the mode's own size and before the geometry stage and the
chain. That is the picture to compare with a golden BMP, a native run or
another emulator. An imported 3D slot is shot the same way. Files land in
`PLAYER_SHOT_DIR` (default: the working directory) as `2ksbox-NNNN.png`.
The shaded window content is what `PLAYER_DUMP_OUT` writes.

### Sampling outside the picture

A curved preset samples outside the frame's edges, and what comes back
must be **transparent black**, the slang default `clamp_to_border`.
librashader's wgpu runtime silently downgrades such samplers to
`clamp_to_edge` on a device opened without
`ADDRESS_MODE_CLAMP_TO_BORDER`, smearing the outermost row and column
over everything outside the tube. So the chain's device is opened with
it (`shader_chain::required_features`, used by the player and the
launcher preview). The player logs `[shader] clamp-to-border sampling: …`
at startup.

## The mode sweep

`player --mode-sweep <dir>` runs the display path over every mode in the
table plus one unlisted size, with no guest and no QEMU. It puts a
geometry test image (a circle drawn in display space, round only when the
aspect is applied; one-pixel lines; SMPTE bars) through the real chain at
each size and checks it for on-screen aspect, fit, whole-pixel scanline
pitch, the parameters reaching the preset, and the scanline count; a PNG
per mode lands in `<dir>`. It is the `mode-sweep` check (~2 s). Two
details make it mean something:

- It renders to a **fixed 3200×2400 surface** and never acquires a
  swapchain image. The result must not depend on the compositor, and an
  occluded window (a test behind a terminal) blocks the second acquire
  forever under FIFO.
- The scanline count comes from the **pitch between the first and last
  band**, since the pitch need not be a whole number of pixels. Below
  three output pixels per scanline the preset draws a flat field, so the
  count is not checked there (1152×864 and up, square-pixel modes whose
  count is their height).

## Latency budget

Target: **≤ 1 host frame added** between guest frame completion and
photons at 60 Hz, measured.

- **In-process handoff.** The display listener publishes surface + dirty
  rects; the render thread uploads immediately. No encode, no copy chain.
- **Present mode.** Mailbox where available, else FIFO;
  `desired_maximum_frame_latency = 1`.
- **Refresh mismatch.** Guests run 70 Hz (VGA text/DOS) and 60/75/85 Hz
  SVGA on 60/120/144 Hz/ProMotion hosts. The newest complete guest frame is
  presented at host vsync, never resampled. VRR passthrough is a later
  nicety.
- **Acquire before sampling.** The QEMU refresh tick wakes the event loop
  (`EventLoopProxy`); the redraw first acquires the swapchain image, *then*
  samples the newest guest frame, uploads and presents. Sampling first
  aged every frame by a vblank whenever the queue was saturated. The
  16 ms tick is slightly faster than 60 Hz, so on Metal it always was
  (≈32 ms publish→present on the M1 Air, 2.5 ms after).
- **Occlusion.** An occluded or minimized window may get no presents
  (Wayland frame callbacks, macOS occlusion); the loop skips on
  `Occluded`/`Timeout` and never lets QEMU stall behind it. Per-frame work
  that must not stall (importing zero-copy slots) runs on the wake event.
- **Measurement.** `PLAYER_LATENCY=1` reports publish→present-return; with
  vsync its floor at 60 Hz is uniform 0–16.7 ms (p50 ≈ 8). Guest
  draw→publish (0–`PLAYER_REFRESH_MS`) is before that window.
- **Quiet unless something is off** (user request). The per-100-frames
  counter is `PLAYER_REFRESH_LOG=1`, beside `PLAYER_LATENCY` and
  `PLAYER_CURSOR_LOG`. What prints unasked is each mode change and
  anything that went wrong.

## Input path

winit events go to QEMU's input injection directly on the event thread.

**Pointer.** Absolute (USB tablet) for the desktop, relative (PS/2 plus a
host cursor grab) for mouselook. Which one is the bundle's
`seamless_mouse` (doc 07's "Seamless mouse" checkbox). With the tablet the
player never grabs and the guest's hardware cursor is the host cursor;
without it a click takes the pointer and Ctrl+Alt+G gives it back. The
player follows the guest (`mouse_is_absolute`), so a machine can be
switched without touching it.

**Keyboard.** A key goes to the guest by **where it sits** (winit's
physical key), since the guest has a layout of its own. The exception is
the keys a host keymap option moves (xkb's `ctrl:swapcaps`, `ctrl:nocaps`,
`caps:escape`, `altwin:swap_alt_win`). Control, Shift, Alt, AltGr, Super,
Caps Lock and Escape go as the host reads them (`keymap::as_host_reads`),
so a Caps Lock the host made Control is Control in the guest. The press's
answer is kept for the release. When the window loses focus the player
releases every key the guest still holds (`lift_all_keys`,
`player/src/main.rs`); without it Cmd+Tab sent the Windows-key press to
the guest and its release to the next app, and the guest kept Win down.

**Host shortcuts go to the guest while the window has focus**, grabbed or
not (`player/src/kbcapture.rs`), because a Windows machine is on the
tablet and never grabs, and its Start menu is the Windows key. winit has
no keyboard grab, so each windowing system gets its own:

- **Wayland.** `zwp_keyboard_shortcuts_inhibit_manager_v1`, one inhibitor
  for the window's life. The compositor applies it only while the surface
  has focus, and its own `--inhibited` bindings are the user's way out.
- **X11.** An active `XGrabKeyboard` while focused, which beats the window
  manager's passive grabs on Super.
- **Windows.** The keyboard is registered for raw input with
  `RIDEV_NOHOTKEYS`. While a window of this process is in front the shell
  does not act on its keyboard hotkeys (both Windows keys, every Win+
  shortcut, Ctrl+Esc), and the key still arrives as an ordinary
  `WM_KEYDOWN`, reaching the guest down the same path as every other key.
  No program gets the *system* hotkeys (Alt+Tab, Alt+Esc, Alt+F4,
  Alt+Space, Ctrl+Alt+Del, Win+L), so they stay the host's. Don't go back
  to a `WH_KEYBOARD_LL` hook. The first version used one, and Windows
  called it for every key on the machine *except* while the player's own
  window was focused: zero calls, not late ones. A hook in a third
  process went blind too while the player's was installed. Measurement
  ruled out `LowLevelHooksTimeout`, re-arming, the hook thread's timer,
  the window itself, a Windows rule (a 60-line program's hook sees its
  own window's keys), integrity levels and exploit-protection
  mitigations. The cause was never found. The user confirmed raw input on
  a real keyboard. `PLAYER_KEYBOARD_LOG=1` prints the registration and
  every focus change.
- **macOS.** Nothing is needed. Cmd reaches the app already (Cmd+Tab
  would need an event tap and the Accessibility permission).

**Ctrl+Alt+K** toggles the capture for the rest of the run (off drops it
outright, on builds a new one; the title bar says when the host has its
shortcuts). `PLAYER_KEYBOARD_CAPTURE=0` starts a run with it off.

**The player's own chords.** Ctrl+Alt+Del is the host's everywhere, so
**Ctrl+Alt+Shift+D** is the guest's (Shift let go, Delete pressed, and
released with D). **Ctrl+Alt+Shift+F** is windowed full screen
(borderless, on the window's monitor) and back. **A close with Alt held
asks first**, since Alt+F4 reaches the player whenever the host has its
shortcuts and a window close stops the machine. The player draws the
question itself (`player/src/prompt.rs`, the VGA 8×16 font blended over
the finished picture), because Linux has no message box that works in the
Flatpak and over a full-screen window. Enter, Close or a second Alt+F4
closes; Esc or Back returns; nothing reaches the guest while it is up.
The title bar's button does not ask.

## 3D and the pipeline

Guest 3D (qemu-3dfx's GL and Glide, the Direct3D executor, the Voodoo
2's VGA-surface frames) goes through the same librashader chain as 2D.
The design target is zero-copy, with IOSurface on macOS, dma-buf into
Vulkan on Linux (doc 12 §4). Windows and a Linux host without the Vulkan
extensions read frames back instead, which works but is the path to
leave. 2D correctness never depends on it.

## Testing

- `--mode-sweep` (above) and `PLAYER_DUMP_OUT` (the shaded frame,
  headless, works while occluded).
- Real-CRT calibration: `build/crtcal-render` writes doc 09's patterns as
  BMPs, `player --shader <preset> --calib <dir>` shades them for holding
  against photographs of the rig's tube (doc 09 has the protocol).
- Not built yet: golden-image boots to known screens compared pixel-exact
  with shaders off, and a mode-cycling test floppy.
