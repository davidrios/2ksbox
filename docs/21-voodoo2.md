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
| `calloc` / `free` (macros in the shim's `86box.h`) | allocations of 1 MB and more — the frame buffer, the texture memories, the `voodoo_t` — are mapped with **64 MB of zero pages after them**: 86Box's display timer indexes `fb_mem` by `front_offset + line * row_width` with no bound, off registers a guest sets to anything (3dfx's Glide put 6.8 MB into 4 MB on a reopen, 2026-09-12); an overrun reads zeros instead of faulting |
| `fatal()` | **returns**, against upstream: the write is refused, counted, the first one dumped with the device's state and last 64 accesses — §8 |

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
**0x54 is `siProcess`**, the silicon-process monitor, and it is the
device's own (2026-09-12, the first run of 3dfx's driver): Glide's
`sst1InitMeasureSiProcess` loads a PCI-clock countdown into bits 27:16,
sets RUN (bit 28) and polls until that field reads zero, then takes the
ring-oscillator count from bits 15:0. QEMU keeps whatever a guest writes
past the 64-byte header, so the loaded count read back for ever and every
Glide program froze in `grSstWinOpen` at ~30 M reads/s; 86Box answers 0
to the whole register (the loop ends at once with a count of 0, "a very
slow process"). Here the countdown is over as soon as RUN is read back
and the count is a production board's (8000, past Glide's 5000
threshold). The guest test runs the same sequence, bounded.

**MMIO.** The BAR is a `MemoryRegion` whose read/write call the handlers
`mem_mapping_add` recorded (`voodoo_readw/readl/writew/writel`, which mask
the address to 24 bits themselves: registers, LFB, texture space and the
command-FIFO window). Byte accesses are answered the way the hardware
answers them — 86Box registers no byte handler — with `0xff` / nothing;
accesses wider than 4 bytes (an SSE store into the LFB) are split into
dwords by the memory core. One bit is not passed on: **`fbiInit1` bit 23,
scanline interleaving**. This device is one card with no partner, and
86Box's display timer takes SLI at its word and draws the odd lines from
`set->voodoos[1]`, a NULL here. The guest that set it was 3dfx's Glide 2.x
at **window teardown** (`grSstWinClose`, 2026-09-12): it streams a burst
of dwords into the command-FIFO window with the FIFO off, and with the
FIFO off that window is the legacy register map (bit 21 = the alternate
register mapping Glide has enabled in `fbiInit3`), so the burst walks the
register file — `intrCtrl` (86Box's `fatal()`, §8), the video registers
(a garbage `videoDimensions` is where the log's 3741×1789 comes from),
`fbiInit1` — exactly as it would reach the chip. A real single board with
the SLI bit set shows half its lines; this one ignores the bit. **The
teardown burst is M14's open bug**: after it the card never reports idle
and Glide spins in `sst1InitIdle` (GLIDETEST hangs at the close, whether
or not the reopen case runs); the install and the first open+draw work.
**No longer (2026-09-13, by hand on `base98-br`):** GLIDETEST does not
hang, and Quake II, Unreal Tournament and NFS Porsche Unleashed all close
cleanly. What remains is that a second game after one has quit sometimes
starts with glitched graphics — likely state the burst leaves behind.
**What the "burst" was, at least in the reproducible case (2026-09-16,
§11):** not Glide's own teardown but a second Glide client — 3dfx's login
helper, in another process — initialising the card under a live window,
so the running Glide went on writing packets into a FIFO that had been
switched off behind it. **Refused since 2026-09-20** (§11's
"stranded client"): the walk is what leaves the card unusable, a real chip
is not, and the writes are nobody's on purpose — `fifo-off-regs=on` is the
A/B that puts the walk back.

**A second shape of it, on the Windows PC (2026-09-17, the user, 3DMark
99 on `base98-br`): the consumer stops inside a packet.** 86Box's
`cmdfifo_get` waits for the next word whenever `depth_rd == depth_wr`, so
a packet header that promises more words than Glide wrote parks the FIFO
thread there with `voodoo_busy` set — and Glide, which polls the status
register for idle before it writes anything else, never writes the words
that would free it. Neither side moves; the guest's Windows is fine, and
a guest reset does not reset the card, so the next login's helper (§11)
hangs on the same card with its "please wait" notice up for ever.
Measured with `ramfifo=off`, where every packet word is counted as the
guest writes it: 15.8 M status reads in 5 s, the ring fully consumed
(`depth 47696494/47696494`), 0 commands outstanding. The same stall with
the ring in RAM shows as Glide polling `cmdFifoRdPtr` instead (the
2026-09-17 hang: 258 M reads over ~5 minutes, which is where the stall
line below comes from), because our packet walk counts words with the
same table, so a bad header stalls both paths alike. **What the device
prints now**, two 5 s windows into such a stall: the word the consumer is
waiting for, the ring around it, and the guest's last 64 accesses —
`voodoo2: the command FIFO is stuck inside a packet`. It also warns on a
packet word written narrower than a dword, which 86Box drops outright
(its `writew` takes only the frame buffer and it has no byte handler), a
lost word being one way to reach the same deadlock. Glide's own device
probe is logged too — every LFB readback with the FIFO off, which is how
its frame-buffer sizing, its TMU configuration strap and its texture
memory sense (2/1/0 MB) can be watched: those four came back right on the
PC, so a stale texture cache is not what broke the 800x600 open.

**One such hole was found and closed, and it is not what 3DMark 99 trips
over** (patch 71, 2026-09-17). Packet 3 names its per-vertex parameters in
bits 17:10, and bit 28 says the colour comes as one packed ARGB word; 86Box
read that word only under the RGB bit, and a separate alpha float only when
bit 28 was clear. Glide sends **iterated alpha over a constant colour**
with the packed bit set and the RGB bit clear (`gglide.c`), one word a
vertex, and both consumers read none — a word a vertex left in the ring,
which is exactly the desynchronisation shape above. Our packet walk counted
by the same rule and is fixed with them. **But the user's 3DMark 99 run
sends no such packet**: the device says so once when it meets one, the line
never appeared, and the glitched loading screen came back with the fix in.
So the FIFO hole was real and the glitch is something else — and the
screenshot says where to look. It is the **800x600 desktop on `d3dpt-vga`**
(docs 15 and 19), 3DMark's own 2D loading screen, its text crisp and the
picture around it in stale horizontal bands, not a Voodoo frame at all: the
bands are what was on screen before, so the adapter kept them while the
guest's writes went unseen. `-device d3dpt-vga,full-frames=on` converts the
whole frame every refresh instead of the dirty spans, which is the A/B
(`scripts/win-voodoo-ab.sh vga:full-frames=on`).


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
of its own — which is a promise the VGA device has to keep: **an
invalidate must put the device's own surface back on the console**, not
only repaint. QEMU's VGA core does (it forgets its last geometry), but
`d3dpt-vga`'s linear mode re-installed its surface only when the mode
changed, so after a hand-back it went on updating the Voodoo's surface
and the player kept the Voodoo's last frame for good — on a Win98 machine
with the card and our driver, every full-screen switch (a game or the 3dfx
driver probing Glide) left a silver screen that never came back
(2026-09-12). The device now re-surfaces on every invalidate, and
`voodoo-guest-d3dpt` holds it (the adapter in an 800×600×32 linear mode
first; the screendump after the hand-back must be that mode). The monitor bitmap is 4096×4224, lazily mapped, because
`h_disp`/`v_disp` are 12-bit fields a guest can set to anything and the
display code indexes by them unchecked.

**Black until the first swap** (2026-09-14, the user: "a gray pattern
flashes before it adjusts" whenever a game took the card or changed
mode). 86Box's frame buffer starts zeroed, so the pattern is the guest's
own: 3dfx's init writing test patterns into the buffer while VGA_PASS is
already on, or the last mode's contents read at a new pitch. A real Voodoo
2 scanned that out too, but the monitor behind it was dark while it
re-locked to the new timings. So the device shows black from the moment the
card takes the monitor, or its `h_disp`×`v_disp` changes, until the guest
swaps (`front_offset` moves, or `frame_count` for a swap on a retrace) —
and one frame more, because a swap that lands mid-frame leaves the lines
above it stale until the next frame redraws them all. A guest that never
swaps (drawing only into the front buffer) is shown after 2 s
(`VOODOO2_BLANK_MS`) regardless. While black, every line is marked dirty
at each present, so a frame is presented at every retrace: the display
timer presents only frames with dirty lines, and the first version of this
left the guest test's frame black for good. That guest swaps once and then
draws nothing, so the frame after its swap never came.

**Reset.** 86Box has no reset entry for the card (the driver re-inits
it), but a guest reboot must give the monitor back: `fbiInit0`,
`fbiInit7`, `initEnable` and the mapping state are cleared and the
override dropped.

**Log.** Every 5 s of activity: `voodoo2: 640x480 on: 61 frames, 12034
triangles, 480211 writes (23011 texture), 3122 reads in 5.0 s; regs read
0x000:2981 0x218:19; written 0x120:279 0x114:263; config read 0x040:12` —
`frames` counts presented frames (swaps with dirty lines, so also the
guest's real frame rate), the rest are 86Box's own counters, and the three
histograms are the four most-hit registers of the window (by `addr &
0x3fc`), the four most-written, and the four most-read configuration-space
dwords: **a guest that spins names what it is spinning on** (`0x000` is
`status`, `0x054` in the config column was the siProcess loop). `; N
writes refused` follows when 86Box's `fatal()` fired in the window. `display on (VGA pass-through)` / `off (VGA
back)` at the switch, and `640x480 shown after 180 ms of black (first
swap)` — or `(no swap)` when the 2 s ran out — once a new mode's first
frame goes up. And `VOODOO2_TRACE=1` in the environment prints
every register- and FIFO-window access (the status polls collapsed to a
count) — thousands of lines a second, for reading one open sequence
against 3dfx's own `sst1init` source.

## 8. Not modelled, and the hostile-guest question

- **SLI** (two cards) and the **Voodoo Graphics** type: not offered.
- **Interrupts**: 86Box `fatal()`s on a write to `intrCtrl` /
  `userIntrCMD`, and on a malformed command-FIFO packet (`CMDFIFO packet
  5 bad space`, and a handful more), which in 86Box ends the emulator.
  **Here `fatal()` returns** (2026-09-12): every call site `break`s or
  falls through after it, so the write is refused and the stream goes on;
  the first is printed with the device's state (`initEnable`, `fbiInit0/7`,
  the FIFO's base, end, read pointer and depth) and the last 64 MMIO
  accesses, the rest are counted into the 5 s line. Not `noreturn` in the
  shim's header — declared so, the compiler dropped the code after the
  call and the return landed in the next case (a SIGSEGV that looked like
  86Box's). The first guest to hit it was 3dfx's Glide at window
  teardown (§7), whose garbage burst reached `intrCtrl`; after the burst
  the card never reports idle and Glide spins in `sst1InitIdle` — M14's
  next bug (the install and the first open+draw work).
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
   **Done 2026-09-15 (`ramfifo`, on by default), and it was most of the
   cost.** 3dfx's driver does use the FIFO, and a `perf` profile of Quake
   II's demo showed why a trap per dword was so dear: under TCG an MMIO
   store that is not a block's last instruction is a `cpu_io_recompile`
   (unwind the block, look it up in the TB tree, regenerate a block that
   ends at the store), about a third of the saturated vCPU, with the MMIO
   path proper another sixth. Glide leaves hole counting on, so it rings
   no doorbell: the chip is meant to see every write. So the device finds
   them itself — the ring is an alias of a page-aligned `fb_mem` (86Box's
   calloc is handed a replacement at realize and gets its own back at
   close), consumed words are poisoned with `0xdeadbee7` (packet type 7,
   which does not exist), and at every access the guest still makes to the
   card the device walks the ring from the last word it counted and adds
   what the guest has written to the depth the per-dword writes used to
   add. The guest learns a slot is free only by reading `cmdFifoRdPtr`,
   which the device answers after poisoning up to exactly what it returns,
   so the guest never writes over a word that is not poison. A packet it
   cannot follow (JSR/RET, AGP, Banshee types) sends the window back to
   MMIO with a warning.

   **The poison word must be one no guest writes** (2026-09-17, the user's
   own call, and the fix for a fortnight of 3DMark 99 hangs on the Windows
   PC). It was `0xffffffff` — which is a white texel. A texture download
   carrying white read as unwritten ring, so the walk stopped on the
   guest's own data and the chip stopped with it; Glide, which waits for
   the read pointer to travel before writing more, then waited on a pointer
   that could never move again (26 M reads of `cmdFifoRdPtr` in 5 s, the
   ring empty behind it). The same collision fed half-read packets to the
   consumer, which is what garbled the loading screens. Only the low three
   bits of the value are forced (the packet type the chip has no meaning
   for); the rest is chosen to be absurd as a float (-2.5e18), absurd as a
   pair of texels, and to name itself in a hex dump: `0xdeadbee7`. The look
   ahead applies to a packet's **payload** only — a header the guest has
   not written is always a gap, since the header is the first word it
   writes, and taking the poison there for a packet dropped the window back
   to MMIO mid-stream on the first run with the new value.

   **It counts words, not packets** (the same day). The chip runs words as
   they arrive and 86Box's consumer blocks inside a half-written packet by
   itself, so the walk hands over whatever has been written rather than a
   packet at a time on the header's word count. Glide does write each
   packet whole before it touches the card again — the 5 s line counts the
   packets met half-written and the count stays at 0 — so this changes
   nothing in practice; it is the faithful model, and it is one assumption
   fewer between a guest and a hang.

   **The read pointer never passes what the guest has written** (2026-09-20).
   There used to be one exception here: should a run of data ever read as
   poison with the chip caught up, the walk took the rest of that packet
   rather than wait for ever, on the argument that an empty ring means the
   guest cannot be waiting for room. The argument eats itself. Glide's free
   space is `rp - wp - 1`, so taking words the guest has not written puts
   the read pointer *past* its write pointer — a state the chip cannot
   reach — and the room the guest computes from that pointer is then a few
   words instead of the whole ring. It waits for space; the ring it is
   waiting on stays empty; nothing moves again.

   **FIFA 2000's loading screen died on exactly that** (the user,
   2026-09-20, `base98-br`): a 66-word type-5 LFB packet with 19 words
   written, the walk idle on it for 64 `cmdFifoRdPtr` polls, 47 words taken
   as data, and a guest with 46 words of room asking for 66 — then
   22.7 M reads of `cmdFifoRdPtr` a second, `depth 24623/24623`, the card
   idle, nothing written, for ever. The device said so itself: `1 packets
   part-written, 47 words taken as data`, and `47 = 66 - 19` exactly. **The
   same game runs matches start to finish on `ramfifo=off`** — the
   transport that counts every write and guesses nothing — which is what
   proved the guest was never the problem.

   So nothing is taken that the guest has not written, however long the
   wait looks: waiting is what the chip does, and a stall where the pointer
   is honest can at least be read. The case the exception was written for —
   real data reading as poison for longer than the look ahead — was closed
   at its root when the poison word stopped being `0xffffffff` above; a run
   of eight dwords of a guest's own data all reading `0xdeadbee7` is not a
   trade worth a deadlock. The `voodoo-guest` check's **partial-packet
   phase** holds the invariant: the header of a two-word packet is written
   and its value is not, `cmdFifoRdPtr` is read 256 times (the old
   exception waited for 64), and the pointer must read one word in —
   `00300004`, the header consumed and the chip parked wanting the value —
   and stay there; then the value arrives and the packet completes at
   `00300008`. With the exception restored it reads `00300008` at the hold,
   which is the FIFA failure in two words. **Quake II `timedemo demo1`: 41.1 → 147.5
   fps** on the same build (`ramfifo=off` the A/B), the chip now the busy
   side (tens of thousands of words queued, ~14 M words a second); UT's
   flyby 33.6 → 40.7 fps; Quake II's frames checked by screendump. The
   `voodoo-guest` check drives the FIFO the same way from a DOS program
   (two batches, one across a JMP, one after a read-pointer read; the read
   pointer and the frames are the verdict) and `voodoo-guest-mmiofifo` is
   the same with `ramfifo=off`.

**Dither subtraction in both recompilers** (2026-09-16, patch 64). A
Voodoo dithers what it writes, so a pixel read back to be blended with
carries that position's dither offset, and the chip subtracts it again when
`fbzMode`'s `DITHER_SUB` (bit 19) is set — 86Box gates that on its own
`dithersub` setting, which is on by default and is `-device
voodoo2,dither-sub=on|off` here. 86Box's plain interpreter did it; neither
the x86-64 nor the ARM64 code generator mentioned `dithersub` at all, and
the recompiler is the default. Upstream's inconsistency, not the port's.
The `voodoo-guest` check's last phase measures it: a grey that has to
dither in every channel (130,130,130) drawn over the screen, then blended
**onto itself** 96 times per column at falling alpha (128, 96 ... 12).
Blending a colour onto itself is that colour, so every column should stay
the background's and the bands above and below are the reference. With
`recompiler=off` every column read the reference exactly (`DITH COL` =
`DITH REF` = 7c0f) and the frame was one 4x4 dither tile; with it on the
columns walked away (7bef, 7bcf ... 6b4d), 10,240 pixels off the tile, a
grainy rectangle. **Patch 64** gives both generators the interpreter's
three table lookups at the destination read-back (`dithersub_rb` /
`dithersub_g` or the 2x2 pair, keyed on the x the dither write uses and
`real_y`, the entry's alpha byte kept), under the same two conditions; the
block cache already keys on the whole `fbzMode`. It is a patch on the
overlaid copy in `hw/voodoo`, so `voodoo/86box/` stays verbatim and the
fix goes upstream to drop it. Both emissions were checked against the
tables before a guest saw them — the x86-64 bytes run natively, the ARM64
ones under Unicorn, every byte value per channel at every dither position —
and the check now *requires* the columns to equal the reference and the
screendump to be one tile, in both modes (`RECOMP=off
tools/voodoo-guest-test.py` is the interpreter, `DITHER_SUB=off` the
control, which fails on both paths with the numbers above; frames in
`build/voodoo-guest[-interp]/dither.ppm`). No title was known to care.

2. **Dropping the BQL round trip** (`memory_region_clear_global_locking`)
   needs a lock of our own between the handlers and the display timer
   (§5). Second, if the profile says so.

The 5 s line's frame rate is its `(N new)` column (2026-09-15): the
presents that showed a buffer the one before did not, i.e. the game's
frames as the monitor shows them, capped at the refresh. `frames` beside
it counts every present with a dirty line — an FMV written through the LFB
reads 225 there with 0 new — and 86Box's own `frame_count` counts only the
swaps that wait for a retrace, so a game with vsync off reads 0 on it.
Against Quake II's own `timedemo` (41.3 fps) the column read ~35: two
swaps inside one refresh show as one frame, so it is a floor.

The measurements the track wants, in order: the 5 s log line's frames
against the game's own counter (is the rasterizer keeping up?), the
vCPU's share in MMIO (`perf` with the register handlers named), the same
on the M1 Air (`tools/tcg-profile.sh`), and Diablo II — the title the
route was chosen for (memory `glide3-landscape`).

Knobs: `threads=1|2|4` (default 2), `recompiler=off` (the interpreter, the
A/B for a rasterizer bug), `bilinear`, `dither-sub`, `filter` (86Box's
"screen filter", off — and a no-op until the guest programs `maxRgbDelta`,
§12), `undither` (ours, off: the dither reconstructed away rather than
blurred, §12), `fbmem=2|4`, `texmem=2|4` (per TMU; 4 is the 12 MB
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

**Our guest tools stay out of its way** (2026-09-13). 3dfx's driver
installs `GLIDE2X.DLL` / `GLIDE3X.DLL` / `FXMEMMAP.VXD` in the system
folder — the names qemu-3dfx's wrappers have — and `SETUP.EXE`'s Glide
component used to copy ours over them on every `/ALL`, so which Glide a
game got was whichever was copied last. SETUP now looks for a *present*
3dfx PCI device (9x: the devnode tree in `HKEY_DYN_DATA`; NT:
`CM_Locate_DevNode`) and, with one, leaves `GLIDE*.DLL`, an existing
`FXMEMMAP.VXD` and `GLIDE2X.OVL` alone. The mapper still goes in where
there is none, because our Direct3D and OpenGL DLLs need it. qemu-3dfx's
`FXMEMMAP.VXD` is 3dfx's own binary (4.10.01.0013, Glide 2.42), so either
copy serves both. A title that should take the pass-through on such a
machine gets ours next to its EXE: `SETUP /GAME 6` (the DLLs), `/GAME 7`
(the DOS overlay). `VOODOO=1 tools/setup-guest-test.sh` is the check: it
passed on Win98 and XP on 2026-09-13, where SETUP logged the card as
`PCI\VEN_121A&DEV_0002&SUBSYS_00000000&REV_02\BUS_00&DEV_05&FUNC_00` (98)
and `…\3&267a616a&0&28` (XP), and every marker planted under 3dfx's names
survived `/ALL`. No 3dfx driver was involved: those files were stand-ins.

The PCI map was checked the same day: on both adapters SeaBIOS puts the
card's BAR at `0xfd000000`, clear of the fixed pass-through windows
(Glide `0xfb000000`–`0xfb7fffff` and `0xfbdff000`, Mesa
`0xea000000`–`0xefffefff`, d3dpt `0xd8000000`–`0xdfffefff`). Nothing
reserves those windows to the guest, so a guest that moved the BAR could
overlap them. An ACPI Win98 keeps the firmware's placement.

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

## 11. Two Glides on one card: 3dfx's login helper

3dfx's Voodoo 2 driver installs a Run entry, `Voodoo2`, that runs
`rundll32.exe 3dfxv2ps.dll,UpdateRegSettings` at every login. That DLL is
the driver's DirectDraw/Direct3D HAL, built on **Glide 3** (`GLIDE3X.DLL`),
and the call brings the board up through Glide 3's own copy of the init
library: `sst1InitMapBoard` (a first map shuts the video down) and
`sst1InitRegisters`, which zeroes the video timing and puts `fbiInit7` back
to its default — the command FIFO off. On a real card that is
milliseconds after the desktop appears. Under TCG, with the init
library's 200 000-iteration clock-settle loops (`init/video.c`), it is
**about three seconds**.

A Glide program started inside those seconds loses. It opens its window —
video mode, `sst1InitCmdFifo(FXTRUE)`, a first swap — and then the helper's
init lands on top of it, in another process with separate static state:
display off, video registers zeroed, FIFO off. Nothing tells the program.
It goes on writing command packets into the window at `0x200000`, where
with the FIFO off they are register writes (§7): `videoDimensions`
scribbled, `intrCtrl` fatals, and a guest spinning for ever on
`cmdFifoRdPtr`. With the 3dfx splash disabled (`FX_GLIDE_NO_SPLASH=1`) the
program survives to exit but draws nothing — the card is still torn down.

How it was pinned (the track doc has the runs): `VOODOO2_TRACE=1` names the
module doing each register write — the PE image around the guest's program
counter, found by walking back to its `MZ` header, with the image at
`0x00400000` identifying the process — and the first init came from
`GLIDE2X.DLL` at `0x10000000` under a `0x42000` image (GLIDETEST.EXE), the
second from `GLIDE3X.DLL` relocated to `0x01690000` under a `0x6000` image
(RUNDLL32.EXE). The same GLIDETEST started 20 or 90 seconds after the
desktop passes every case, the close/reopen one included.

What the device does about it: nothing to the init, which is the chip's
behaviour — two inits overlapping would wreck a real card too. It **names
it**: a config write of `initEnable = 1` (how `sst1InitRegisters` opens)
while the command FIFO is on can only be someone else's init, since
Glide's own close turns the FIFO off first, and it warns `the card is
being re-initialised (sst1InitRegisters) while a Glide window has the
command FIFO on`. Glide's own close-and-reopen does not trip it.

### The stranded client's packets are refused (2026-09-20)

**It also happens at a game's close, and there the warning above cannot
fire.** FIFA 2000 on `base98-br`, the user's own run: 800×600 on the card
for ~2½ minutes at 30 fps (150 frames per 5 s window, ~700 k triangles),
then `command FIFO through MMIO` and `display off (VGA back)` — an
ordinary `grSstWinClose` — then a full `sst1InitRegisters` with the memory
probe, and then a client streaming command-FIFO packets into the window
with the FIFO off. Because the close had already cleared `fbiInit7`, the
re-initialisation warning's `cmdfifo_enabled` test was false and it said
nothing. What a **healthy** reopen looks like is worth knowing, because it
is unmistakable and none of it was there (`VOODOO2_TRACE=1` on GLIDETEST,
same day):

    initEnable <= 00005001
    wr 0001e0 020101c2   cmdFifoBaseAddr      wr 0001f0 001c1ffc   amax
    wr 0001e8 001c2000   rdPtr                wr 0001f4 00000000   depth
    wr 0001ec 001c1ffc   amin                 wr 0001f8 00000000   holes
    wr 00024c 0ffb8300   fbiInit7, bit 8: the FIFO on
    initEnable <= 00005003

FIFA's had no `00005001`, no `0x1e0`–`0x1f8` block and no `fbiInit7` with
bit 8 — and the ring registers still held the *dead* session's values
(`base 002e5000 … rp 002eee80`). So the streaming client had not reopened
at all: it resumed on the old ring after someone else's init had switched
the FIFO off under it. The guest then spun — 640 943 status reads and
919 878 LFB writes to the front buffer in five seconds — and the machine
reset five seconds after that, with no bugcheck (the display driver logged
no `message mode: VGA text` and no `linear mode off`).

**So the walk is refused by default now.** Nothing writes that window on
purpose with the FIFO off — Glide only writes there when it believes the
FIFO is on — so every such dword is a stranded client's packet, and
letting them walk the register file destroys the card for everything
after: `videoDimensions` is zeroed and never rewritten, so the display
timer stops generating retraces and `status` never reads idle again;
`fbiInit7` flips the FIFO on and off at random; `intrCtrl` reaches 86Box's
`fatal()`. A real chip is not left unusable by this — 3dfx's own Glide
does it routinely — so the permanence is the emulation's, not the card's.
`-device voodoo2,fifo-off-regs=on` is the A/B, the walk exactly as 86Box
decodes it. Offsets below `0x100` still pass either way: those are the
vertex and triangle registers under Glide's alternate mapping, and a stray
triangle renders and is over, where the rest sticks.

The warning now also **names both sides** without the trace, once: `the
packet comes from …` (the module and call chain around the guest's program
counter, with its `cr3`) and `the last sst1InitRegisters was …`. The same
`cr3` in both is one program re-initialising under itself; a different one
is a second Glide client.

The `voodoo-guest` check guards it (`tools/voodoo-guest-test.py`, the
stranded-client phase): with the FIFO left off by the teardown phase, a
burst goes into the window at the offsets of `cmdFifoBaseAddr`,
`videoDimensions` and `fbiInit7`, and the ring's own register has to read
back unmoved. `FIFO_OFF_REGS=on` is the control and must move it —
measured `03010300 -> 02AD02EF`.

**What the guest tools do about it (2026-09-16): a start-up guard.**
`SETUP /I 6` on 98/Me with a 3dfx card present ("Voodoo 2 start-up guard",
`guest-tools/src/v2start.c`) moves 3dfx's `Voodoo2` value out of HKLM's
Run key into `HKLM\SOFTWARE\2ksbox\Voodoo2` (`Command`) and puts
`C:\WINDOWS\V2START.EXE` there instead. At login V2START runs that command
itself, waits for it to exit (bounded at 120 s), and while it waits keeps
a small topmost window up — "Voodoo 2 driver is loading, please wait
before running 3dfx games" — shown only if the init is still going after
half a second. It informs, it does not block: the desktop stays usable
(user decision, over a first cut that covered the screen). It writes
`C:\WINDOWS\V2START.LOG` when it is done, which is what a harness can
watch. A driver reinstalled after SETUP puts the Run value back; V2START
moves it again at the next login and waits for the rundll32 Explorer
already started rather than starting a second init. A machine whose
3dfx DLL is gone runs nothing (no rundll32 error box at every login).

**Measured on `base98-br` with the guard (2026-09-16):** the helper took
**11.3 s and 18.7 s** on two logins under TCG — not the ~3 s above, which
was one quiet boot, and close to the harness's old fixed 20 s wait.
GLIDETEST started the moment V2START's log appeared passed 3/0 both
times, with no re-initialisation warning; the notice is in the
screendumps (`build/w98game/v2shot/shots/`).

`tools/win98-game-test.sh` on a machine with the card watches for that log
when the image has V2START.EXE (`VOODOO_WAIT`, 60 s, is then the cap) and
prints it; an image without the guard gets the old fixed wait (20 s).
Either way the summary names a collision if one happened.

The hangs seen by hand were this too: the user had noticed a stray
`rundll32` running every time a game froze and could not say why
(2026-09-16). If that process ever turns up outside login, the warning
above is still what names the collision.

## 12. The dither, undone (`undither=on`)

The chip renders colour at more than 16 bits and stores RGB565 through an
ordered dither; 3dfx's RAMDAC put a box filter on the scanout that partly
undid it, which is what "22-bit colour" meant. Two filters exist here now
and they are not the same thing.

**`filter=on`** is 86Box's — leilei's approximation of the RAMDAC, a pair
of 256x256 blend tables (`voodoo_generate_filter_v2`) applied at
`vid_voodoo_display.c:554`. On a Voodoo 2 it is a **single-scanline** pass:
`voodoo_filterline_v2()` takes one row pointer and its `line` argument is
`UNUSED`, taps sit at `src[x±1..3]`. So it softens the dither along a line,
leaves the vertical half of every pattern, and it only runs at all once the
guest has written a non-zero threshold to **`maxRgbDelta`** (register
0x230, `SST_scrFilter`, and only with `initEnable & 1`): nothing seeds
`scrfilterThreshold`, so `filter=on` alone is a no-op until 3dfx's driver
programs it. It is kept as what the hardware's filter looked like.

**`undither=on`** (`voodoo/undither.c`, ours) is the other direction. In an
emulator the dither matrix is not something to approximate: it is the table
the rasterizer dithered *with*. `86box/vid_voodoo_dither.h` holds it,
`vid_voodoo_render.c:1329` applies it indexed by `(real_y & 3, x & 3)`, and
for a linear non-SLI buffer the row dithered as row r is scanned out as row
r — so at scanout the phase is `(y & 3, x & 3)`, known exactly. Inverting
the table gives, per phase and per stored code, the interval of 8-bit
values that dither to it.

Pixels that came from one pre-dither colour carry intervals with a common
member, and the intersection is what that colour can have been. Two window
sizes, in that order:

- **4x4, centred.** The interval comes out exactly **one value wide** for
  every value and every phase of both 4x4 tables, so the output is the
  colour the rasterizer had, exactly — and, what a 2x2 cannot do, the *same*
  value at all sixteen phases.
- **2x2, anchored** — the fallback where a 4x4 holds more than one colour
  (an edge, a steep gradient). Within 2/255: the four tables measure 2 for
  `dither_rb`, 1 for `dither_g`, 1 for both 2x2 ones.

The 4x4 stage is not optional prettiness. A 2x2 alone is within 2/255, but
its midpoint **moves with the phase**: measured, **255 of 256** flat colours
come back out of a 2x2 with more than one level in them — a residual 1-LSB
pattern exactly where there should be none. With the 4x4 stage first, every
one of the 256 comes back as a single level equal to what was rendered. It
is cheap because the 4x4 is the intersection of four *2x2* results, which a
row of them already has: four lookups on top, not sixteen.

An empty intersection at both sizes is the proof of the opposite: no single
colour could have dithered into those pixels, so the window straddles an
edge, and that pixel is written exactly as the unfiltered path wrote it
(`code << 3`, `code << 2`). **An edge here is not "a difference bigger than
N" — it is an arithmetic impossibility**, which is why this needs no
threshold, never blurs across an edge, and leaves noisy texture untouched.

Measured on a synthetic frame (sky gradient, lit sphere, checkered ground,
noise panel) put through the real tables, mean error against what was
rendered, and the worst single channel:

| | mean | worst |
|---|---|---|
| dithered, as the card shows it today | 3.11 | 14 |
| `filter=on` (86Box's, threshold 0x202020) | 2.91 | 42 |
| a naive 2x2 box, for reference | 4.94 | 90 |
| `undither=on` | **0.92** | 14 |

The box filters are *worse than no filter at all* on that frame: they pull
the checkerboard and the noise panel about. The undither cuts the error to
under a third of the unfiltered frame's, and its worst case *is* the
unfiltered worst case, because where it cannot fire it writes the
unfiltered pixel.

**The check is `voodoo-guest-undither`** (`UNDITHER=on
tools/voodoo-guest-test.py`), and the dither phase is the oracle for it:
that scene is one grey (130,130,130 — it dithers in every channel) drawn
over the whole screen and blended onto itself, so a correct undither has to
bring it back *flat*, at the colour the program drew rather than a level
off it. Through the real device it does: `0 interior pixels are not the one
colour (130, 130, 130)`, against the 4x4 tile the same scene is with the
setting off. The outermost two rows and columns are exempt — their window
is clamped at the edge of the screen and so has fewer than sixteen phases
in it.

It runs in `voodoo2_present()` — our own file, so no vendored edit and no
patch — over the front buffer, on the main loop with the BQL, once per
presented frame. **Cost: 1.4 ms a frame at 640x480** on the M1 Air, measured
against the shipping routine with no QEMU around it, and got there in three
steps from 4.7: the hot loops written plane-at-a-time over contiguous bytes,
the CLUT lookup skipped when the ramp is the identity one, and then
`combine_span` forced to vectorize — clang's cost model declines it, and it
is worth 2.2x on the routine's hottest loop (1.88 ms of the frame to 0.86).
By stage, at 640x480: `decode_row` 0.4 ms (three table lookups a pixel, a
gather, the one part that stays scalar), `quad_row` 0.1, `emit_row` 0.9.

That is a BQL hold, so it is in the same family as the 3D-race stalls
`tools/audio-glitch-test.py` counts — well clear of the 10–14 ms ones that
made it click, but it is per presented frame, so at Quake II's 147 fps it is
~20 % of the main loop's core and at a 60 Hz cap ~8 %. Off by default.

**Why not on the GPU.** The player could do this in its filter chain for
nothing — librashader is already there, and the phase is `(y & 3, x & 3)` of
the surface either way. It is not where this belongs, for three reasons.
The dither is a property of the *card*, not of the monitor, and the player
is the monitor (doc 03); the surface the player gets has been through the
CLUT, so a shader can only invert the codes while that ramp is the identity
one; and above all a shader's output exists only in the player's window,
where **no QMP screendump can see it** — so the check that proves this
correct, and every headless game tool that judges a frame, would be looking
at the unfiltered picture. A frame that is right only where nothing can
measure it is not what this is for. It reads `fb_mem` rather than `frame->line[]` because the
stored 565 codes are where the dither is; `frame->line[]` is what 86Box
already made of them. Consequences: the filtered pixels land in the console
surface, so **QMP screendumps, the VNC fallback and the player all see
them** (a player-side shader would not show in a screendump), and dirty-line
tracking stops mattering for correctness since every present rebuilds the
whole frame.

It declines a frame it cannot answer for, says why once, and the ordinary
copy runs instead: the guest is not dithering (`FBZ_DITHER` clear in the
last `fbzMode` — an LFB-blitted menu, a 2D screen), the colour buffer is
tiled (not scanned out linearly), or the front buffer would run past the
frame buffer. `VOODOO2_UNDITHER_PATTERN=4x4|2x2` overrides the pattern
`fbzMode` reports, for the A/B when a frame looks wrong.

`undither=on` supersedes `filter=on` on any frame it accepts: it writes the
whole surface itself and never looks at what the scanline filter did.

## 13. One order: the ring, the teardown, and the LFB

The chip has one way in from the PCI bus. A packet in the command FIFO, a
write into the LFB or texture aperture and a register write all reach it in
the order the guest made them. 86Box has **two** queues — `voodoo->fifo`,
where a frame-buffer or texture write is queued, and the ring — and
`voodoo_fifo_thread` empties the whole of the first before it looks at the
second (`vid_voodoo_fifo.c`: the `while (!FIFO_EMPTY)` loop, then the
`while (voodoo->cmdfifo_enabled && ...)` one). A register the vCPU thread
writes does not queue at all. So the ring is the stream that can be
overtaken, and it is what hangs a guest at the end.

### The teardown hang

Glide's `grSstWinClose` writes its last packets and then clears fbiInit7's
command-FIFO bit. That write is applied at once while the ring is still
being consumed, and 86Box's consumer loop ends the moment `cmdfifo_enabled`
goes false — so whatever it had not reached is never run, and
`cmdfifo_depth_rd != cmdfifo_depth_wr` for ever. `SST_status`'s busy bit
(0x380) is built from exactly that difference (`vid_voodoo.c`, `case
SST_status`), so the card reads **busy to every later poll**, and the poll
is Glide's own `grSstIdle`.

Carmageddon's 3dfx build hung there on 2026-09-18 — the user's own run, the
DOS build in a Win98 DOS box on `base98-us`. The DOS box spun for minutes
while Windows carried on around it. The log says it plainly, and the shape
is worth knowing:

    voodoo2: command FIFO through MMIO (ring 001c2000+40000)
    voodoo2: 640x480 on: 122 frames (1 new), 0 triangles, 4 writes, 27298405 reads
      in 5.0 s; regs read 0x000:27298401; busy: 0 cmds outstanding (wr 49668 rd
      49668), fifo depth 59495109/59495111

Nothing is busy — no commands outstanding, no `voodoo_busy`, no render
thread — and the depths differ by two words of the 104 the last walk
counted. Twenty-seven million reads of register 0x000 in five seconds is
the guest asking about those two words.

**The fix, in our own file** (`voodoo2.c`, "one order"): before a write that
reconfigures the command FIFO — fbiInit7 clearing the enable, or the ring's
own pointers — **run the ring out**. Everything the guest wrote before that
access has just been counted by `voodoo2_fifo_sync()`, so waiting for the
consumer to catch up puts the register behind the packets, where the bus
puts it. The wait sets `voodoo->flush`, which is 86Box's own escape for a
thread draining from the guest's side: a swap in the ring then completes
without waiting for a retrace the display timer cannot deliver while the
vCPU holds the BQL. It is bounded (250 ms) and says so once if it ever runs
out — a consumer parked inside a packet the guest has not finished writing
leaves the depths *equal* and returns at once, so the bound is for a stream
this walk has mis-counted, nothing else.

Behind it, unconditionally: if a write leaves the command FIFO off with
words counted and not run, the depths are equalised and the card goes idle.
Nothing is ever going to run those words, and a card that reads busy for
ever is worse than a lost packet. It fires only on the off transition, so a
`cmdFifoDepth` write made with the FIFO already off — which sets a
difference on purpose — is never touched.

**The check** is the teardown phase of `tools/voodoo-guest-test.py`: a cyan
fill and a swap go into the ring and the FIFO is turned off at once, with no
idle wait at all. The frame has to be cyan (the packets were run, not
dropped) and the status register has to read idle afterwards. Both halves
fail with `LFB_ORDER=off`.

### One thing the DOS program had to learn: swapbufferCMD twice

While writing that phase the card read busy with nothing pending, and the
reason is in 86Box rather than here. On a Voodoo 2 a swap arriving as a
**packet** does not increment `cmd_written_fifo` — only Banshee and later
count one (`vid_voodoo_fifo.c`) — while every card's swap increments
`cmd_read` (`vid_voodoo_reg.c`). A program that sends the packet alone
therefore drives `written - cmd_read` negative, and the status register's
busy bit is that difference. 3dfx's Glide never trips it because it writes
`swapbufferCMD` to the register window as well as putting the packet in the
ring — with the FIFO on that write only counts, and it is where the swap
backlog in status bits 31:28 comes from. Carmageddon's stream shows exactly
one of each per frame (`written 0x128:301` beside 301 swaps). The guest test
does the same now.

### The flashing HUD: the other direction, and the ring is never empty

What the guest writes through the LFB **before** a batch of packets has to
be on the card before they are — and 86Box's `voodoo_fifo_thread` drained
its memory FIFO once per wake and then stayed in the ring for as long as
the guest kept feeding it. In a race that is the whole frame. Every 5 s line
of Carmageddon's race says so:

    busy: 1 cmds outstanding (wr 58305 rd 58304), fifo depth 66330585/66349779,
    voodoo_busy

Nineteen thousand words behind, all race long. So the HUD the game writes
with `grLfbWriteRegion` — ~5.2 M LFB writes across 300 frames, about 23,000
dwords a *new* frame — waited in the memory FIFO while the swap that
followed it in the ring was consumed, and landed in the buffer that swap had
just turned into the back one. It showed a frame late, or not at all.

The user's two screenshots of one race, 26 s apart, say it exactly. In the
first the HUD is whole. In the second the driver's portrait and the panel
rectangles are there and **the sprites inside them are gone** — the top bar,
the gauges, the speedometer, the damage map, each replaced by the flat panel
it is drawn on. What survives is what the game draws as geometry, through
the ring; what disappears is what it writes through the LFB.

**And the first patch 72 was backwards.** Making the ring loop yield the
moment anything appears in the memory FIFO fixed that direction and broke
the other: an LFB write then jumped *ahead* of ring words the guest had
published before it and the consumer had not reached — and the consumer is
20,000 words behind. The HUD landed before the world geometry of its own
frame and was painted over. The user's run: **the HUD almost never
appeared**, where before it had mostly appeared. Every other measure of that
run was clean — 301 frames, 301 new, 60 Hz, `0 packets part-written`, and
`LFB writes: 0 to the front buffer, 4446671 to the back, rows 0..469`, which
is the HUD going exactly where the game means it to go.

**So neither queue goes first: they interleave.** Patch 72 gives each memory
FIFO entry the ring's write pointer as it was queued (`cmdfifo_mark`). The
entry runs once the ring has been consumed that far and not before, and the
ring loop yields the moment the memory FIFO's head is due. That is the
guest's own order, with no wait anywhere — the same work, sequenced. A
sequence number is what the thread never had, and every arrangement without
one is wrong in one direction or the other. **The user's run
confirmed it 2026-09-19: the flicker stopped.**

One clause of it is not about ordering at all. A mark the ring can never
reach — which a `cmdFifoDepth` write makes, because it puts the read pointer
back to nothing under entries already queued — is *stale*, not in the
future, and is taken as due. Without that, such an entry waits for a pointer
that is not coming back and the next guest write blocks in
`voodoo_queue_command` on a FIFO that never empties. Draining the memory
FIFO from the guest's side instead is the obvious alternative and it is
wrong: it means holding 86Box's `flush` while a swap goes past, and `flush`
flips the buffer where it stands rather than at the retrace. Measured — it
tore the teardown scene's frame exactly 64 rows down, which is where the
beam was.


The vCPU waited for the memory FIFO instead for a day, at the ring's publish
point, and Carmageddon priced that: **3,200 waits and 1.8 s of vCPU time per
5 s**, about thirteen LFB-then-ring turns a frame — one per HUD element.
(The frame rate went *up* all the same, 40–46 to 50–56 new frames a second,
because the guest stopped spinning on the status register: 7 M reads per 5 s
became 950.) What is left of it in `voodoo2.c` is the count, `N publishes
behind the LFB queue` in the 5 s line, on both transports: it is the number
of times the ring published words with the memory FIFO not yet empty, which
is the situation patch 72 handles.

Neither reaches one case: a consumer already parked inside `cmdfifo_get`
waiting for the rest of a packet the guest has not finished writing. It
cannot drain the memory FIFO from there, and holding the words back would
deadlock it. That shows as `partial` in the same line, and is 0 in every run
so far.

**What `lfb-order` (off by default) adds** is the mirror: a wait for the ring
before an LFB or texture write, so a packet already counted is drawn first.
That window is only as wide as the consumer's wake, and the ordering phase of
the guest test measures it away on an unloaded host — the block lands on top
with the switch either way. It is kept as the A/B rather than turned on.

**The check** is the ordering phase: the ring gets 192 full-screen red
fastfills — a *loaded* ring, because a single one is consumed before the
guest has written its second LFB dword and no arrangement can fail — then
the guest writes a 38,400-dword blue block through the LFB, then the ring
gets the swap. The block has to be on top, and the device has to report at
least one publish behind the memory FIFO. It is a correctness statement
rather than a discriminating guard: measured both ways on an unloaded host,
the block lands on top with and without the patch, because the window needs
a rasterizer that is actually behind, which is a game under TCG. The game is
the oracle here, and the 5 s line is how to read it.

### A card cannot be busy with nothing to do

`SST_status`'s busy bit is four things or-ed together, and one of them is
`cmd_written + cmd_written_fifo - cmd_read`, a running difference that no
event ever resynchronises. On a Voodoo 2 it is not symmetric: a swap arriving
as a command-FIFO *packet* increments `cmd_read` and not `cmd_written_fifo`
(only Banshee and later count one), while the register write Glide makes
beside it increments `cmd_written`. The two are meant to cancel, and when for
any reason they do not, the difference stands for ever — the card reads busy
to every later poll and `grSstIdle` never returns.

Measured 2026-09-19 on `ramfifo=off`: Carmageddon froze on its first race
frame with `wr 3243 rd 3242`, **one** command outstanding, the ring caught up
(`fifo depth 631660/631660`), nothing rendering, and the guest reading
register 0x000 26 million times in five seconds.

So when the guest is polling status and every real sign of work is clear —
both FIFOs empty, the ring caught up, no render thread, no swap pending, the
consumer not in its loop — the difference is stale and is put back. The
hysteresis is what makes it safe: the consumer is briefly between dequeueing
a command and counting it, and in that window the card looks exactly like
this, so it takes 20,000 status reads in a row, which only a spinning guest
can produce. It says so once when it fires.

**A run of those polls ends when the guest really adds a command**, not when
it writes anything at all — that is the difference between a rule that fires
and one that does not. Carmageddon's race carried one stale outstanding
command for its whole length while writing the card thousands of times a
second, so the card read busy to every poll and the guest read the status
register **9 to 11 million times per 5 s** over it (2026-09-19). Between two
swaps that is ~35,000 reads of one register, all of them the answer "busy"
about a command that finished long ago.

### What the 5 s line says about a game's LFB writes

`LFB writes: F to the front buffer, B to the back, E elsewhere, rows lo..hi`
— the buffer each write was aimed at, read from `fb_write_offset` against
`params.front_offset` / `back_offset` as the guest made the write, and the
rows it touched. It is there because "which buffer does the HUD go into" is
the question a flickering overlay asks, and nothing else in the line
answers it. A handful of writes land on the wrong side of a swap the
consumer is making at that moment, so treat a small `front` count beside a
large `back` one as noise; the shape is what matters.

### The menu that looked wide is the game's own letterbox

Reported in the same run and measured out of the screenshots rather than
argued: the player draws the guest's 640x480 frame at exactly 3x with square
pixels, 1920x1440 in a full-width window, the CRT preset's scanline period 3
host rows throughout (autocorrelation on the shot: peaks at lag 3, 6, 9).
The menu's artwork is the middle 1,206 of those 1,440 rows — 402 source rows
of 480, i.e. **400 rows of art with 40 black rows above and below**. The
guest writes 153,601 dwords a frame, which is 640x480 at 16 bpp exactly, so
the black bands are in the guest's own frame buffer: Carmageddon puts a
640x400 front end in a 640x480 Glide buffer. Nothing in the display path
stretches anything, and the race frames from the same session measure 4:3 to
four decimal places.

### The ring through MMIO counts what is contiguous (2026-09-21)

`ramfifo=off` had one freeze of its own left, and it was the transport's.
Carmageddon's 3dfx build froze on every race start on it (the user,
`/tmp/launcher.log` with `VOODOO2_TRACE=1`): the 5 s line read `11
refused`, the first of them 86Box's `fatal()` on `Banshee 2D register
00000020=02020202` in the middle of a texture download, and after it 27
million `cmdFifoRdPtr` reads per 5 s with nothing written — Glide waiting
for room on a ring whose read pointer had stopped. The guest's ring
content, all 65,503 words of that lap taken out of the trace, parses
cleanly end to end by 86Box's own packet rules, so the decoder was wrong
about no packet: the consumer had read a word before the guest wrote it.

How: **Glide writes a two-word packet value first, header second** — 182 of
the 65,503 words, every one the single-register packet (`color1`,
`fastfillCMD`, `swapbufferCMD`: header at `n`, value at `n+4`, written
`n+4` then `n`). The chip is built for that: `cmdFifoAMin` / `AMax` /
`Holes` count what has been written *contiguously* and the depth advances
over that alone, so a header whose value is missing is never seen. 86Box's
`voodoo_writel` counts `cmdfifo_depth_wr++` on every write to the window,
whatever its address, and its consumer reads one word from the read
pointer for every count. A consumer that has caught up — at a race's start
it has, the guest being the slow side while it decompresses textures —
sits on the header's slot, the value's write wakes it, and it reads that
slot as it is: whatever the last lap or the last session left there, taken
as a header. A stale word that says "256 values follow" (a type-1 header,
`num` in bits 31:16) eats the next 256 words as data and parks wanting
more; the first word it then lands on with `2` in its low bits is a
Banshee packet on a Voodoo 2, the `fatal()`; and the read pointer stops
where the guest can never make room. The desync surfaced 2,300 words after
the last swapped pair, which is what a swallowed run looks like.

The device now does the chip's counting on this path
(`voodoo2_mmio_ring_write`): the word is stored where 86Box's handler put
it, and counted for the consumer only once every word before it has
arrived — a word ahead of the next expected one is held in a 64-bit bitmap
and counted when the gap closes; a write below the expected one is the
guest continuing at the base after its jump packet, and a hole still open
at that moment is counted rather than stranded, with a warning. The ring's
own registers (`cmdFifoBaseAddr`, `RdPtr`, `Depth`, `fbiInit7`) start the
count over. It replaces two lines of `voodoo_writel` and touches no
vendored file. The 5 s line says `N ring words written ahead of a hole, M
counted with one open`, and the first out-of-order write is named once.
The ring in RAM never had the problem: its walk stops on a poison header
and counts the pair when both are there, which is why the freeze was
`ramfifo=off`'s alone.

The `voodoo-guest` check's **swapped-pair phase** holds it, and it is the
`voodoo-guest-mmiofifo` variant that discriminates: a first lap plants a
type-1 header claiming 256 values in the ring's second slot (as a colour,
run and harmless), the ring is set to its base again, a NOP brings the
consumer to rest on that slot, the value of a `color1` packet goes into
the third slot, `cmdFifoDepth` is polled to zero so the consumer has had
its look, and only then the header goes into the second, followed by a
fill and a swap. The frame has to be yellow. Counted per write, the stale
header eats the fill and the swap; in RAM the restart poisons the slot and
the phase only has to complete.
