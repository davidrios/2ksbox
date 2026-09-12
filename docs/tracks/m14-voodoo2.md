# Track: M14 — the Voodoo 2 device (doc 21)

The handoff for a session working on the emulated 3dfx Voodoo 2: 86Box's
rasterizer as a QEMU PCI device, `-device voodoo2`. Read
`docs/00-status.md` first for the global picture and the track rules,
then this file, then doc 21 (the design: why a chip beside the
pass-through, the shim, threads, timers, the display path, what to
measure), then `patches/openglide/README.md` §"Emulating the chip
instead" for the argument the track was opened on.

Opened 2026-09-12 on the user's decision. Branch `track/m14-voodoo2`,
worktree `.claude/worktrees/m14-voodoo2`. **A second session is working
on the OpenGLide side (Glide 3 in the high-level wrapper) at the same
time**: this track does not touch `third_party/openglide`,
`patches/openglide/`, `glidept/` or `hw/3dfx`; the two meet only in the
machine form, when both are pickable (next steps, 1).

## Scope and files (this track owns them)

- `voodoo/` — everything: `86box/` (the vendored sources + `UPSTREAM`),
  `shim/` (86Box's platform headers reimplemented), `voodoo_shim.c`,
  `voodoo2.c`, `meson.build`.
- `patches/qemu/62-voodoo2-device.patch` — one line: the meson subdir.
- `scripts/prepare-qemu.sh` (the `voodoo/` overlay), `scripts/build.sh`
  (the stamp input), `scripts/sync-86box-voodoo.sh`.
- `tools/voodoo-guest-test.py` and the `voodoo-guest` check in
  `scripts/test.sh`'s guest stage.
- Doc 21, ADR-016, this file.
- Later: `bundle::Accel3d` (or whatever the picker is called) in
  `launcher-core/src/bundle.rs`, its row in `wizard.rs` and both front
  ends — shared with M6, edited minimally, the commit says M14.

## State

**Built, instantiated, and driven by a guest with no 3dfx code
(2026-09-12).** Nothing has run a 3dfx driver or a game on it yet.

- **The port.** 86Box's nine Voodoo files (`vid_voodoo.c`, `_blitter`,
  `_display`, `_fb`, `_fifo`, `_reg`, `_render`, `_setup`, `_texture`) and
  their headers, at upstream `b0e0b7f0` (2026-09-11), **unmodified**,
  compiled as a static library against `voodoo/shim/` — 86Box's `86box.h`,
  `device.h`, `mem.h`, `pci.h`, `plat.h`, `thread.h`, `timer.h`, `video.h`,
  `vid_svga.h`, `machine.h`, `rom.h` and `cpu.h`, each holding exactly what
  those nine files use (doc 21 §4 has the table). `link_whole`d into the
  i386 sourceset, so `qemu-system-i386` and `libqemu-embed-i386.so` both
  carry it (48 `voodoo_*` symbols in each). Two of 86Box's warnings
  suppressed for the library, none of QEMU's; `-msse2` on x86-64; the
  ARM64 recompiler compiles by `#if` on the Air (not tried yet, see below).
- **The device.** PCI `121a:0002` rev 2, class 0400, subsystem 0:0, one
  16 MiB BAR; `initEnable` at 0x40 answered by 86Box (`0x5001` read back
  after writing 1: the Voodoo 2 strap in byte 1); properties `fbmem`,
  `texmem`, `threads`, `bilinear`, `dither-sub`, `filter`, `recompiler`;
  one per machine (86Box's primary-SVGA hook is a process global); a
  reset entry that clears `fbiInit0`/`fbiInit7`/`initEnable` and gives the
  monitor back. `info pci` lists it; `-device voodoo2` on a `pc` machine
  boots.
- **The display path.** `fbiInit0` VGA_PASS → `graphic_hw_passthrough`
  on console 0 → the VGA stops drawing; the display timer's end-of-frame
  blit copies the monitor bitmap into a `DisplaySurface` of ours on that
  console. **Verified by screendump** (`tools/voodoo-guest-test.py`): a
  FreeDOS program in unreal mode finds the card, maps it at `E0000000`,
  writes fbiInit1/2, videoDimensions, hSync/vSync, the DAC PLL (25.2
  MHz), the 33-entry CLUT, `lfbMode` (RGB565, write back / read back),
  fills 640×480 through the LFB, reads a dword back (`F800F800`), swaps,
  and QEMU's own screendump is **640×480, 100 % (248,0,0)** — the
  CLUT-ramped red; after `fbiInit0 = 0` the next screendump is the VGA's
  720×400 text screen. 5 s in the guest. The `voodoo-guest` check.
  **The same passes beside our own adapter** (`VGA=d3dpt`: `-vga none
  -device d3dpt-vga`, the pairing a launcher machine on our display
  driver would run): the Voodoo borrows console 0 whichever VGA device
  owns it, and gives it back.
- **Two bugs found and fixed by that run**, both in the shim, neither in
  86Box: the timer expiry kept only 32 bits of nanoseconds (it wrapped
  4.3 s in; a wrapped timer re-arms in the past and `timerlist_run_timers`
  never leaves it — the main loop spun in `voodoo_callback`, the PIT
  starved, the guest's tick delay never ended, and QEMU ignored SIGTERM,
  which is how it showed), now 48 bits (`ns << 16`); and the test's own
  `puts` clobbered AL before every hex print, which read as "the low byte
  of every register is zero" for an hour. Doc 21 §6 has the units.
- **Not exercised**: the FIFO under a real command stream, the render
  threads with a real triangle (the LFB fill is the FIFO's `WRITEL_FB`
  path and the display half, not `voodoo_triangle`), the command FIFO
  mode, textures, the recompiler's code (it initialises; nothing has
  been rasterized through it), the ARM64 build, Windows (`VirtualAlloc`
  path written, never compiled), a guest reboot with VGA_PASS set.

### Measured

| What | Number |
|---|---|
| guest test, boot to DONE (TCG, this box) | 5 s |
| LFB fill, 153 600 dword MMIO writes | inside that; not timed on its own yet |

## Test loop

```sh
scripts/build.sh                      # or, after a voodoo/ edit:
scripts/prepare-qemu.sh && ninja -C build/qemu qemu-system-i386 libqemu-embed-i386.so
python3 tools/voodoo-guest-test.py    # ~10 s; outputs in build/voodoo-guest/
scripts/test.sh all                   # voodoo-guest is in the guest stage
```

A hung QEMU cannot be attached to on this box (ptrace scope), so a hang
is diagnosed by running the same command line *under* gdb and sending
the QEMU process SIGINT: `gdb -q -batch -ex run -ex 'thread apply all bt
14' --args build/qemu/qemu-system-i386 …` in the background, then
`kill -INT $(ps -C qemu-system-i386 -o pid=)`. That is what found the
timer wrap. `voodoo2:` lines are on QEMU's stderr (`info_report`): the
device's configuration at start, `display on/off`, and every 5 s of
activity a line with frames, triangles, register/texture writes and
reads.

To put the card on a machine from a tool: `tools/win98-game-test.sh`'s
`EXTRA='-device voodoo2'`. The launcher has no way to add it yet (no
free-form arguments in a bundle; next steps, 1).

## Next steps, in order

1. **A guest driver, and the launcher pick that lets a user try it.**
   The 3dfx Voodoo2 reference driver in a Win98 image (the user's own
   download; never in the repo or the ISO) — Device Manager must find
   `121a:0002` and load it, and the driver's `glide2x.dll` must find the
   card (`initEnable`'s strap, the DAC, `fbiInit` reads). Then
   `GLIDETEST.EXE` from the guest-tools ISO against *3dfx's* `glide2x.dll`
   rather than qemu-3dfx's: the four cases through a real Glide, the
   frames on the console. In the machine form: a 3D-accelerator picker
   (none / pass-through, the default / Voodoo 2) in `launcher-core`,
   drawn by both front ends, its check in `scripts/test.sh`; the bundle
   writes `-device voodoo2` and nothing else changes. A Win98 machine on
   `d3dpt-vga` and a Voodoo 2 is the interesting pairing (D3D on ours,
   Glide on the chip).
2. **A game and the numbers** (doc 21 §9): Carmageddon's 3dfx build or
   Rayman 2 on the chip through `tools/win98-game-test.sh
   EXTRA='-device voodoo2'`, the 5 s log line's frames against the game's
   own counter, `perf` on the vCPU thread with the MMIO handlers named,
   and the same run on the M1 Air. Does the 3dfx driver enable the
   command FIFO? (the log will say: `fbiInit7` bit 8, and the wr/tex
   counters). Then **Diablo II** — the title the route was chosen for.
3. **The two performance steps, if the profile asks for them**: the
   command-FIFO window as RAM (a `MemoryRegion` alias into `fb_mem`, the
   doorbell on `cmdFifoDepth` / the wake timer; no trap per dword), and
   the region without the BQL (`memory_region_clear_global_locking` plus
   a device lock between the MMIO handlers and the display timer).
4. **Harden `fatal()`**: 86Box aborts on a malformed command-FIFO packet
   and on `intrCtrl`; a guest must not be able to take QEMU down. Mark
   the card dead, have the FIFO thread drop the stream, log once.
5. **The Air**: the ARM64 recompiler (`MAP_JIT`,
   `pthread_jit_write_protect_np` — 86Box's own code; our `plat_mmap`
   passes the flag) and the `voodoo-guest` check there; then the numbers
   from step 2.
6. **Windows**: the `VirtualAlloc` branch of `plat_mmap` in the cross
   build (`scripts/build-windows.sh`), and whether QEMU's `-Wundef` set
   and mingw like the 86Box sources.
7. A Voodoo Graphics (`VOODOO_1`) type if a title wants one (no command
   FIFO; otherwise the same code), and SLI never.

## Rules

- The vendored files stay verbatim. A change 86Box needs goes upstream
  or into the shim; `scripts/sync-86box-voodoo.sh` must keep working.
- Every claim about a game or a driver comes from a run with its log
  and screendump in `build/`; "the driver should find it" is not a
  state.
- One TCG guest at a time on the box; end scripted Win98 runs with the
  ACPI power button (CLAUDE.md).
