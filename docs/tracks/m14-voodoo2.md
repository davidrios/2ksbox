# Track: M14 — the Voodoo 2 device

The emulated 3dfx Voodoo 2: 86Box's emulation as a QEMU PCI device,
`-device voodoo2`, beside (never instead of) qemu-3dfx's Glide and
OpenGL pass-through. Doc 21 is the design and holds everything learned
— why a chip, the shim, threads and timers, the display path, the
command FIFO in RAM, the login helper, the stranded client, the dither,
the ring's ordering. This file is the track's working state: scope, test
loop, traps, and what is open. ADR-016 is the decision;
`patches/openglide/README.md` §"Emulating the chip instead" the argument
it was opened on. The track runs on `main`.

## Scope and files

- `voodoo/` — `86box/` (vendored **verbatim**, with `UPSTREAM`), `shim/`
  (86Box's platform headers reimplemented), `voodoo_shim.c`, `voodoo2.c`
  (the device), `undither.c`, `meson.build`.
- Patches: 62 (the meson subdir), 64 (dither subtraction in both 86Box
  code generators), 71 (packet 3's packed colour word), 72 (the FIFO
  thread's order) — all on the overlaid `hw/voodoo/86box/` copy.
- `scripts/prepare-qemu.sh` (the overlay), `scripts/sync-86box-voodoo.sh`.
- `tools/voodoo-guest-test.py` and its four checks in `scripts/test.sh`.
- `guest-tools/src/v2start.c` (`SETUP /I 6`, the login-helper guard) and
  SETUP's "leave a 3dfx card's Glide alone" rule (doc 21 §10).
- The machine form's "Emulated 3dfx Voodoo 2" checkbox (`voodoo2` in the
  bundle, `-device voodoo2,addr=0x05`) — shared with M6, edited
  minimally.

## State

- **3dfx's own Win98 driver and Glide 2 and 3 run on it.** By hand on
  `base98-br`: Quake II (MiniGL), Unreal Tournament (Glide 2), NFS
  Porsche Unleashed, FIFA 2000 (matches start to finish), Carmageddon's
  3dfx build (races, since `texmem=2`), 3DMark 99 on the Windows PC;
  DxDiag's DirectDraw test passes full screen. A second game after one
  has quit sometimes starts glitched (below).
- **Performance**: the command FIFO in guest RAM (`ramfifo=on`, the
  default) took Quake II's `timedemo demo1` 41.1 → 147.5 fps and UT's
  CityIntro flyby 33.6 → 40.7 (doc 21 §9). What still traps is mostly
  status polls while the guest waits for the chip, so the BQL-free MMIO
  region is not worth doing now.
- **The Glide hang at `grSstWinOpen` is 3dfx's login helper**
  initialising the card from another process (doc 21 §11); the guest
  tools' `V2START.EXE` guard waits it out with a notice, and the device
  warns when it happens. A client left streaming into a FIFO switched
  off under it is refused rather than walked through the register file
  (`fifo-off-regs=on` is the A/B).
- **The ring is exact on both transports**: the RAM walk never passes
  what the guest wrote, and the MMIO path counts only contiguous words
  and follows the guest's JMP (doc 21 §13; `mmio-holes` is the switch).
- The 8 MB board (`texmem=2`) is the default: the 12 MB board's 4 MB TMUs
  let a 1997 allocator straddle a 2 MB boundary, which Glide refuses.
- `undither=on` reconstructs the dither away at scanout (doc 21 §12).

## Test loop

```sh
scripts/build.sh                      # or, after a voodoo/ edit:
scripts/prepare-qemu.sh && ninja -C build/qemu qemu-system-i386 libqemu-embed-i386.so
python3 tools/voodoo-guest-test.py    # ~15 s; build/voodoo-guest/
VGA=d3dpt python3 tools/voodoo-guest-test.py    # beside our adapter
RAMFIFO=off python3 tools/voodoo-guest-test.py  # the ring through MMIO
UNDITHER=on python3 tools/voodoo-guest-test.py
RECOMP=off python3 tools/voodoo-guest-test.py   # 86Box's interpreter
scripts/test.sh all                   # voodoo-guest{,-d3dpt,-mmiofifo,-undither}
```

The phases and their controls (`DITHER_SUB=off`, `FIFO_OFF_REGS=on`,
`MMIO_HOLES`, `LFB_ORDER`) are in `docs/testing.md`. A game on the
card, headless: `tools/win98-game-test.sh <image> <name>` with
`EXTRA='-device voodoo2,addr=0x05'` — the launcher's slot; any other is
new hardware and a restart prompt before the shell. The harness waits
for `WINDOWS\V2START.LOG` when the image has the guard (`VOODOO_WAIT` the
cap), a fixed wait otherwise. The benchmark is Quake II's `timedemo
demo1` with `gl_swapinterval 0` on a raw copy of `base98-br` (the game's
own counter; doc 21 §9 has the numbers and the `(N new)` column's
limits).

What the device says: `voodoo2:` lines on QEMU's stderr — the
configuration at start, `display on/off`, and every 5 s the frames (with
an `(N new)` column), triangles, register and texture traffic, and three
histograms (registers read, written, config dwords read) that name what a
spinning guest polls. `VOODOO2_TRACE=1` logs every register and
FIFO-window access, spins collapsed, and names the guest module behind
each `sst1InitRegisters`. The first refused write dumps the FIFO state
and the last 64 accesses.

## Traps

- **A hung QEMU cannot be attached to here** (ptrace scope). Run the same
  command line under gdb and interrupt it:
  `gdb -q -batch -ex run -ex 'thread apply all bt 14' --args
  build/qemu/qemu-system-i386 …` in the background, then
  `kill -INT $(ps -C qemu-system-i386 -o pid=)`.
- **A stray `rundll32` in the guest is 3dfx's login helper**, and a Glide
  program started under it wedges (doc 21 §11). On an image without the
  guard, wait out the login before any Glide run.
- **Get the Glide source** before reading register traces:
  `https://codeload.github.com/sezero/glide/tar.gz/refs/heads/master`,
  `glide2x/cvg` (`glide/src/gsst.c` is `grSstWinOpen`, `init/` the
  `sst1Init*` layer).
- **Long runs of `status` reads are not timeouts**: Glide's fixed
  200 000-iteration clock-settle loops (`init/video.c`) always run to the
  end, healthy runs included.
- `FX_GLIDE_NO_SPLASH=1` skips 3dfx's splash, which is what turns a
  torn-down card from broken into hung; `FX_GLIDE_REQUIREOEMDLL` wants
  `GR_SKIP_OEMDLL` (0xee1feef) in **decimal**, 249691887 — a wrong value
  silently loads `FXOEM2X.DLL`.
- A killed Win98 guest never flushes its FAT, so an empty
  `GLIDETEST.LOG` is no evidence; read COM1 or the screendump.
- **NFS Porsche on a Voodoo 2 is Direct3D unless told otherwise**, and its
  green tyre smoke is the game's at 16 bpp, not the chip's — don't debug
  it again (doc 21 §10).
- Porsche's and other menus cannot be clicked headless while the card has
  the monitor: `relclick` steers by `d3dpt-vga`'s cursor registers, which
  do not move then.
- The 86Box command counters (`cmd_written` against `cmd_read`) only
  balance because Glide rings the swap doorbell *and* posts the packet;
  correcting either alone leaves BUSY stuck (doc 21 §13).
- One TCG guest at a time; end scripted Win98 runs with the ACPI power
  button (CLAUDE.md).

## Open, in order

1. **Left-over state between Glide windows**: a second game after one has
   quit sometimes starts glitched (the user, by hand; not re-checked
   since the ring and teardown fixes of 2026-09-17..21). Trace it with
   `VOODOO2_TRACE=1`: what the last window left in `videoDimensions`,
   `fbiInit*` and the ring.
2. **Why a client resumes on a dead ring** (FIFA 2000's close, doc 21
   §11): it streamed on the old session's ring after someone else's
   `sst1InitRegisters` with none of a healthy reopen's writes. The device
   now names both modules and their `cr3` on its own; the next such log
   says whether it is one program or two.
3. **DxDiag's Direct3D 7 test fails at step 46, `GetDC`**
   (`DDERR_CANTCREATEDC`) on the card. Open whether a real Voodoo 2 with
   this driver fails it too (the card has no GDI) or the device lacks
   something.
4. **The M1 Air**: the ARM64 recompiler under `MAP_JIT` (86Box's own
   code), patch 64's ARM64 half, the `voodoo-guest` check there, then the
   game numbers.
5. **Diablo II** — the title the route was chosen for — measured, and
   Tirtanium (one of the titles that stranded the screen) run.
6. **Upstream**: offer patches 64 and 71 and the two command-counter
   asymmetries to 86Box; a sync that brings them in drops the patch.
7. A Voodoo Graphics (`VOODOO_1`) type only if a title wants one; SLI
   never.

## Rules

- The vendored files stay verbatim. A change 86Box needs goes upstream,
  into the shim, or — when it must be in 86Box's code — into a QEMU queue
  patch on the overlaid copy, dropped once upstream has it;
  `scripts/sync-86box-voodoo.sh` must keep working.
- Every claim about a game or a driver comes from a run with its log and
  screendump in `build/`.
- The pass-through (qemu-3dfx's Glide wrapper and OpenGL) is not retired
  by this device and is not this track's to touch.
