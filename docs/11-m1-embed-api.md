# 11. M1 design: `libqemu_embed` — QEMU in-process

Status 2026-09-02: implemented (API v3) — lifecycle, display, input, audio
ring, refresh interval, and QMP over a socketpair (`player/src/qmp.rs`, no
C changes: exactly the design below); verified on Linux and the M1 Air.
Doc 00 has the cheat sheet.

Derived from a source survey of QEMU v9.2.4 (patched tree). File:line refs
are to `qemu/` as prepared.

## Shape

One shared library per target, `libqemu-embed-i386.{so,dylib,dll}`, built by
QEMU's own meson from the existing per-target static lib
(`meson.build` ~4227: `lib = static_library('qemu-' + target, ...)` already
excludes `system/main.c`, so there is no `main()` to fight) plus our shim
`embed/libqemu_embed.c`. The player links it and drives it through
`embed/libqemu_embed.h` (bindgen). Our sources live in `/embed/` and are
overlaid into `qemu/embed/` by `prepare-qemu.sh`, like the 3dfx devices;
`patches/qemu/10-embed-api.patch` only touches `meson.build`.

## What needs no QEMU changes (the big win)

- **Lifecycle.** `qemu_init(argc, argv)` → `qemu_main_loop()` →
  `qemu_cleanup()` (`include/sysemu/sysemu.h:98-100`), called by us on one
  caller-created thread. `qemu_init` takes the BQL on the calling thread
  (`system/runstate.c:864`, thread-local ownership `system/cpus.c:515`), so
  init and the main loop **must be on the same thread**. We always pass
  `-S` so the guest is paused when init returns (otherwise `qmp_cont` runs
  before displays exist, `vl.c:2751`), hook up, then `vm_start()`.
- **Display.** Pass `-display none` and, after init (BQL held), register a
  `DisplayChangeListener` on `qemu_console_lookup_default()`
  (`ui/console.c:694`). Callbacks: `dpy_gfx_switch` (new surface; the old
  one is freed on return — never retain it, `ui/console.c:853`),
  `dpy_gfx_update` (clamped dirty rect), `dpy_refresh` (we call
  `graphic_hw_update()` — this is the pull that makes the VGA device render;
  the GUI timer only exists if some listener has `dpy_refresh`,
  `ui/console.c:108-127`), `dpy_gfx_check_format` (accept `x8r8g8b8` only in
  v1 → QEMU shadows 8/15/16/24bpp into 32bpp for us), cursor/mouse-set.
  All fire on the main-loop thread under BQL. Registration synchronously
  fires switch+update+cursor before returning. Zero-copy is real for 32bpp
  modes (surface points into VRAM, `hw/display/vga.c:1637`), shadowed
  otherwise.
- **Input.** `qemu_input_event_send_key_qcode`, `qemu_input_queue_rel/abs/btn`
  + `qemu_input_event_sync` (`include/ui/input.h`). Must run under BQL and
  is dropped while paused. We enqueue from any thread and drain in a
  bottom-half (`aio_bh_schedule_oneshot(qemu_get_aio_context(), …)`), one
  sync per batch. Scancode→QKeyCode via the exported
  `qemu_input_map_atset1_to_qcode` table. `qemu_input_is_absolute()` +
  mouse-mode notifier tell the player whether the guest wants tablet or
  PS/2 semantics.
- **VM control.** `qemu_system_{vmstop,reset,powerdown,shutdown}_request()`
  are async and thread-safe; `vm_start()` needs BQL → bottom-half.
- **QMP.** `socketpair(AF_UNIX)` in the player, pass one end as
  `-chardev socket,id=qmp0,fd=N -mon chardev=qmp0,mode=control`. Full QMP
  incl. events, monitor runs on its own iothread. No filesystem path, no
  network — good enough for "in-memory". (True pipe: `qemu_chr_open_fd()`
  on a `TYPE_CHARDEV_PIPE`, ~15 lines, if ever needed.)

  Windows has no `socketpair()`: the player makes a connected pair on the
  loopback interface and keeps it only when the accepted peer address is
  the exact address its own connecting socket was given, which no other
  process can be using at the same time. And that `N` is **not** a
  `SOCKET`. Every socket call in QEMU's Windows build is an `os-win32.h`
  wrapper that starts with `_get_osfhandle(fd)`, so `fd=` is an index into
  a C-runtime descriptor table — and which table depends on which CRT a
  module links, which the player cannot know about the library. So the
  handle crosses the boundary as a handle and **`qemu_embed_socket_to_fd()`
  (API v7) converts it inside the library**. A raw `SOCKET` passed straight
  through is refused at startup as `File descriptor 'N' is not a socket`.

## What needs patches

1. `10-embed-api.patch` — meson: `shared_library('qemu-embed-<target>',
   files('embed/libqemu_embed.c'), objects: lib.extract_all_objects(...),
   dependencies: arch_deps, link_args…)` next to the executable.
   Objects are linked directly (not via archive) so `type_init`
   constructors survive. Requires `-fPIC` (configure-qemu.sh adds it).
2. `20-embed-audio.patch` (done) — driver lives in `embed/embedaudio.c`
   (compiled into the shared lib, so no `audio/meson.build` change): the
   mixer clips straight into the caller's ring via
   `get_buffer_out`/`put_buffer_out`. Pacing (2026-09-10, the file's
   header has the whole argument): the guest is drained at its own clock's
   pace and never in a burst, because QEMU's sb16 and AC97 move the
   guest's DMA exactly as far as the mixer drains it — a burst moves the
   guest's play cursor past what its driver has written, and that was the
   crackle (`tools/audio-glitch-test.py`). A main loop late by more than
   1.5 ticks loses the excess rather than catching up (never more than 3
   ticks owed), and a ±25/10 % rate correction holds the ring's *minimum*
   over a quarter second at `out.buffer-length` (default 40 ms, player
   `PLAYER_AUDIO_MS`), the cushion under whatever period the host device
   drains it in; the player starts a stream once the ring holds a period
   plus the cushion. It replaced (2026-09-04) a design that topped the
   ring up to its target every tick and dropped whole ticks over it — 12
   clicks in 20 s against a 2048-frame device, 922 against 4096 — which
   in turn replaced a 10 ms version that never caught up after a stall
   (XP audio grew laggier the longer and harder it ran). QEMU-side
   touches: `qapi/audio.json` enum+union entry, `audio_template.h`
   per-direction case, **and `audio/audio.c: audio_create_pdos()` CASE**
   (missing it → NULL pdo → segfault in `audio_validate_per_direction_opts`).
   The player appends `-audiodev embed,id=embed0,out.frequency=<host rate>,
   out.channels=2,out.format=s16`; attach devices with `audiodev=embed0`.

## Hazards recorded

- `qemu_init` errors are `error_fatal` → `exit(1)`: validate config before.
- `os_setup_signal_handling()` installs SIGINT/SIGHUP/SIGTERM handlers
  process-wide (`os-posix.c:57`): save/restore around init in the player.
- `qemu_cleanup` is incomplete (`runstate.c:929` TODO): **one VM per process
  lifetime** — matches our launcher/player split (doc 02).
- qemu-3dfx's `graphic_hw_passthrough()` makes `graphic_hw_update` skip the
  device (`ui/console.c:147-152`) while 3D is active. Upstream then renders
  into QEMU's SDL2 window and **refuses to activate without it**
  (`sdl_display_valid()` exits the process), so with `-display none` the 2D
  path worked and any 3D title killed the VM. **Settled in M3:** patch 30
  puts those entry points behind a provider vtable and the embed library
  registers a window-less one, and since 2026-09-07 QEMU is built
  `--disable-sdl` outright — `ui/sdl2.c` is not compiled and libqemu-embed
  no longer links SDL at all.

## Embed API (v1)

See `embed/libqemu_embed.h`. Thread contract: `*_new/run/destroy` on one
thread; display callbacks on that thread (BQL held, must not block);
everything else callable from any thread.

## Player side (M1 steps)

1. `qemu-embed-sys` crate: bindgen over the header, links the shared lib
   from `build/qemu/`.
2. Player spawns the QEMU thread, gets frames via callbacks into a
   triple-buffered `Vec<u32>` staging (copy dirty rects under the callback,
   publish on `on_refresh_done`), presents through the existing wgpu path.
3. Keyboard/mouse from winit → `qemu_embed_key/mouse_*` + `flush`.
4. FreeDOS boot → Win98 boot. Then audio patch, QMP, librashader.
