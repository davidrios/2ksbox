# 21 — The Voodoo 2 device (M14)

A real 3dfx Voodoo 2 on the machine's PCI bus: 86Box's emulation of the
chip (Sarah Walker's PCem rasterizer with 86Box's x86-64 and ARM64
recompilers), vendored verbatim and wrapped as a QEMU PCI device,
`-device voodoo2`. The guest runs 3dfx's own drivers and the game's own
`glide2x.dll` / `glide3x.dll` / `GLIDE2X.OVL` against the registers they
were written for; nothing in the guest is ours.

Status and next steps: `docs/tracks/m14-voodoo2.md`. The decision:
ADR-016 in doc 10. Opened 2026-09-12.

## 1. Why a chip, when there is a pass-through

Doc 12 §5 gives a Glide game a *wrapper*: qemu-3dfx's `hw/3dfx` device
carries the guest's Glide calls to the host, where our build of OpenGLide
turns them into OpenGL. That is fast (the host GPU draws) and it is a
translation, so it is only as complete as the wrapper: Glide 3 is not in
it (62 entry points; `patches/openglide/README.md`), the LFB tricks of the
era each need their own patch (`05-lfb-locked-swap` for Carmageddon), and
a 1996 title with Glide linked statically cannot be reached at all.
`patches/openglide/README.md` §"Emulating the chip instead" makes the
case for the other route and records the decision procedure agreed
2026-09-10; this milestone is that route, opened on the user's decision
(2026-09-12) rather than on the Diablo II measurement — which is still
the first thing to take, now on our own device.

What the chip buys: **completeness by construction.** Glide 2 and 3, the
DOS overlay, static links, every LFB and texture-format corner, and 3dfx's
released Glide source as an open driver if one is ever wanted. What it
costs: the frame is drawn by a software rasterizer on host cores, at a
Voodoo 2's own resolutions and formats (800×600, 16-bit, 256×256
textures), and every register write the guest makes is an MMIO trap on
the one vCPU thread (§9).

**It does not replace qemu-3dfx.** qemu-3dfx has two halves. The Glide
half (`hw/3dfx` + the guest `GLIDE2X.DLL`/`.OVL` + OpenGLide) is what a
working Voodoo 2 stands beside: for a Glide title the box then has the
wrapper for speed and the chip for fidelity, and the machine form picks.
The OpenGL half (`hw/mesa` + the guest `opengl32.dll` wrapper) is what
GLQuake, Quake 2, Half-Life in GL mode and wglgears use, and a Voodoo 2
covers that only through 3dfx's period MiniGL / ICD on the emulated chip
— at rasterizer speed and Voodoo 2 limits. So the pass-through stays for
OpenGL titles and for the hosts where the rasterizer does not keep up.
(The Direct3D path, docs 14/15, is a third thing and untouched.)

## 2. What is vendored, and from where

`voodoo/86box/` holds 86Box's `src/video/vid_voodoo*.c` and
`src/include/86box/vid_voodoo*.h` at the commit in `voodoo/86box/UPSTREAM`,
**unmodified** — the port is entirely in the shim and the device, so a
newer upstream drops in (`scripts/sync-86box-voodoo.sh <commit>`). Not
vendored: the Banshee / Voodoo 3 files (2D+3D cards with their own VGA
core; the four entry points the Voodoo 1/2 files still call on them are
stubbed, and unreachable for `type < VOODOO_BANSHEE`) and the 32-bit x86
recompiler. Licence GPL-2.0-or-later, QEMU's own.

The emulation is one 3dfx *set* (`voodoo_set_t`) of one card; the type is
fixed at `VOODOO_2` (a Voodoo Graphics is a property away but has no
command FIFO, §9, so there is no reason to offer it). SLI is not modelled.

## 3. Layout

```
voodoo/
  86box/            the vendored sources + UPSTREAM
  shim/cpu.h        what "cpu.h" resolves to for them
  shim/86box/*.h    86Box's platform headers, reimplemented (§4)
  voodoo_shim.h     the device <-> shim seam (hooks, config)
  voodoo_shim.c     the shim's implementation over QEMU
  voodoo2.c         the PCI device
  meson.build       -> hw/voodoo/meson.build
patches/qemu/62-voodoo2-device.patch   subdir('hw/voodoo') in meson.build
scripts/sync-86box-voodoo.sh
```

`scripts/prepare-qemu.sh` rsyncs `voodoo/` to `qemu/hw/voodoo/`; the
directory's meson builds the 86Box files as their own static library
(their include path is `hw/voodoo` for `<86box/vid_voodoo_*.h>` and
`hw/voodoo/shim` for everything else 86Box, so their `"cpu.h"` is the
shim's and never `target/i386/cpu.h`), with `-msse2` on x86-64 and
86Box's warning set rather than QEMU's, and `link_whole`s it into the
i386 sourceset so both `qemu-system-i386` and `libqemu-embed` carry it.

## 4. The shim: 86Box's platform over QEMU

Everything the nine vendored files ask of 86Box (measured by grep, not
guessed), and what answers it:

| 86Box | Here |
|---|---|
| `thread_create/wait` | `qemu_thread_create` (joinable) / `qemu_thread_join` |
| events (`thread_create_event`, `set`, `reset`, `wait_event(ms)`) | manual-reset over `QemuMutex` + `QemuCond`; `wait_event` returns 1 on timeout, 0 when set, `< 0` waits for ever — upstream's contract |
| mutexes | `QemuMutex` |
| `timer_add`, `timer_advance_u64`, `timer_set_delay_u64`, `timer_is_enabled`, `timer_get_ts_int`, `TIMER_USEC`, `tsc` | `QEMUTimer` on `QEMU_CLOCK_VIRTUAL`; §6 for the units |
| `mem_mapping_add/set_addr/disable` | records the handlers; the device's `MemoryRegion` calls them (§7) |
| `pci_add_card`, `pci_burst_time`, `pci_nonburst_time` | recorded / constants; the device forwards config bytes (§7) |
| `plat_mmap(size, executable)` | `mmap` (+ `MAP_JIT` on macOS), `VirtualAlloc` on Windows |
| `plat_timer_read`, `plat_delay_ms` | `get_clock()`, `g_usleep` |
| `device_get_config_int(name)` etc. | a table the device fills from its properties before `voodoo_init` |
| `cycles -= …` (charging the guest CPU for a PCI access) | a dummy int; TCG charges nothing beyond the trap |
| `svga_get_pri`, `svga_set_override`, `svga_doblit`, `monitors[].target_buffer` | the display path, §7 |
| `fatal()` | prints and aborts, as upstream (its callers fall off the end of a switch) — §8 |

The shim's `86box.h` keeps upstream's atomics verbatim (`volatile` on
x86, C11 atomics elsewhere): the FIFO/render thread protocol is written
in them.

## 5. Threads and the one invariant

86Box runs the guest CPU, the device's register handlers and the
device's timers on **one thread**, and the Voodoo's FIFO thread and 1–4
render threads beside it. The code assumes it: `voodoo_callback` (the
display timer) and `voodoo_writel` touch `line`, `front_offset`,
`dirty_line`, `swap_pending` with only `swap_mutex` between them.

Here the register handlers run on the vCPU thread **with the BQL** and the
timers on the main loop **with the BQL**, so they exclude each other the
same way. The FIFO and render threads are 86Box's own and touch nothing of
QEMU's. The region is therefore **not** `memory_region_clear_global_locking`'d
— that would run the handlers beside the timer with no lock — and the
lock round trip per MMIO write is a cost this design accepts for now (§9).

One deadlock to know about, already handled upstream: the vCPU can block
in a Voodoo wait with the BQL held (`voodoo_flush` on a register read,
`voodoo_queue_command` on a full FIFO, LFB reads), and while it does the
main loop cannot fire the display timer, whose retrace is what completes a
pending buffer swap the FIFO thread may be waiting for. 86Box has the
same shape (its timers are on the blocked CPU thread), and
`voodoo_wait_for_swap_complete` breaks the wait when `flush` is set or the
FIFO is full — exactly the two cases in which the vCPU is blocked. A
status-register poll (what Glide does for a swap) is not a block: the vCPU
leaves the BQL between reads.

## 6. Timers and their units

86Box timers hold a 32.32 timestamp in TSC ticks. Here a *delay* is
nanoseconds of `QEMU_CLOCK_VIRTUAL` in that same 32.32 form (so the
timers follow `-icount` and stop with the VM): `TIMER_USEC = 1000 << 32`,
`cpuclock = 1e9` (then `voodoo_pixelclock_update`'s `clock_const =
cpuclock / pixel_clock` is ns per pixel and its own `* (1ULL << 32)`
makes `line_time` a delay in these units), and `tsc` is the virtual
clock in ns, so `hvRetrace`'s "time until the end of this line" is
consistent. The *expiry* a timer holds is `ns << 16` — 48 bits of
nanoseconds, 78 hours, with a fraction fine enough that a line time
that is not a whole number of ns does not drift. The first version kept
the 32.32 form for the expiry too and wrapped 4.3 s after boot: a
wrapped expiry re-arms in the past, `timerlist_run_timers` never leaves
it, the PIT starves and QEMU stops answering SIGTERM. The guest test
found it in its first run.

The **display timer fires per scanline** — `line_time`, ~32 µs at 640×480
— which on 86Box's CPU thread is cheap and on QEMU's main loop is 31 000
wakeups a second, each taking the BQL. The shim coalesces: a timer whose
callback re-arms it less than `slack_ns` ahead is called again in the same
wakeup until it is that far ahead (`shim_timer_fire`); the device gives
the display timer 1 ms of slack, so the main loop wakes ~1000 times a
second for it and runs ~32 lines each time. Nothing in `voodoo_callback`
depends on wall time between lines — it is a state machine over `line` —
so the only visible effect is that `SST_vRetrace` advances in steps and
`hvRetrace`'s horizontal position is coarse. The wake timer (the FIFO
thread's batching delay, 100 µs) gets no slack.

## 7. The device: PCI, MMIO, display

**Identity.** Vendor `121a`, device `0002`, revision 2, class 0400
(multimedia video). One 16 MiB memory BAR. Bytes 0x40–0x43 of
configuration space are `initEnable`, 86Box's handler answers them (byte 1
carries `0x50 |`, the strap 3dfx's driver reads to know it has a Voodoo 2);
the command register and the BAR's top byte are echoed to the handler so
the card's own `pci_enable` / `memBaseAddr` track what the PCI core did.

**MMIO.** The BAR is a `MemoryRegion` whose read/write call the handlers
`mem_mapping_add` recorded (`voodoo_readw/readl/writew/writel`, which mask
the address to 24 bits themselves: registers, LFB, texture space and the
command-FIFO window). Byte accesses are answered the way the hardware
answers them — 86Box registers no byte handler — with `0xff` / nothing;
accesses wider than 4 bytes (an SSE store into the LFB) are split into
dwords by the memory core.

**Display.** A Voodoo 1/2 is a pass-through card: the 2D adapter's signal
goes through it, and with `fbiInit0`'s VGA_PASS bit the Voodoo drives the
monitor. 86Box models that as the Voodoo overriding the primary SVGA; the
shim turns `svga_set_override` into `graphic_hw_passthrough(console 0)`
(patch 30's hook: the VGA device's `gfx_update` is skipped while it is
set), and `svga_doblit` — called by the display timer at the end of a
frame with dirty lines — into a copy of the monitor bitmap into a
`DisplaySurface` of ours on that console and `dpy_gfx_update_full`. So the
frame reaches the player through the ordinary VGA surface path (the
embed's `on_switch`/`on_update`), a QMP `screendump` shows it, and the VNC
fallback shows it: no 3D-frame path, no player change. When VGA_PASS
clears, the console is invalidated and the VGA draws again into a surface
of its own. The monitor bitmap is 4096×4224, lazily mapped, because
`h_disp`/`v_disp` are 12-bit fields a guest can set to anything and the
display code indexes by them unchecked.

**Reset.** 86Box has no reset entry for the card (the driver re-inits
it), but a guest reboot must give the monitor back: `fbiInit0`,
`fbiInit7`, `initEnable` and the mapping state are cleared and the
override dropped.

**Log.** Every 5 s of activity: `voodoo2: 640x480 on: 61 frames, 12034
triangles, 480211 writes (23011 texture), 3122 reads in 5.0 s` —
`frames` counts presented frames (swaps with dirty lines, so also the
guest's real frame rate), the rest are 86Box's own counters. And
`display on (VGA pass-through)` / `off (VGA back)` at the switch.

## 8. Not modelled, and the hostile-guest question

- **SLI** (two cards) and the **Voodoo Graphics** type: not offered.
- **Interrupts**: 86Box `fatal()`s on a write to `intrCtrl` /
  `userIntrCMD`, so no 86Box guest writes them; a driver that does brings
  the process down. `fatal()` here is upstream's contract (print, abort),
  which means a **malformed command-FIFO packet from the guest aborts
  QEMU** (`CMDFIFO packet 5 bad space`, and a handful more). 86Box lives
  with that; a hardened `fatal` that marks the card dead and has the FIFO
  thread drop the packet is on the track's list, after the device is seen
  to work.
- **Migration**: no vmstate; a snapshot of a machine with the card does
  not carry it (the Voodoo's state is what the driver re-creates).
- **Byte accesses** to the LFB: none on the hardware, none here.

## 9. Performance: what to expect and what to measure

The frame is drawn by 86Box's rasterizer: the per-pixel pipeline for the
current register state is JIT-compiled (x86-64 SSE2, or ARM64 NEON —
86Box has that, PCem does not, which is why it is 86Box) and run by 1, 2
or 4 render threads (`threads=`), each owning alternate scanlines. Those
threads land on cores the one-vCPU guest leaves idle. A Voodoo 2 filled
~90 Mpixel/s; a 640×480 game needs 20–30 for 30 fps; a modern core does
that.

The cost that is ours is on the vCPU: every register write is an MMIO
trap — generated code to the softmmu slow path, the address-space walk,
the BQL — where 86Box's CPU core calls the handler directly. Dozens of
writes per triangle, tens of thousands a frame. Two things reduce it:

1. **The command FIFO** (Voodoo 2 only; `fbiInit7` bit 8): Glide writes
   packets into a window of the card's own memory and the chip pulls them.
   86Box stores them into `fb_mem` on the trap and wakes the FIFO thread
   every 20 entries — still a trap per dword. The step past that is to
   back the window with **RAM** (a `MemoryRegion` alias into `fb_mem`'s
   page range) so the writes are plain stores and one doorbell (the
   `cmdFifoDepth` write, or the wake timer) hands the batch over — the
   trick qemu-3dfx's own FIFO uses. Whether the 3dfx driver enables the
   command FIFO at all is the first thing the log will say.
2. **Dropping the BQL round trip** (`memory_region_clear_global_locking`)
   needs a lock of our own between the handlers and the display timer
   (§5). Second, if the profile says so.

The measurements the track wants, in order: the 5 s log line's frames
against the game's own counter (is the rasterizer keeping up?), the
vCPU's share in MMIO (`perf` with the register handlers named), the same
on the M1 Air (`tools/tcg-profile.sh`), and Diablo II — the title the
route was chosen for (memory `glide3-landscape`).

Knobs: `threads=1|2|4` (default 2), `recompiler=off` (the interpreter, the
A/B for a rasterizer bug), `bilinear`, `dither-sub`, `filter` (86Box's
"screen filter", off), `fbmem=2|4`, `texmem=2|4` (per TMU; 4 is the 12 MB
board).

## 10. The guest side

Nothing of ours: the card wants **3dfx's Voodoo2 reference drivers**
(Win9x: the `3dfxV2` package, Voodoo2 drivers 3.02/3.03; a `glide2x.dll`
and `glide3x.dll` come with them; XP: the last 3dfx reference driver or
the community's). Those are the user's own downloads and are never added
to the repository or the guest-tools ISO. A DOS Glide game carries its
own `GLIDE2X.OVL` (3dfx's, not qemu-3dfx's — the two overlays are
different programs for the same name: ours talks to `hw/3dfx`, 3dfx's to
the chip).

Detection: 3dfx's driver scans PCI for `121a:0002` and reads
`initEnable`; a Voodoo 2 with no monitor pass-through cable is still a
Voodoo 2 (the card never sees the cable).

**In the launcher** the card is one checkbox on the machine form
("Emulated 3dfx Voodoo 2", `voodoo2` in the bundle, doc 07): off unless
picked, on every family, `-device voodoo2,addr=0x05` when on — the slot
after the sound card's, on whichever 2D adapter the machine has. Our own
adapter plus the chip is the pairing to want on a Win98 machine: Direct3D
on the doc 19 driver, Glide on the chip. The `voodoo2` check in
`scripts/test.sh` walks it from the form to `query-pci` on our QEMU.

The device's own check, with no driver, is `tools/voodoo-guest-test.py`:
a DOS program finds the card in configuration space, maps it, runs the
init sequence 3dfx's `sst1init` runs (fbiInit resets, the DAC PLL, video
timing, `videoDimensions`, VGA_PASS), fills the LFB with a colour, swaps,
and a screendump has to be that colour — the whole path from a guest
store to the console surface, without a line of 3dfx code.
