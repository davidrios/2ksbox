# 21. The Voodoo 2 device (M14)

A real 3dfx Voodoo 2 on the machine's PCI bus. It is 86Box's emulation of
the chip (Sarah Walker's PCem rasterizer with 86Box's x86-64 and ARM64
recompilers), vendored verbatim and wrapped as a QEMU PCI device,
`-device voodoo2`. The guest runs 3dfx's own drivers and the game's own
`glide2x.dll` / `glide3x.dll` / `GLIDE2X.OVL` against the registers they
were written for. Nothing in the guest is ours.

The decision is ADR-016 (doc 10). The argument that opened it is
`patches/openglide/README.md` §"Emulating the chip instead". Track
state, test loop and open items are in `docs/tracks/m14-voodoo2.md`, the
test tools in `docs/testing.md`, and the Glide pass-through beside the
chip in doc 12 §5.

## 1. Why a chip, when there is a pass-through

Doc 12 §5 gives a Glide game a *wrapper*. qemu-3dfx's `hw/3dfx` carries
the guest's Glide calls to the host, where our OpenGLide build turns them
into OpenGL. That is fast, because the host GPU draws, but a translation
is only as complete as the wrapper. Glide 3 is missing (62 entry points),
the era's LFB tricks each need a patch (`05-lfb-locked-swap` for
Carmageddon), and a 1996 title with Glide linked statically cannot be
reached at all.

The chip is **complete by construction**: Glide 2 and 3, the DOS
overlay, static links, every LFB and texture-format corner. It costs a
software rasterizer on host cores at a Voodoo 2's own limits (800×600,
16-bit, 256×256 textures), and a trap on the vCPU thread for every
access the guest makes to the card that is not in RAM (§9).

**It does not replace qemu-3dfx.** Its Glide half (`hw/3dfx`, the guest
`GLIDE2X.DLL`/`.OVL`, OpenGLide) stands beside the chip. The wrapper is
for speed, the chip for fidelity, and the machine form picks. Its OpenGL
half (`hw/mesa` and the guest `opengl32.dll`) is what GLQuake, Half-Life
in GL mode and wglgears use. A Voodoo 2 covers those only through 3dfx's
period MiniGL/ICD at rasterizer speed. So the pass-through stays for
OpenGL titles and for hosts where the rasterizer does not keep up. The
Direct3D device (docs 14/15) is a third, separate path.

## 2. What is vendored, and from where

`voodoo/86box/` holds 86Box's `src/video/vid_voodoo*.c` and
`src/include/86box/vid_voodoo*.h` at the commit in `voodoo/86box/UPSTREAM`,
**unmodified**. The port lives in the shim and the device, so a newer
upstream drops in with `scripts/sync-86box-voodoo.sh <commit>`. Changes
86Box's own code needs are QEMU queue patches on the *overlaid* copy in
`qemu/hw/voodoo/86box/` (64, 71, 72; §7, §9, §13), each dropped once
upstream has it. Not vendored are the Banshee / Voodoo 3 files (the four
entry points the Voodoo 1/2 code still calls are stubbed, unreachable for
`type < VOODOO_BANSHEE`) and the 32-bit x86 recompiler. The licence is
GPL-2.0-or-later.

The emulation is one 3dfx *set* (`voodoo_set_t`) of one card, fixed at
`VOODOO_2`. A Voodoo Graphics is a property away but has no command FIFO
(§9), so it is not offered. SLI is not modelled.

## 3. Layout

```
voodoo/
  86box/            the vendored sources + UPSTREAM
  shim/cpu.h        what "cpu.h" resolves to for them
  shim/86box/*.h    86Box's platform headers, reimplemented (§4)
  voodoo_shim.h     the device <-> shim seam (hooks, config)
  voodoo_shim.c     the shim's implementation over QEMU
  voodoo2.c         the PCI device
  undither.c/.h     the scanout undither (§12)
  meson.build       -> hw/voodoo/meson.build
patches/qemu/62-voodoo2-device.patch   subdir('hw/voodoo') in meson.build
scripts/sync-86box-voodoo.sh
```

`scripts/prepare-qemu.sh` rsyncs `voodoo/` to `qemu/hw/voodoo/`. Its
meson builds the 86Box files as their own static library, with `-msse2`
on x86-64 and 86Box's warning set. The include path is `hw/voodoo` for
`<86box/vid_voodoo_*.h>` and `hw/voodoo/shim` for the rest, so their
`"cpu.h"` is the shim's and never `target/i386/cpu.h`. The library is
`link_whole`d into the i386 sourceset, so `qemu-system-i386` and
`libqemu-embed` both carry it.

## 4. The shim: 86Box's platform over QEMU

Everything the vendored files ask of 86Box (found by grep), and what
answers it:

| 86Box | Here |
|---|---|
| `thread_create/wait` | `qemu_thread_create` (joinable) / `qemu_thread_join` |
| events (`thread_create_event`, `set`, `reset`, `wait_event(ms)`) | manual-reset over `QemuMutex` + `QemuCond`; `wait_event` returns 1 on timeout, 0 when set, and `< 0` waits for ever, as upstream's does |
| mutexes | `QemuMutex` |
| `timer_add`, `timer_advance_u64`, `timer_set_delay_u64`, `timer_is_enabled`, `timer_get_ts_int`, `TIMER_USEC`, `tsc` | `QEMUTimer` on `QEMU_CLOCK_VIRTUAL`; units in §6 |
| `mem_mapping_add/set_addr/disable` | records the handlers; the device's `MemoryRegion` calls them (§7) |
| `pci_add_card`, `pci_burst_time`, `pci_nonburst_time` | recorded / constants; the device forwards config bytes (§7) |
| `plat_mmap(size, executable)` | `mmap` (+ `MAP_JIT` on macOS), `VirtualAlloc` on Windows |
| `plat_timer_read`, `plat_delay_ms` | `get_clock()`, `g_usleep` |
| `device_get_config_int(name)` etc. | a table the device fills from its properties before `voodoo_init` |
| `cycles -= …` (charging the CPU for a PCI access) | a dummy int; TCG charges nothing beyond the trap |
| `svga_get_pri`, `svga_set_override`, `svga_doblit`, `monitors[].target_buffer` | the display path, §7 |
| `calloc` / `free` (macros in the shim's `86box.h`) | allocations of 1 MB and more (frame buffer, texture memories, `voodoo_t`) get **64 MB of zero pages after them**. 86Box's display timer indexes `fb_mem` by `front_offset + line * row_width` with no bound, from registers a guest sets freely (Glide put 6.8 MB into 4 MB on a reopen). An overrun reads zeros instead of faulting |
| `fatal()` | **returns**, unlike upstream: the write is refused and counted, and the first is dumped (§8) |

The shim's `86box.h` keeps upstream's atomics verbatim (`volatile` on
x86, C11 atomics elsewhere), because the FIFO/render thread protocol is
written in them.

## 5. Threads and the one invariant

86Box runs the guest CPU, the device's register handlers and its timers
on **one thread**, with the Voodoo's FIFO thread and 1–4 render threads
beside it, and the code assumes it. `voodoo_callback` (the display timer)
and `voodoo_writel` share `line`, `front_offset`, `dirty_line` and
`swap_pending` with only `swap_mutex` between them.

Here the register handlers run on the vCPU thread and the timers on the
main loop, **both with the BQL**, so they exclude each other the same
way. The FIFO and render threads are 86Box's and touch nothing of
QEMU's. The region is therefore **not**
`memory_region_clear_global_locking`'d, which would run the handlers
beside the timer unlocked. Doing so would need a lock of our own between
the handlers and the display timer. With the ring in RAM (§9), what still
traps is mostly status polls, so it is not worth it now.

Upstream already handles the one deadlock. The vCPU can block in a
Voodoo wait with the BQL held (`voodoo_flush` on a register read,
`voodoo_queue_command` on a full FIFO, LFB reads). Meanwhile the main
loop cannot fire the display timer, whose retrace completes a pending
swap the FIFO thread may wait on. 86Box has the same shape, and
`voodoo_wait_for_swap_complete` breaks the wait when `flush` is set or
the FIFO is full, the two cases in which the vCPU blocks. A status poll
(how Glide waits for a swap) is not a block, because the vCPU drops the
BQL between reads.

## 6. Timers and their units

86Box timers hold a 32.32 timestamp in TSC ticks. Here a *delay* is
nanoseconds of `QEMU_CLOCK_VIRTUAL` in that same 32.32 form, so the
timers follow `-icount` and stop with the VM. `TIMER_USEC = 1000 << 32`
and `cpuclock = 1e9`, so `voodoo_pixelclock_update`'s `clock_const` is ns
per pixel and `line_time` a delay in these units. `tsc` is the virtual
clock in ns, which keeps `hvRetrace`'s "time to the end of this line"
consistent. The *expiry* a timer holds is `ns << 16`: 48 bits of
nanoseconds (78 hours) with a fraction fine enough that a non-integer
line time does not drift. **It is not 32.32.** That form wrapped 4.3 s
after boot. A wrapped expiry re-armed in the past, `timerlist_run_timers`
never left it, the PIT starved and QEMU stopped answering SIGTERM.

The **display timer fires per scanline** (`line_time`, ~32 µs at
640×480). That is cheap on 86Box's CPU thread but 31 000 BQL-taking
wakeups a second on QEMU's main loop. The shim coalesces them. A timer
whose callback re-arms it less than `slack_ns` ahead is called again in
the same wakeup (`shim_timer_fire`), and the display timer has 1 ms of
slack, so the main loop wakes ~1000 times a second and runs ~32 lines
each time. `voodoo_callback` is a state machine over `line` with no
dependence on wall time between lines. The only visible effect is that
`SST_vRetrace` advances in steps and `hvRetrace` is coarse. The wake
timer (the FIFO thread's 100 µs batching delay) gets no slack.

## 7. The device: PCI, MMIO, display

**Identity.** Vendor `121a`, device `0002`, revision 2, class 0400
(multimedia video), one 16 MiB memory BAR. Configuration bytes 0x40–0x43
are `initEnable`, answered by 86Box's handler (byte 1 carries the `0x50 |`
strap 3dfx's driver reads to know it has a Voodoo 2). The device echoes
the command register and the BAR's top byte to it, so the card's
`pci_enable` / `memBaseAddr` track the PCI core.

**0x54 is `siProcess`**, the silicon-process monitor, and the device
models it. Glide's `sst1InitMeasureSiProcess` loads a PCI-clock
countdown into bits 27:16, sets RUN (bit 28), polls until the field reads
zero, then takes the ring-oscillator count from bits 15:0. QEMU keeps
whatever a guest writes past the 64-byte header, so without this model
the loaded count read back for ever and **every Glide program froze in
`grSstWinOpen`** at ~30 M reads/s (86Box answers 0 to the whole
register). Here the countdown is over as soon as RUN is read back, and
the count is a production board's (8000, past Glide's 5000 threshold).

**MMIO.** The BAR is a `MemoryRegion` whose read/write call the handlers
`mem_mapping_add` recorded (`voodoo_readw/readl/writew/writel`, which mask
to 24 bits: registers, LFB, texture space, command-FIFO window). Byte
accesses get what the hardware gives, `0xff` / nothing, since 86Box
registers no byte handler. The memory core splits wider accesses (an SSE
store into the LFB) into dwords. The ring's window is guest RAM by
default (§9).

**`fbiInit1` bit 23, scanline interleaving, is masked.** This is one card
with no partner, and 86Box's display timer takes SLI at its word and
draws the odd lines from `set->voodoos[1]`, which is NULL here. A stray
write walking the register file set it (§11). A real single board with
the bit set shows half its lines. This one ignores it.

**Packet 3's packed colour** (patch 71). Packet 3 names its per-vertex
parameters in bits 17:10, and bit 28 says the colour is one packed ARGB
word. 86Box read that word only under the RGB bit, and a separate alpha
float only with bit 28 clear. Glide sends **iterated alpha over a
constant colour** with the packed bit set and the RGB bit clear
(`gglide.c`). That is one word a vertex that both consumers skipped,
leaving the ring desynchronised. Our packet walk (§9) counts by the same
rule and was fixed with it. The device says once when it meets such a
packet.

**A consumer stuck inside a packet** is the shape every ring desync
takes. 86Box's `cmdfifo_get` waits for the next word whenever
`depth_rd == depth_wr`, so a header promising more words than Glide wrote
parks the FIFO thread with `voodoo_busy` set. Glide polls for idle before
writing more, so it never frees it. A guest reset does not reset the
card, so the next login's helper (§11) hangs on it too. After two 5 s
windows of it the device prints `voodoo2: the command FIFO is stuck
inside a packet` with the awaited word, the ring around it and the
guest's last 64 accesses. It also warns on a packet word written
narrower than a dword, which 86Box drops (its `writew` takes only the
frame buffer), and logs Glide's device probe (every LFB readback with the
FIFO off: frame-buffer sizing, TMU strap, texture-memory sense).

**Display.** A Voodoo 1/2 is a pass-through card. With `fbiInit0`'s
VGA_PASS bit it drives the monitor instead of the 2D adapter. 86Box
models that as overriding the primary SVGA. The shim turns
`svga_set_override` into `graphic_hw_passthrough(console 0)` (patch 30's
hook, which skips the VGA device's `gfx_update` while it is set). It
turns `svga_doblit`, the display timer's end-of-frame call, into a copy
of the monitor bitmap into a `DisplaySurface` of ours and
`dpy_gfx_update_full`. The frame therefore reaches the player through the
ordinary VGA surface path, and a QMP `screendump` and the VNC fallback
see it. There is no 3D-frame path and no player change. The monitor
bitmap is 4096×4224, lazily mapped, because `h_disp`/`v_disp` are 12-bit
fields the display code indexes unchecked.

When VGA_PASS clears, the console is invalidated and the VGA draws again
into its own surface. So **an invalidate must put the adapter's own
surface back on the console**, not only repaint. QEMU's VGA core does.
`d3dpt-vga` once re-installed its surface only on a mode change, so a
Win98 machine with the card kept the Voodoo's last frame after every
full-screen switch. It re-surfaces on every invalidate now, and
`voodoo-guest-d3dpt` checks it. The same check covers patch 66, which
hides the adapter's hardware cursor while the Voodoo has the monitor.

**Black until the first swap.** At a takeover or a change of
`h_disp`×`v_disp` the frame buffer holds 3dfx's init test patterns or the
last mode's contents at a new pitch, a grey pattern flashing before the
picture settles. A real monitor was dark while it re-locked, so the
device shows black until the guest swaps (`front_offset` moves, or
`frame_count` for a swap on a retrace) and one frame more (a mid-frame
swap leaves the lines above it stale). A guest that never swaps is shown
after 2 s (`VOODOO2_BLANK_MS`). While black, every line is marked dirty
at each present. The display timer presents only frames with dirty
lines, and a guest that swaps once and draws nothing more would
otherwise stay black.

**Reset.** 86Box has no reset entry (the driver re-inits the card), but a
guest reboot must give the monitor back. The device clears `fbiInit0`,
`fbiInit7`, `initEnable` and the mapping state and drops the override.

**Log.** Every 5 s of activity:

    voodoo2: 640x480 on: 61 frames, 12034 triangles, 480211 writes (23011
      texture), 3122 reads in 5.0 s; regs read 0x000:2981 0x218:19;
      written 0x120:279 0x114:263; config read 0x040:12

`frames` counts presents (swaps with dirty lines) with an `(N new)`
column (§9). The rest are 86Box's counters. The three histograms are the
four most-read registers (by `addr & 0x3fc`), the four most-written and
the four most-read configuration dwords, so **a spinning guest names
what it spins on** (`0x000` is `status`, and `0x054` in the config
column was the siProcess loop). Further clauses appear when they have
something to say: refused writes (§8, §11), the RAM ring's words and
part-written packets (§9), `busy:` with outstanding commands and FIFO
depths, ordering waits and ring holes (§13), LFB writes by buffer (§13).
`display on (VGA pass-through)` / `off (VGA back)` mark the switch, and
`640x480 shown after 180 ms of black (first swap)` (or `(no swap)`) marks
a new mode's first frame. `VOODOO2_TRACE=1` prints every register and
FIFO-window access, with status polls collapsed to a count, for reading
one open sequence against 3dfx's `sst1init` source.

## 8. Not modelled, and the hostile-guest question

- **SLI** and the **Voodoo Graphics** type are not offered.
- **Interrupts.** 86Box `fatal()`s on a write to `intrCtrl` /
  `userIntrCMD` and on a malformed packet (`CMDFIFO packet 5 bad space`
  and a handful more), which ends 86Box. **Here `fatal()` returns.**
  Every call site `break`s or falls through after it, so the write is
  refused and the stream goes on. The first is printed with the device's
  state (`initEnable`, `fbiInit0/7`, the FIFO's base, end, read pointer
  and depth) and the last 64 accesses. The rest are counted into the 5 s
  line (`N writes refused`). **It is not `noreturn` in the shim's
  header.** Declared so, the compiler dropped the code after the call
  and the return landed in the next case, a SIGSEGV that looked like
  86Box's.
- **Migration.** There is no vmstate, so a snapshot does not carry the
  card. Its state is what the driver re-creates.
- **Byte accesses** to the LFB: none on the hardware, none here.

## 9. Performance: the command FIFO in RAM

86Box's rasterizer draws the frame. The per-pixel pipeline for the
current register state is JIT-compiled (x86-64 SSE2, or ARM64 NEON; 86Box
has the ARM64 one and PCem does not, which is why we took 86Box's code)
and run by 1, 2 or 4 render threads (`threads=`, 2) owning alternate
scanlines, on cores the one-vCPU guest leaves idle. A Voodoo 2 filled
~90 Mpixel/s. A 640×480 game needs 20–30 for 30 fps, which a modern core
does.

The cost that is ours is on the vCPU. Every access to the card is an MMIO
trap, and under TCG an MMIO store that is not a block's last instruction
is a `cpu_io_recompile` (unwind the block, look it up, regenerate one
ending at the store). In a `perf` profile of Quake II's demo on the MMIO
ring that was a third of the saturated vCPU, with the MMIO path proper
another sixth. 3dfx's Glide uses the Voodoo 2's **command FIFO**
(`fbiInit7` bit 8), packets written into a window of the card's memory
for the chip to pull, so that window is where the traps were.

**`ramfifo=on` (the default) makes the ring guest RAM.** It is an alias
of a page-aligned `fb_mem` (86Box's calloc is handed a replacement at
realize and gets its own back at close), so Glide's packets are plain
stores. Glide leaves hole counting on and rings no doorbell, because the
chip is meant to see every write, so the device finds the packets
itself. At every other access the guest makes to the card, it walks the
ring from the last word it counted and adds what has been written to the
depth that the per-dword writes used to add. Consumed words are
**poisoned with `0xdeadbee7`**. The guest learns a slot is free only by
reading `cmdFifoRdPtr`, which the device answers after poisoning up to
exactly what it returns, so the guest never writes over a word that is
not poison. A packet the walk cannot follow (JSR/RET, AGP, Banshee
types) sends the window back to MMIO with a warning. `ramfifo=off` is the
per-dword MMIO path, the A/B.

**Quake II `timedemo demo1`: 41.1 → 147.5 fps**, with the chip now the
busy side (~14 M words a second). UT's CityIntro flyby went 33.6 → 40.7.

The walk keeps three rules, each learned from a hang:

- **The poison is a word no guest writes.** It was `0xffffffff`, a white
  texel. A texture download carrying white read as unwritten ring, the
  walk stopped on the guest's own data, and Glide waited for a read
  pointer that could never move (26 M reads of `cmdFifoRdPtr` in 5 s).
  That was the 3DMark 99 hang on the Windows PC, with half-read packets
  garbling its loading screens. `0xdeadbee7` has packet type 7 (which
  does not exist) in its low three bits and is absurd as a float
  (-2.5e18) or a texel pair. The look-ahead past poison applies to a
  packet's **payload** only. An unwritten header is always a gap, since
  the header is written first.
- **It counts words, not packets.** The chip runs words as they arrive,
  and 86Box's consumer blocks inside a half-written packet by itself, so
  the walk hands over whatever has been written. Glide writes each
  packet whole before touching the card again (the 5 s line's
  `packets met part-written` stays 0), so this is the faithful model
  rather than a fix.
- **The read pointer never passes what the guest has written.** Glide's
  free space is `rp - wp - 1`. Taking unwritten words as data puts the
  read pointer past the write pointer, a state the chip cannot reach.
  The guest then computes a few words of room, waits for space, and the
  empty ring never moves again. FIFA 2000's loading screen died that
  way: a 66-word type-5 LFB packet with 19 words written, 47 taken as
  data after 64 idle polls, the guest asking for 66 with 46 of room, and
  22.7 M `cmdFifoRdPtr` reads a second for ever. The same game ran
  matches start to finish on `ramfifo=off`. Nothing is taken that the
  guest has not written, however long the wait.

The `voodoo-guest` check drives the ring as Glide does (two batches, one
across a JMP, one after a read-pointer read). Under `ramfifo=on` only the
walk can have drawn the frames. Its **partial-packet phase** checks the
third rule. A two-word packet's header is written and its value is not,
`cmdFifoRdPtr` is read 256 times, and the pointer must stay at
`00300004` (header consumed, chip wanting the value), then reach
`00300008` when the value arrives. `voodoo-guest-mmiofifo` is the same
with `ramfifo=off`.

**Measuring a game's frame rate.** The 5 s line's `(N new)` column counts
presents that showed a buffer the one before did not. Those are the
game's frames as the monitor shows them, capped at the refresh. `frames`
beside it counts every present with a dirty line (an FMV written through
the LFB reads 225 with 0 new), and 86Box's `frame_count` counts only
swaps that wait for a retrace (0 for a game with vsync off). Two swaps
inside one refresh show as one, so the column is a floor: ~35 against
Quake II's own `timedemo` 41.3 on the MMIO ring. The benchmark recipe is
in the track doc.

### Dither subtraction in both recompilers (patch 64)

A Voodoo dithers what it writes, so a pixel read back for blending
carries its position's dither offset. The chip subtracts it again when
`fbzMode`'s `DITHER_SUB` (bit 19) is set. 86Box gates that on its
`dithersub` setting (`dither-sub=on|off` here, on). Its interpreter
applied it, but neither the x86-64 nor the ARM64 code generator did, and
the recompiler is the default. On a grey blended onto itself at falling
alpha the columns walked away from the colour (7c0f → 6b4d), a grainy
rectangle. Patch 64 gives both generators the interpreter's three table
lookups at the destination read-back (`dithersub_rb` / `dithersub_g` or
the 2x2 pair, keyed on the dither write's x and `real_y`), under the same
two conditions. The block cache already keys on the whole `fbzMode`. Both
emissions were checked against the tables outside a guest (x86-64
natively, ARM64 under Unicorn, every byte value at every dither
position). The `voodoo-guest` dither phase requires every column to equal
the reference and the screendump to be one 4x4 tile, with the recompiler
on and off (`RECOMP=off`). `DITHER_SUB=off` is the control that fails.

### Knobs

| Property | Default | |
|---|---|---|
| `threads=1\|2\|4` | 2 | render threads |
| `recompiler` | on | off: 86Box's interpreter, the A/B for a rasterizer bug |
| `bilinear` | on | |
| `dither-sub` | on | above |
| `filter` | off | 86Box's scanline filter, a no-op until the guest programs `maxRgbDelta` (§12) |
| `undither` | off | the dither reconstructed away at scanout (§12) |
| `fbmem=2\|4` | 4 | MB of frame buffer |
| `texmem=2\|4` | **2** | MB per TMU: the 8 MB board. `texmem=4` is the 12 MB board, whose 4 MB TMUs break a 1997 game (§13) |
| `ramfifo` | on | the ring in guest RAM |
| `lfb-order` | off | a wait for the ring before an LFB write (§13) |
| `fifo-off-regs` | off | on: a FIFO-window write with the FIFO off walks the register file, as 86Box does (§11) |
| `mmio-holes` | on | off: 86Box's per-write count on the MMIO ring (§13) |

On the Windows PC `scripts/win-voodoo-ab.sh <prop>=<value>` runs the
A/B.

## 10. The guest side

Nothing is ours. The card wants **3dfx's Voodoo2 reference drivers**
(Win9x: the `3dfxV2` package, drivers 3.02/3.03, with `glide2x.dll` and
`glide3x.dll`; XP: the last 3dfx reference driver or the community's).
They are the user's own downloads and never go into the repository or
the guest-tools ISO. A DOS Glide game carries its own `GLIDE2X.OVL`,
3dfx's and not qemu-3dfx's. They are two programs with one name, one
talking to the chip and one to `hw/3dfx`.

3dfx's driver detects the card by scanning PCI for `121a:0002` and
reading `initEnable`. A Voodoo 2 with no pass-through cable is still a
Voodoo 2.

**Our guest tools stay out of its way.** 3dfx's driver installs
`GLIDE2X.DLL` / `GLIDE3X.DLL` / `FXMEMMAP.VXD` in the system folder,
which are qemu-3dfx's wrappers' names. SETUP looks for a *present* 3dfx
PCI device (9x: the devnode tree in `HKEY_DYN_DATA`; NT:
`CM_Locate_DevNode`). With one, it leaves `GLIDE*.DLL`, an existing
`FXMEMMAP.VXD` and `GLIDE2X.OVL` alone. The mapper still goes in where
there is none, because our Direct3D and OpenGL DLLs need it. qemu-3dfx's
`FXMEMMAP.VXD` is 3dfx's own binary (4.10.01.0013), so either copy
serves both. A title that should take the pass-through on such a machine
gets ours next to its EXE: `SETUP /GAME 6` (DLLs), `/GAME 7` (the DOS
overlay). `VOODOO=1 tools/setup-guest-test.sh` checks it on 98 and XP.

**The PCI map.** On both 2D adapters SeaBIOS puts the card's BAR at
`0xfd000000`, clear of the fixed pass-through windows (Glide
`0xfb000000`–`0xfb7fffff` and `0xfbdff000`, Mesa
`0xea000000`–`0xefffefff`, d3dpt `0xd8000000`–`0xdfffefff`). Nothing
reserves those windows, so a guest that moved the BAR could overlap
them. An ACPI Win98 keeps the firmware's placement.

**In the launcher** the card is one checkbox on the machine form
("Emulated 3dfx Voodoo 2", `voodoo2` in the bundle, doc 07). It is off
unless picked, offered on every family, and adds
`-device voodoo2,addr=0x05`, the slot after the sound card's, on
whichever 2D adapter. A headless run must use the same slot. Any other
is new hardware and a restart prompt before the shell. Our adapter plus
the chip is the pairing for Win98: Direct3D on the doc 19 driver, Glide
on the chip. The `voodoo2` check walks the checkbox to `query-pci`, and
`tools/voodoo-guest-test.py` exercises the device with no 3dfx code at
all (`docs/testing.md`).

### What real titles do on it

Run by hand on `base98-br` with 3dfx's Win98 driver:

- **Unreal Tournament** is Glide 2 (`GlideDrv.dll` imports `glide2x.dll`,
  and its log says `Found Glide: 2.56`). **Quake II** goes through 3dfx's
  MiniGL, Glide 2 too.
- **NFS Porsche Unleashed is Direct3D on a Voodoo 2 unless told
  otherwise.** Its `3DSetup\3dsetup.ini` sends a Voodoo 2 to `/M:dx`
  (`dx7z.dll`, 3dfx's Direct3D HAL). Its Glide 3 renderer `voodoo2z.dll`
  is for a Voodoo 1 or Rush. 3D Setup (which must be run once) writes one
  value, `HKLM\SOFTWARE\Electronic Arts\Need For Speed - Porsche 2000`
  `Thrash Driver` = `dx`. Setting it to `voodoo2` (a `.reg` through
  `regedit /s`) is the whole switch, and gives the menu at ~50 fps
  against Direct3D's 30.
- **Porsche's green tyre smoke is the game's, not the chip's.** It is
  green under Glide on the chip and under the pass-through, unchanged by
  `recompiler=off`, and not green in Direct3D at 32-bit. Period reports
  give 32-bit colour as the cure, which a Voodoo 2 does not have. Don't
  debug it again.
- **Carmageddon's DOS 3dfx build** puts a 640x400 front end in a 640x480
  Glide buffer. The menu's black bands above and below are in the
  guest's own frame buffer (153,601 dwords a frame, 640x480x16 exactly),
  not a display-path stretch. Its races measure 4:3.
- **DxDiag (DirectX 9.0c)** shows the card as display 2, "Voodoo2
  DirectX 7 Driver", `3dfxV2.drv` 4.11.0001.1151, DDI 7, 12 MB. Its
  DirectDraw test passes full screen at 640x480 (the Voodoo takes the
  monitor and gives it back). Its **Direct3D 7 test fails at step 46,
  `GetDC`, `0x88760249` (`DDERR_CANTCREATEDC`)**. Whether a real Voodoo
  2 with this driver fails it too is open (the card has no GDI). The
  Direct3D 8 test runs on `d3dpt-vga` instead (DirectX 8 has no device
  for a DDI 7 driver) and the 9 one is skipped. Recipe: `NO_DRIVER=1
  EXTRA='-device voodoo2,addr=0x05'
  GUEST_CMD='C:\WINDOWS\SYSTEM\DXDIAG.EXE' CLICKS=…
  tools/win98-game-test.sh <image> <name>`.

## 11. Two Glides on one card: 3dfx's login helper

3dfx's Voodoo 2 driver installs a Run entry, `Voodoo2`, that runs
`rundll32.exe 3dfxv2ps.dll,UpdateRegSettings` at every login. That DLL
is the driver's DirectDraw/Direct3D HAL, built on **Glide 3**, and the
call brings the board up through Glide 3's own copy of the init library:
`sst1InitMapBoard` (a first map shuts the video down) and
`sst1InitRegisters`, which zeroes the video timing and resets `fbiInit7`,
turning the command FIFO off. On a real card that takes milliseconds.
Under TCG, with the init library's 200 000-iteration clock-settle loops
(`init/video.c`), it takes **3 to 19 s** (11.3 s and 18.7 s on two
`base98-br` logins).

**A Glide program started inside that window loses.** It opens its
window (video mode, `sst1InitCmdFifo(FXTRUE)`, a first swap). Then the
helper's init lands on top of it from another process with separate
static state: display off, video registers zeroed, FIFO off. Nothing
tells the program. It goes on writing packets into the window at
`0x200000`, where with the FIFO off they are register writes: a garbage
`videoDimensions`, `intrCtrl` fatals, `fbiInit1`'s SLI bit, and a guest
spinning on `cmdFifoRdPtr`. With the splash disabled
(`FX_GLIDE_NO_SPLASH=1`) it survives to exit but draws nothing. By hand
this looked like a game freezing with a stray `rundll32` in the task
list. `VOODOO2_TRACE=1` pinned it by naming the module behind each
register write (the PE image around the guest's program counter, walked
back to its `MZ` header). The first init came from `GLIDE2X.DLL` under
GLIDETEST.EXE, the second from `GLIDE3X.DLL` under RUNDLL32.EXE. The
same GLIDETEST started 20 s after the desktop passes every case.

**The device names it.** A config write of `initEnable = 1` (how
`sst1InitRegisters` opens) while the command FIFO is on can only be
someone else's init, because Glide's own close turns the FIFO off first.
The device warns `the card is being re-initialised (sst1InitRegisters)
while a Glide window has the command FIFO on`. Glide's own
close-and-reopen does not trip it.

### The stranded client's packets are refused

The same thing happens at a game's close, where that warning cannot fire
because the close already cleared `fbiInit7`. FIFA 2000 on `base98-br`
ran ~2½ minutes on the card at 30 fps, then did an ordinary
`grSstWinClose` (`command FIFO through MMIO`, `display off (VGA back)`),
then a full `sst1InitRegisters` with the memory probe, and then a client
streamed packets into the window with the FIFO off. A **healthy** reopen
is unmistakable (`VOODOO2_TRACE=1` on GLIDETEST):

    initEnable <= 00005001
    wr 0001e0 020101c2   cmdFifoBaseAddr      wr 0001f0 001c1ffc   amax
    wr 0001e8 001c2000   rdPtr                wr 0001f4 00000000   depth
    wr 0001ec 001c1ffc   amin                 wr 0001f8 00000000   holes
    wr 00024c 0ffb8300   fbiInit7, bit 8: the FIFO on
    initEnable <= 00005003

FIFA's had none of it, and the ring registers still held the dead
session's values. The client had resumed on the old ring after someone
else's init switched the FIFO off under it. It then spun (640 943 status
reads and 919 878 front-buffer LFB writes in 5 s) and the machine reset
five seconds later, with no bugcheck. Why a client resumes on a dead
ring is still open (track doc).

**So the walk is refused by default.** Glide only writes that window
when it believes the FIFO is on, so every dword there with the FIFO off
is a stranded client's packet. Walking the register file with them ruins
the card for everything after: `videoDimensions` zeroed and never
rewritten (no retraces, `status` never idle), `fbiInit7` flipping the
FIFO at random, `intrCtrl` reaching `fatal()`. A real chip survives what
3dfx's Glide does routinely, so the permanence is the emulation's.
`fifo-off-regs=on` is the A/B, the walk as 86Box decodes it. Offsets
below `0x100` still pass either way. Under Glide's alternate mapping
those are the vertex and triangle registers, and a stray triangle
renders and is over.

The warning also **names both sides**, once, without the trace: `the
packet comes from …` (the module and call chain around the guest's
program counter, with its `cr3`) and `the last sst1InitRegisters was …`.
The same `cr3` in both is one program re-initialising under itself. A
different one is a second Glide client.

The `voodoo-guest` stranded-client phase checks this. With the FIFO left
off, a burst goes into the window at the offsets of `cmdFifoBaseAddr`,
`videoDimensions` and `fbiInit7`, and the ring's register must read back
unmoved. `FIFO_OFF_REGS=on` must move it (`03010300 -> 02AD02EF`).

### The guest tools' start-up guard

`SETUP /I 6` on 98/Me with a 3dfx card present ("Voodoo 2 start-up
guard", `guest-tools/src/v2start.c`) moves 3dfx's `Voodoo2` value out of
HKLM's Run key into `HKLM\SOFTWARE\2ksbox\Voodoo2` (`Command`) and puts
`C:\WINDOWS\V2START.EXE` there instead. At login V2START runs the
command itself and waits for it (bounded at 120 s). If the init is still
going after half a second, it shows a small topmost notice, "Voodoo 2
driver is loading, please wait before running 3dfx games". It informs
and does not block, so the desktop stays usable (user decision). When
done it writes `C:\WINDOWS\V2START.LOG`, which harnesses wait on
(`tools/win98-game-test.sh`, with `VOODOO_WAIT` the cap; an image without
the guard gets a fixed wait). A driver reinstalled after SETUP puts the
Run value back. V2START moves it again at the next login and waits for
the rundll32 Explorer already started rather than starting a second
init. A machine whose 3dfx DLL is gone runs nothing. With the guard,
GLIDETEST started the moment the log appeared passes with no
re-initialisation warning.

A guest program that waits at login must not poll (a DOS-box CHOICE
loop made the helper's init twelve times slower). Use
`TESTS\WAITFILE.EXE` (`docs/testing.md`).

## 12. The dither, undone (`undither=on`)

The chip renders colour at more than 16 bits and stores RGB565 through an
ordered dither. 3dfx's RAMDAC box-filtered the scanout, partly undoing
it ("22-bit colour"). Two filters exist here, and they are not the same.

**`filter=on`** is 86Box's, leilei's approximation of the RAMDAC: a pair
of 256x256 blend tables (`voodoo_generate_filter_v2`) applied in
`vid_voodoo_display.c`. On a Voodoo 2 it is a **single-scanline** pass
(`voodoo_filterline_v2()` takes one row, with taps at `src[x±1..3]`), so
it softens the dither along a line and leaves the vertical half of every
pattern. It runs only once the guest has written a non-zero threshold to
**`maxRgbDelta`** (0x230, `SST_scrFilter`, with `initEnable & 1`).
Nothing seeds `scrfilterThreshold`, so `filter=on` alone is a no-op until
3dfx's driver programs it. It is kept as what the hardware looked like.

**`undither=on`** (`voodoo/undither.c`, ours) goes the other way. In an
emulator the dither matrix is not something to approximate. It is the
table the rasterizer dithered *with* (`86box/vid_voodoo_dither.h`,
applied in `vid_voodoo_render.c` indexed by `(real_y & 3, x & 3)`). For a
linear non-SLI buffer, row r is scanned out as row r, so the phase at
scanout is `(y & 3, x & 3)`, known exactly. Inverting the table gives,
per phase and stored code, the interval of 8-bit values that dither to
it. Pixels that came from one pre-dither colour have intervals with a
common member, and the intersection is what that colour can have been.
Two window sizes, in order:

- **4x4, centred.** The interval comes out exactly **one value wide** for
  every value and phase of both 4x4 tables, so the output is the colour
  the rasterizer had, the same value at all sixteen phases.
- **2x2, anchored.** The fallback where a 4x4 holds more than one colour
  (an edge, a steep gradient). It is within 2/255 (`dither_rb` 2,
  `dither_g` 1, both 2x2 tables 1).

The 4x4 stage is not optional. A 2x2's midpoint **moves with the
phase**, so 255 of 256 flat colours come back from a 2x2 alone with a
1-LSB residual pattern. With the 4x4 first, all 256 come back as one
level equal to what was rendered. It is cheap because the 4x4 is the
intersection of four 2x2 results a row already has.

An empty intersection at both sizes proves no single colour could have
dithered into those pixels, so they are an edge, and the pixel is
written exactly as the unfiltered path writes it (`code << 3`,
`code << 2`). **An edge is an arithmetic impossibility, not "a difference
bigger than N"**. There is no threshold, no blur across edges, and noisy
texture stays untouched.

On a synthetic frame (sky gradient, lit sphere, checkered ground, noise
panel) put through the real tables, error against what was rendered:

| | mean | worst |
|---|---|---|
| dithered, as the card shows it | 3.11 | 14 |
| `filter=on` (threshold 0x202020) | 2.91 | 42 |
| a naive 2x2 box, for reference | 4.94 | 90 |
| `undither=on` | **0.92** | 14 |

The box filters are worse than none on that frame, because they smear the
checkerboard and the noise. The undither's worst case is the unfiltered
worst case, because where it cannot fire it writes the unfiltered pixel.

**The check is `voodoo-guest-undither`** (`UNDITHER=on
tools/voodoo-guest-test.py`), with the dither phase as the oracle. One
grey (130,130,130) blended onto itself over the screen must come back
*flat* at exactly that colour (`0 interior pixels are not the one
colour (130, 130, 130)`), where the setting off gives the 4x4 tile. The
outer two rows and columns are exempt, because their window is clamped.

It runs in `voodoo2_present()`, over the front buffer's stored 565 codes
in `fb_mem` (where the dither is; `frame->line[]` is what 86Box already
made of them), on the main loop with the BQL, once per presented frame.
**It costs 1.4 ms a frame at 640x480** on the M1 Air. The hot loops are
written plane-at-a-time, the CLUT lookup is skipped when the ramp is the
identity, and `combine_span` is forced to vectorize (clang's cost model
declines it; 2.2x on the hottest loop). By stage: `decode_row` 0.4 ms
(the scalar gather), `quad_row` 0.1, `emit_row` 0.9. That is a BQL hold,
well clear of the 10–14 ms stalls that made audio click
(`tools/audio-glitch-test.py`), but it is paid per frame: ~20 % of the
main loop's core at Quake II's 147 fps, ~8 % at a 60 Hz cap. **Off by
default.**

**Why not a shader in the player.** The dither is a property of the
*card*, and the player is the monitor (doc 03). The player's surface has
been through the CLUT, so a shader could invert the codes only while the
ramp is the identity. A shader's output also exists only in the window,
where **no QMP screendump sees it**, so the check that proves this, and
every headless tool that judges a frame, would see the unfiltered
picture. Done in the device, the result lands in the console surface for
screendumps, VNC and the player alike. Since every present rebuilds the
whole frame, dirty-line tracking no longer matters for correctness.

It declines a frame it cannot answer for, says why once, and the
ordinary copy runs. It declines when the guest is not dithering
(`FBZ_DITHER` clear in the last `fbzMode`: an LFB-blitted menu, a 2D
screen), when the colour buffer is tiled, or when the front buffer would
run past the frame buffer. `VOODOO2_UNDITHER_PATTERN=4x4|2x2` overrides
the pattern `fbzMode` reports, for the A/B when a frame looks wrong. On
any frame it accepts, it supersedes `filter=on`.

## 13. One order: the ring, the teardown, and the LFB

The chip has one way in from the PCI bus. A packet in the command FIFO,
a write into the LFB or texture aperture and a register write all reach
it in the order the guest made them. 86Box has **two** queues,
`voodoo->fifo` for frame-buffer and texture writes and the ring, and
upstream's `voodoo_fifo_thread` empties the whole of the first before
looking at the second. A register the vCPU writes does not queue at all.
So the ring is the stream that can be overtaken. What keeps the guest's
order is all in `voodoo/voodoo2.c` ("one order") and patch 72:

### The teardown hang

Glide's `grSstWinClose` writes its last packets and then clears
`fbiInit7`'s FIFO bit. Applied at once, that ended 86Box's consumer loop
(it runs while `cmdfifo_enabled`) with words unrun, so
`cmdfifo_depth_rd != cmdfifo_depth_wr` for ever. `SST_status`'s busy bit
(0x380) is built from that difference, so the card read **busy to every
later poll**, the poll being Glide's own `grSstIdle`. Carmageddon's DOS
build in a Win98 DOS box spun there for minutes:

    voodoo2: 640x480 on: 122 frames (1 new), 0 triangles, 4 writes, 27298405 reads
      in 5.0 s; regs read 0x000:27298401; busy: 0 cmds outstanding (wr 49668 rd
      49668), fifo depth 59495109/59495111

Nothing busy, the depths two words apart, 27 M reads of `status`.

**The fix.** Before a write that reconfigures the command FIFO
(`fbiInit7` clearing the enable, or the ring's own pointers), the device
**runs the ring out**. Everything the guest wrote before that access has
just been counted, so waiting for the consumer puts the register behind
the packets, where the bus puts it. The wait sets `voodoo->flush`,
86Box's own escape for draining from the guest's side, so a swap in the
ring completes without the retrace the display timer cannot deliver
while the vCPU holds the BQL. It is bounded at 250 ms
(`VOODOO2_DRAIN_MS`) and says so once if it runs out. A consumer parked
inside an unfinished packet leaves the depths equal and returns at once.
Behind it, unconditionally, a write that leaves the FIFO off with words
counted and not run equalises the depths. Nothing will run them, and a
card busy for ever is worse than a lost packet. This fires only on the
off transition, so a `cmdFifoDepth` write made with the FIFO already off
(which sets a difference on purpose) is untouched.

The `voodoo-guest` teardown phase puts a cyan fill and a swap in the ring
and turns the FIFO off at once. The frame must be cyan and the status
idle afterwards. The phase announces the scene half a second after the
idle poll ends, because the swapped frame is scanned out over the *next*
frame period, and a screendump taken sooner shows the previous scene.

### The swap is counted twice, on purpose

`SST_status`'s busy bit ors in `cmd_written + cmd_written_fifo -
cmd_read`. On a Voodoo 2 a swap arriving as a **packet** increments
`cmd_read` but not `cmd_written_fifo` (only Banshee and later count
one, `vid_voodoo_fifo.c`). Glide balances it by also writing
`swapbufferCMD` to the register window. With the FIFO on, that write
only counts, and it is where status bits 31:28's swap backlog comes from.
Carmageddon's stream shows one of each per frame. A program that sends
the packet alone drives the difference negative and the card reads busy,
so the guest test writes both, as Glide does. Correcting either counter
alone leaves BUSY stuck.

### The memory FIFO and the ring interleave (patch 72)

Upstream's FIFO thread drained the memory FIFO once per wake and then
stayed in the ring as long as the guest fed it. In a race that was the
whole frame, with the consumer ~20,000 words behind (`fifo depth
66330585/66349779, voodoo_busy`). Carmageddon writes its HUD with
`grLfbWriteRegion` (~23,000 dwords a frame) before the frame's packets.
The HUD landed after the swap that followed it and went into the new
back buffer, so the **HUD sprites flickered or vanished**, leaving the
flat panels drawn as geometry. Making the ring yield whenever the memory
FIFO had something broke the other direction. An LFB write jumped ahead
of ring words published before it, and the frame's own world painted
over the HUD.

**Neither queue goes first.** Patch 72 tags each memory-FIFO entry with
the ring's write pointer at queueing (`cmdfifo_mark`). The entry runs
once the ring has been consumed that far, and the ring loop yields the
moment the memory FIFO's head is due. That is the guest's own order,
with no wait anywhere, and the flicker stopped. A mark the ring can
never reach (a `cmdFifoDepth` write makes one, by resetting the read
pointer under queued entries) is *stale* and taken as due. Otherwise the
entry waits for a pointer that is not coming and the next guest write
blocks in `voodoo_queue_command`. Draining the memory FIFO from the
guest's side instead is wrong. It means holding `flush` while a swap
goes past, and `flush` flips the buffer where the beam stands (the
teardown frame tore 64 rows down).

The 5 s line's `N publishes behind the LFB queue` counts ring publishes
made with the memory FIFO not yet empty, the case patch 72 sequences.
It cannot reach a consumer already parked inside a packet the guest has
not finished (`partial` in the line, 0 in every run so far).
`lfb-order=on` adds the mirror, a wait for the ring before an LFB or
texture write. The ordering phase measures that window away on an
unloaded host, so it stays an A/B. The phase itself (192 full-screen
fills in the ring, a 38,400-dword LFB block, then the swap; the block on
top and at least one publish behind) states correctness rather than
discriminating. The window needs a rasterizer that is really behind,
which is a game under TCG, read through the 5 s line.

### A card cannot be busy with nothing to do

No event ever resynchronises the `written - cmd_read` difference, so
when the two swap counts fail to cancel for any reason it stands for
ever. Carmageddon froze on its first race frame with `wr 3243 rd 3242`,
the ring caught up, nothing rendering, and 26 M status reads in 5 s.
Another race carried one stale command for its whole length, the guest
reading `status` 9–11 M times per 5 s.

So the device puts the difference back, with a one-time message, when
the guest polls status and every real sign of work is clear **for 20 ms
of continuous polling** (`VOODOO2_IDLE_MS`, reset the moment any sign is
not clear). The signs are both FIFOs empty, the ring caught up, no
render thread busy, no swap pending and the consumer not in its loop.
The hold is what makes it safe, because between dequeueing a command
and counting it the consumer briefly looks exactly like this. The gate
is the idle state itself, not polls since the last guest write:
`cmd_written_fifo` rises on every triangle packet (300,000 in 5 s in a
race), so that rule never fired.

### LFB writes by buffer

`LFB writes: F to the front buffer, B to the back, E elsewhere, rows
lo..hi` in the 5 s line says which buffer each write aimed at
(`fb_write_offset` against `params.front_offset` / `back_offset` at the
write) and the rows touched, which is what a flickering overlay needs.
A few writes land on the wrong side of a swap in progress, so a small
`front` count beside a large `back` one is noise.

### The ring through MMIO counts what is contiguous

**Glide writes a two-word packet value first, header second.** Every
single-register packet does (`color1`, `fastfillCMD`, `swapbufferCMD`:
header at `n`, value at `n+4`, written `n+4` then `n`). The chip is built
for that. `cmdFifoAMin` / `AMax` / `Holes` count what has been written
*contiguously*, and the depth advances over that alone. 86Box's
`voodoo_writel` counted `cmdfifo_depth_wr++` on every write to the
window, so a caught-up consumer woken by the value read the header slot
as it was, a stale word from the last lap. A stale type-1 header ("256
values follow", `num` in bits 31:16) ate the next 256 words and parked.
The next word with `2` in its low bits was a Banshee packet on a Voodoo
2 (`fatal()`: `Banshee 2D register 00000020=02020202`). The read pointer
stopped where the guest could never make room, and **Carmageddon's race
start froze on `ramfifo=off`** (27 M `cmdFifoRdPtr` reads per 5 s). The
RAM ring never had the bug, because its walk stops on a poison header
and counts the pair when both are there.

The device does the chip's counting on this path
(`voodoo2_mmio_ring_write`; `mmio-holes=off` is 86Box's count). The word
is stored where 86Box put it and counted for the consumer only once every
word before it has arrived. A word ahead of the next expected one is
held in a 64-bit bitmap until the gap closes. The write side **follows
the ring as the consumer will** (`voodoo2_mmio_step`, header by header
with `voodoo2_packet_words`), so a JMP moves the expected address to its
target. Glide writes `cmdFifoAMin` / `AMax` only at `grSstWinOpen`,
never at a wrap, so the chip must recognise its own JMP on the write
side. Otherwise the base's value-first pair after every wrap reads as a
jump back, and the next lap is held as ahead of a hole. The count starts
where `cmdFifoRdPtr` says, and the ring's registers (`cmdFifoBaseAddr`,
`RdPtr`, `Depth`, `fbiInit7`) start it over. A write that still lands
where nothing expected it (a hole wider than the bitmap, a ring taken up
with no register written) is counted as it comes, with a warning naming
the first. The 5 s line says `N ring words written ahead of a hole, M
counted with one open, K jumps followed`.

The `voodoo-guest` **swapped-pair phase** checks it, and
`voodoo-guest-mmiofifo` is the variant that discriminates. A first lap
plants a type-1 header claiming 256 values in the ring's second slot.
The ring is set to its base, a NOP brings the consumer to rest on that
slot, a `color1` value goes into the third slot, `cmdFifoDepth` is
polled to zero, and only then the header goes in, followed by a fill and
a swap. The frame must be yellow. The **wrap phase** plants the stale
header at the base as a real packet, JMPs back to the base, and writes
the pair value first, a fill and a swap. The frame must be green.

### The 8 MB board is the default

With the ring exact, Carmageddon still crashed 20–50 s into every race.
It was a DOS/4GW page fault in the game's own `strnlen`, inside `%s` of
BRender's `FATAL ERROR: %s`, printing a Glide error string from 3dfx's
overlay. The one error of its nine that depends on the card is
`grTexDownloadMipMapLevelPartial: mipmap level cannot span 2 Mbyte
boundary`. The game sizes its texture space from `grTexMinAddress` /
`grTexMaxAddress`, and the device then reported **4 MB per TMU** (the 12
MB board). A 1997 allocator written for 2 MB TMUs walked past the 2 MB
line, and Glide refused the straddling level. An 8 MB Voodoo 2 (86Box's
default too) has 2 MB TMUs and cannot reach that error. **`texmem=2` is
the default.** `texmem=4` is the 12 MB board for a title that wants it.
