# Track: M14 — the Voodoo 2 device (doc 21)

The handoff for a session working on the emulated 3dfx Voodoo 2: 86Box's
rasterizer as a QEMU PCI device, `-device voodoo2`. Read
`docs/00-status.md` first for the global picture and the track rules,
then this file, then doc 21 (the design: why a chip beside the
pass-through, the shim, threads, timers, the display path, what to
measure), then `patches/openglide/README.md` §"Emulating the chip
instead" for the argument the track was opened on.

Opened 2026-09-12 on the user's decision and **merged to `main` the same
day** (branch `track/m14-voodoo2`, worktree `.claude/worktrees/m14-voodoo2`,
both deleted after the merge); the track continues on `main`, as M12
did. **A second session is working
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

**Built, instantiated, driven by a guest with no 3dfx code, and — since
the evening of 2026-09-12 — by 3dfx's own Win98 driver and Glide 2.x**
(the user's `win98-2` machine: `d3dpt-vga` + the card, the reference
driver installed, the guest tools' `GLIDE2X.DLL` removed). The first run
of the real driver found three things, all fixed the same day and all in
the device or the shim, none in 86Box:

- **PCI configuration 0x54, `siProcess`.** Glide's
  `sst1InitMeasureSiProcess` loads a PCI-clock countdown into bits 27:16,
  sets RUN (bit 28) and polls until the field reads zero. QEMU's
  `pci_init_wmask` makes every byte past the 64-byte header writable, so
  the loaded count read back for ever: **every Glide program froze in
  `grSstWinOpen`** (GLIDETEST, Diablo II's video test) at 30 M config
  reads a second — the 5 s line's new histograms named it. 86Box answers
  0 to the whole register. `voodoo2_config_read` now ends the countdown
  as soon as RUN is set and reports a count of 8000; the `voodoo-guest`
  check runs the same sequence, bounded (doc 21 §7).
- **The shim's `fatal()` returns** and is not `noreturn`: 86Box's
  `fatal()` on an `intrCtrl` write or a bad command-FIFO packet ended the
  emulator; here the write is refused, counted in the 5 s line, and the
  first one is printed with the FIFO state and the last 64 accesses.
  Declared `noreturn` it produced a SIGSEGV in the caller's next `case`.
- **`fbiInit1` bit 23 (SLI) is not writable** on the single card: 86Box's
  display timer dereferences `set->voodoos[1]` when it is set.
- **The frame buffer and texture memories carry 64 MB of zero pages
  after them** (the shim's `calloc`/`free`): the display timer read 6.8 MB
  into a 4 MB frame buffer off garbage `videoDimensions`/`fbiInit1`.

The last two are reached at **Glide's window teardown**: after
GLIDETEST's first `grSstWinOpen` draws its three cases (the 5 s line
shows `640x480 on: 2 frames, 583 triangles` — the clear, the triangle,
the reclear all render), Glide re-inits for another window: it calls
`sst1InitRegisters`, which resets `fbiInit7` to its default (`0x08080000`,
**FIFO off**) and zeroes `videoDimensions`, then **keeps streaming
command-FIFO packets to the `0x200000` window without re-enabling the
FIFO**. With the FIFO off that window is the legacy register map, so
86Box (and a real chip) decode every packet dword as the register at bits
9:2 — traced 2026-09-12 (`VOODOO2_TRACE=1`), the first is
`0x2002c8=0001fa34` → register `0x2c8`. The corruption cascades: a dword
whose bit 8 happens to be set lands on `fbiInit7` and spuriously turns the
FIFO back **on** (seen: `0x20064c=c71dcf5f`), `videoDimensions` is never
restored (written once at first open, zeroed here, never again — so
`v_disp` stays 0 and the display timer's retrace generation breaks),
garbage `triangleCMD`s queue geometry that keeps the render pipeline
"busy", and `intrCtrl` gets hit (the refusal). Glide then spins in
`sst1InitIdle` reading `status` at ~25 M/s waiting for an idle/vsync the
garbage state never produces. It is the **close/re-init that streams to a
reset FIFO**, not the reopen draw, so `GLIDETEST -noreopen` does not avoid
it and the program never exits (its `C:\GLIDE.LOG` redirect never flushes).
The device names this exactly now: `warning: voodoo2: command-FIFO packet
… to the window … with the FIFO off -> decoded as register …`.
**This is the track's next bug; the install and the first open+draw work.**
**It is not only a teardown, and not only Diablo II (2026-09-16):** on
`base98-br`, with the desktop up (3dfx's driver has switched the card on
and off once at boot), *any* Glide program wedges at `grSstWinOpen` —
`TESTS\GLIDETEST.EXE -noreopen` and the new `TESTS\DITHTEST.EXE` both
print their first line, hang, and leave the card at a scribbled
`3028x1044` with the guest spinning on `cmdFifoRdPtr` (millions of reads a
second, no writes) after the `packet ... to the window with the FIFO off`
warning and tens of thousands of refused `intrCtrl` writes. Not the RAM
command FIFO: `ramfifo=off` hangs the same way (garbage dimensions, the
spin on the status register instead). **And it is intermittent, games included** (the user, 2026-09-16, testing
by hand on this machine: sometimes a game hangs, sometimes the same game
does not) — so it is neither a tools-only path nor a fixed sequence, and an
earlier note here saying games were unaffected was wrong.
**The judgement call is made (2026-09-20): the writes are refused**, at
offset ≥ `0x100`, keeping the alternate-mapped `< 0x100` ones. The
argument: nothing writes that window on purpose with the FIFO off — Glide
only writes there when it believes the FIFO is on — so every such dword is
a stranded client's packet; a real chip is not left unusable by them,
3dfx's own Glide does this routinely, so the permanence is the
emulation's; and the walk zeroes `videoDimensions`, which the reopen never
rewrites, so the display timer stops generating retraces and `status`
never reads idle again. `-device voodoo2,fifo-off-regs=on` is the A/B, the
walk exactly as 86Box decodes it (86Box upstream would abort at the same
`intrCtrl` write). What forced it: **FIFA 2000 on `base98-br` took the
guest's Windows down at its close** (the user, 2026-09-20,
`/tmp/launcher.log`) — 800×600 on the card for 2½ minutes at 30 fps, an
ordinary `grSstWinClose`, then another `sst1InitRegisters` and a client
still streaming on the *dead* session's ring (`base 002e5000 … rp
002eee80`), 640 943 status reads and 919 878 front-buffer LFB writes in
five seconds, then a guest reset with no bugcheck. The device now names
the writing module and the last init's without `VOODOO2_TRACE=1` (the
`cr3` in each says whether it is one program re-initialising under itself
or two clients), and the `voodoo-guest` check has a stranded-client phase:
a burst into the window at the offsets of `cmdFifoBaseAddr`,
`videoDimensions` and `fbiInit7` must leave the ring's register unmoved,
with `FIFO_OFF_REGS=on` the control that must move it (`03010300 ->
02AD02EF`). **A second FIFA failure, and a second fix the same day
(2026-09-20): the RAM walk must never pass the guest's write pointer.**
With the refusal above in, the game reached a match and then froze on the
*loading* screen — a different bug and an older one. The walk reconstructs
the guest's write pointer from poison, and a last resort took the rest of a
part-written packet as data after 64 idle `cmdFifoRdPtr` polls. Glide's
free space is `rp - wp - 1`, so that puts the pointer past `wp` and the
guest waits for room on an empty ring: a 66-word type-5 LFB packet with 19
words written, 47 taken (`47 = 66 - 19`, and the 5 s line said `1 packets
part-written, 47 words taken as data`), and a guest with 46 words free
asking for 66 — 22.7 M `cmdFifoRdPtr` reads a second, `depth 24623/24623`,
card idle, nothing written, for ever. **`ramfifo=off` runs the game through
several matches start to finish** (the user), which is what proved the
guest was never at fault. The guess is gone; the case it was written for
was closed when the poison word stopped being `0xffffffff` (2026-09-17).
The `voodoo-guest` check's partial-packet phase holds it, and the restored
guess is the control that fails it (`00300008` where `00300004` is right).

**What is still open**: why the reopening client does not
re-enable the FIFO. A healthy reopen is unmistakable — `initEnable <=
00005001`, the whole `0x1e0`–`0x1f8` block, `fbiInit7` with bit 8 — and
`VOODOO2_TRACE=1` on GLIDETEST shows it every time (its close/reopen case
passes 4/0), while FIFA's had none of it. The next FIFA run's log will
name the two modules itself.

**Real games on the card, by hand — 2026-09-13** (the user, on the
launcher's `base98-br` machine, Win98 with 3dfx's driver): **Quake II,
Unreal Tournament and Need for Speed: Porsche Unleashed all run on the
emulated Voodoo 2, and all three quit cleanly.** UT is a **Glide 2**
title — its `GlideDrv.dll` imports `glide2x.dll`, and its log says
`Found Glide: 2.56` — and Quake II goes through 3dfx's MiniGL (Glide 2
too). **Porsche is not Glide on this card**: its own
`3DSetup\3dsetup.ini` sends a Voodoo 2 to `/M:dx` (`dx7z.dll`, Direct3D
through 3dfx's HAL for the chip) and only a Voodoo 1 or Rush to
`voodoo2z.dll` — the headless run (2026-09-15) had the card take the
monitor and `d3dpt-vga`'s own Direct3D draw nothing. `voodoo2z.dll`
imports `glide3x.dll` (`grVertexLayout`), so Porsche *has* a Glide 3
renderer, reachable by pointing that ini line at `/M:voodoo2` and running
3D Setup again. **What 3D Setup writes is one registry value** — the user
ran it on `base98-br` on 2026-09-15 and picked 3dfx, and
`HKLM\SOFTWARE\Electronic Arts\Need For Speed - Porsche 2000` came out
`3D Card = 3Dfx Voodoo2`, `Group = 3Dfx`, **`Thrash Driver = dx`**: the
ini's mapping, so "3dfx" still meant Direct3D. Setting that value to
`voodoo2` (a `.reg` through `regedit /s`) is the whole switch, and it is
**verified 2026-09-16** on a copy of that image with `DRIVERS\dx7z.dll`
renamed away, so Direct3D could not load: the game came up and drew its
menu at **~50 fps against the Direct3D path's 30**, `build/w98game/nfsg`.
**The green tire smoke is the game's, not ours** (2026-09-16): the user saw
it on the chip under Glide *and* under the Glide pass-through, it did not
move with `recompiler=off` (so not the rasterizer's dither subtraction,
which the recompilers skipped and the interpreter did — patch 64 since),
and **in Direct3D at 32-bit it is not
green** (the user, same day). Period reports say the same: a VOGONS thread
on this game has DX7-era cards showing "a slight green tint" on the smoke
with 32-bit colour as the cure, and PCGamingWiki carries that fix. A
Voodoo 2 has no 32-bit mode, so on the chip the smoke is green exactly as
it was on the real card. Don't debug it again.

**What the smoke hunt did turn up: neither 86Box recompiler subtracted the
dither on a blend read-back** (doc 21 §9), while its interpreter did and
real hardware does — **fixed by patch 64 the same day** (job B below). Measured 2026-09-16 by the `voodoo-guest` check's new
dither phase — a grey blended onto itself 96 times per column at falling
alpha, through the chip's own setup unit and the command FIFO, no Glide
involved. `recompiler=off`: every column exactly the reference (7c0f, green
sd 1.32). Default: 7bef, 7bcf, 73ce, 73ae, 738e, 6b6d, 6b4d — a grainy
band, green sd to 7.19, pixels 24 levels under the reference.
`RECOMP=off tools/voodoo-guest-test.py` is the A/B; the frames are
`build/voodoo-guest[-interp]/dither.ppm`. Not known to matter to any title
(Porsche's smoke is green with and without it); fixed anyway, in both
code generators, as a patch on the overlay (job B).
With Diablo II that makes two Glide 3 titles in hand (Diablo II is capped
at 25 fps). Quake II
and UT felt fine; **Porsche felt slow**. **Starting another game after
one has quit sometimes comes up with glitched graphics** — the same
class of state as the teardown above (a register or the monitor left
over from the last window), not yet looked at. **GLIDETEST does not hang
any more** either (the user's run, same day): the close above no longer
wedges the card. Hand tests: no numbers and no logs from them.

**The monitor handed back to a desktop on our driver — fixed the same
evening.** On the user's `test98` (d3dpt-vga + the card, 3dfx's driver,
still on the desktop) every full-screen switch left a silver, glitched
screen that never came back. The log said why: `voodoo2: display on` then
`display off (VGA back)`, and no `[display] switch 800x600` from the
player after it — `d3dpt-vga`'s invalidate only asked for a full repaint,
and its linear path re-installs its own surface only on a mode change, so
it kept updating the surface the Voodoo had left on console 0 (the
Voodoo's last frame). The invalidate now re-surfaces (`resurface` in
`d3dpt/hw/d3dpt_vga.c`), and `VGA=d3dpt tools/voodoo-guest-test.py` (the
`voodoo-guest-d3dpt` check) first puts the adapter in an 800×600×32
linear mode, so the hand-back must land on that mode.

**DxDiag on the card, 2026-09-13** (DirectX 9.0c's, on a raw copy of
`test98` with the fix: `RAW=… NO_DRIVER=1 EXTRA='-device voodoo2,addr=0x05'
GUEST_CMD='C:\WINDOWS\SYSTEM\DXDIAG.EXE' CLICKS='80:298,68 100:550,307'
KEYS=… tools/win98-game-test.sh`, outputs in `build/w98game/ddtest` and
`d3dtest`). The two programs the user had seen strand the screen were
DxDiag and a title called Tirtanium. What the runs showed:

- **3dfx's driver switches the card on and off at every boot**, before
  any program runs: `display on` / `display off` about 4 s after the
  desktop's mode is set, with a 5 s line of `2 frames, ~1100 triangles`
  and `status` polled ~2 M times. That boot-time hand-back is what left
  the unfixed build on the Voodoo's frame from the first second of the
  desktop, which is why "everything" full-screen looked broken.
- **DxDiag lists the card as display 2**, "Voodoo2 DirectX 7 Driver",
  `3dfxV2.drv` 4.11.0001.1151, DDI 7, 12 MB, DirectDraw and Direct3D
  acceleration on.
- **Its DirectDraw test passes on the chip**: full screen at 640×480,
  the Voodoo takes the monitor (`640x480 on: 300 frames … in 5.0 s`,
  60 fps, the white bouncing box by screendump), gives it back, and DxDiag
  reports "Todos os testes tiveram êxito". The desktop is back every time.
- **Its Direct3D 7 test fails at step 46, `GetDC`, `0x88760249`
  (DDERR_CANTCREATEDC)**: two short on/off pairs and 4 frames, then the
  error. Open: whether a real Voodoo 2 with this driver fails the same
  step (the card has no GDI, so a refused DC may be the driver's normal
  answer) or something the device does not model. The Direct3D 8 test
  runs on `d3dpt-vga` instead (DirectX 8 has no device for a DDI 7
  driver) and the Direct3D 9 one is skipped for the same reason.
- No hang in either test; the machine powers off on the ACPI button.

Tirtanium was not on the image, the shelf or the host; it is still to
run.

Diagnostics that came out of the day, all in `voodoo2.c`: the 5 s line's
three histograms (registers read, written, config dwords read — a
spinning guest names its register), `VOODOO2_TRACE=1` (every register-
and FIFO-window access, status polls collapsed), and the refusal dump.

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
VGA=d3dpt python3 tools/voodoo-guest-test.py   # beside our adapter in a linear mode
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

To put the card on a machine: the machine form's "Emulated 3dfx
Voodoo 2" checkbox (`voodoo2 = true` in the bundle, `-device
voodoo2,addr=0x05`; both front ends, the C API, `launcherx --wizard-edit
… voodoo`; the `voodoo2` check), or `tools/win98-game-test.sh`'s
`EXTRA='-device voodoo2'`.

## Two jobs, written up for a new session (2026-09-16)

### A. The Glide hang at `grSstWinOpen` — **found, 2026-09-16: 3dfx's login helper**

**Answer first** (doc 21 §11 is the write-up): 3dfx's driver runs
`rundll32.exe 3dfxv2ps.dll,UpdateRegSettings` from a Run entry named
`Voodoo2` at every login, and that initialises the card through
`GLIDE3X.DLL` — in another process, with its own copy of the init
library's state. Under TCG it takes about three seconds. A Glide program
started at the desktop (every headless harness run, and a game started by
hand soon enough) has its window torn down under it: video registers
zeroed, command FIFO switched off, and the program then streams packets
into the FIFO window, which 86Box decodes as registers — the wedge below.
**GLIDETEST started 20 s after the desktop passes all four cases, the
close/reopen one included** (`build/w98game/glok`, `glre`); with no wait it
wedges every time (`glcol`).

What changed:

  * the device warns `the card is being re-initialised (sst1InitRegisters)
    while a Glide window has the command FIFO on` at the config write that
    starts such an init (`initEnable = 1` with the FIFO on — Glide's own
    close turns the FIFO off first, and its own reopen does not trip it);
  * `tools/win98-game-test.sh` puts `choice /c:y /t:y,$VOODOO_WAIT` (20 s)
    before the batch when `EXTRA` has a `voodoo2`, and its summary names the
    collision if the wait was not enough — **since the same day it watches
    for the guest tools' guard instead** (below);
  * **the guard** (user request, same day): `SETUP /I 6` puts
    `V2START.EXE` in the helper's place in the Run key; it runs the helper,
    shows "Voodoo 2 driver is loading, please wait before running 3dfx
    games" until it exits and writes `WINDOWS\V2START.LOG` (doc 21 §11). On
    a raw copy of `base98-br` the helper took 11.3 s and 18.7 s, and
    GLIDETEST started the moment the log appeared passed both times;
    `VOODOO=1 tools/setup-guest-test.sh <image> win98` checks the registry
    move in a guest;
  * `VOODOO2_TRACE=1` names the guest module behind each init — the PE
    image around the program counter, walked back to its `MZ` header, with
    the image at `0x00400000` naming the process. Reads are RAM only (a
    first cut read through the card's own BAR mapping, which QEMU blocks as
    re-entrant I/O). That is what found it: the first init from `GLIDE2X.DLL`
    at `0x10000000` under GLIDETEST's `0x42000` image, the second from
    `GLIDE3X.DLL` at `0x01690000` under a `0x6000` one, RUNDLL32.EXE, which
    the registry's Run entry named.

**The hand-run hangs are this too** (the user, 2026-09-16): every time a
game froze by hand there was a stray `rundll32` running that they could
not account for — the helper. It is not only a login-time thing, then, or
not only seconds long when it overlaps a game; either way the device's
warning names it now. `busy: 1 cmds outstanding` with no such warning
would be the 86Box command-count pairing below, a different bug.

The investigation as it went, kept because most of it is still true and
some of it is a trap:

#### What the 2026-09-16 session established

**It is `grSstWinOpen`, not the close.** The screendump of a hung run shows
*both* of the program's first lines — `Glide 2.56.00.0459, resolution 7 =
640x480` **and** `1 board(s) reported` — so `grSstQueryHardware` returned
and the program is inside `grSstWinOpen`. An earlier note here saying only
the first line appears was wrong. `GLIDETEST.LOG` being empty is not
evidence either: the guest is killed, so the FAT is never flushed.

**`FX_GLIDE_NO_SPLASH=1` avoids the hang.** Put `set FX_GLIDE_NO_SPLASH=1`
in the `GUEST_CMD` batch before the program and GLIDETEST runs to
completion every time — `3 cases, 3 failed`, the failures being pixel
readbacks, and a clean exit. So the code that actually wedges is Glide's
3dfx splash, which `grSstWinOpen` runs last of all (`glide2x/cvg/glide/src/
gsst.c`, `LoadLibrary("3dfxsplash2.dll")` and then `grSplash()`).

**The splash is not the cause, though — it is what turns broken into
hung.** With the splash off the card still ends at **`1x0` with its display
off** and all three of GLIDETEST's readbacks come back black. On a real
Voodoo 2 that program shows a red screen and passes.

**What the register trace says.** `VOODOO2_TRACE=1` now collapses a spin on
any one register (not just the status poll), so a whole run is a few MB and
readable; two runs diffed against each other is how the following was read
off (`awk` the two `initEnable <= 00000001` regions into files, strip the
counts, `diff`). Glide runs `sst1InitRegisters` **twice**, and the two are
write-for-write identical for 5 367 writes:

  * the first one is followed by the video mode (`hSync`, `vSync`,
    `backPorch`, `videoDimensions` — 640x480 or 800x600, whichever the app
    asked for), the DAC gamma ramp, `sst1InitCmdFifo(FXTRUE)` (the
    `0x1e0…0x1f8` block and `fbiInit7 |= 0x100`), and a swapbufferCMD. The
    card comes up and shows its first frame;
  * the second one ends at its register zero sweep — `vRetrace`,
    `backPorch`, `videoDimensions`, `hSync`, `vSync` all to 0 and
    `fbiInit7` to `SST_FBIINIT7_DEFAULT`, i.e. the command FIFO **off**
    (`init/sst1init.c` ~527 and ~734) — and **nothing re-programs them**.
    `sst1InitCmdFifo` never runs again.

From there Glide goes on believing the FIFO it set up is live: its next
packets go to the window at `0x200000` with `fbiInit7` saying off, 86Box
decodes each dword as the register at bits 9:2 (`vid_voodoo.c`'s
`voodoo_writel`, the `(addr & 0x200000) && (fbiInit7 & CMDFIFO_ENABLE)`
branch not taken), `videoDimensions` and the `fbiInit`s are scribbled, and
the guest spins on a read pointer that will never move.

**Do not read the `x200006` status runs as timeouts.** They are Glide's
fixed 200 000-iteration "wait for the video clock to stabilize" and "wait
for the graphics clock to stabilize" delay loops (`init/video.c:585` and
`:934`) — they always run to the end, and they appear in healthy runs too.
That cost an hour.

**Get the Glide source.** It answers these questions in minutes and none of
the above is guessable from register traces alone:
`https://codeload.github.com/sezero/glide/tar.gz/refs/heads/master`,
then `glide2x/cvg` (`glide/src/gsst.c` is `grSstWinOpen`, `init/` is the
`sst1Init*` layer).

#### Two accounting bugs in 86Box found on the way — real, but not the fix

Both are worth reporting upstream; neither, on the evidence below, is the
root cause.

  1. `vid_voodoo.c`, `voodoo_writel`, `case SST_swapbufferCMD`:
     `cmd_written++` and `swap_count++` sit **above** the
     `if (fbiInit7 & FBIINIT7_CMDFIFO_ENABLE) return;`, where
     `triangleCMD`, `ftriangleCMD`, `fastfillCMD` and `nopCMD` all return
     first. So a swap doorbell written to the register map with the FIFO on
     is counted and then refused.
  2. `vid_voodoo_fifo.c`: `cmd_written_fifo++` for a `swapbufferCMD` taken
     out of the command FIFO is guarded by `type >= VOODOO_BANSHEE`, while
     `vid_voodoo_reg.c` does `cmd_read++` for every card. So a swap that
     comes through the FIFO is run but not counted.

3dfx's Glide rings the doorbell **and** posts the packet for every swap, so
the two cancel frame after frame — which is why the card works at all, and
why neither can be corrected alone. The status register reports BUSY while
`cmd_written + cmd_written_fifo + cmd_written_fifo_2` differs from
`cmd_read` (a negative difference counts as busy too: the expression is
used as a truth value), so wherever a doorbell and a packet do not pair
off, BUSY sticks for the life of the card. **That is the user's second
shape of this hang**: `busy: 1 cmds outstanding (wr 4911 rd 4910)`, 16 M
status reads a second, the command FIFO drained and both render threads
asleep.

**What was tried, and what it did** (one run each, so leads, not results):

  * dropping the register-map doorbell whole, and separately letting it
    through and doing `v->cmd_written--` after: **both made
    `grSstWinOpen` complete** — the card stayed `640x480 on`, the splash
    rendered 297 787 triangles and 74 new frames in 5 s, no `fifo-off`
    writes, no refusals — and then hung with `busy: -154 cmds outstanding
    (wr 596 rd 750)`, exactly the 154 swaps of the run, i.e. bug 2 with
    bug 1 no longer cancelling it;
  * re-pairing the two counts at the `fbiInit7` write that switches the
    command FIFO on or off (the one place a doorbell can be orphaned):
    fired, but only later, at a *scribbled* `fbiInit7` during the garbage
    phase — at the real teardown the counts were already equal. So the
    orphan-at-teardown theory does not hold for this path;
  * counting FIFO `swapbufferCMD` packets in `voodoo2_fifo_sync`'s walk
    (packet type 1, register `0x128`) together with uncounting the
    doorbell: regressed. The packet detection is probably over-counting;
    `num == 0` and the address-range test both want checking.

All of that was reverted; `voodoo/voodoo2.c` carries only the trace change.

#### The lead that was taken (and why it was not the answer)

GLIDETEST calls `grSstWinOpen(0, …)` — **hWnd 0**, so Glide falls back to
`GetActiveWindow()` (gsst.c warns about exactly this). The second
`sst1InitRegisters` arrives ~0.2 s after the first frame and reads like a
*teardown*: display off, VGA back, every video register zeroed, the FIFO
switched off. That is what 3dfx's driver does when a Glide window loses the
screen. A console program started from the Run dialog may well have no
usable active window, which would make this a property of the harness
rather than of the device — and would fit the user's games, which own a
real window and mostly work. Worth answering first, because it decides
whether the bug is ours at all:

  * run a Glide program that owns a proper window (or pass a real `hWnd`)
    and see whether the second `sst1InitRegisters` still comes;
  * trace one of the user's games (Quake II, UT) through a window open and
    look for the same second pass;
  * if it is the harness, the fix is in the harness and GLIDETEST should
    say so rather than hang.

It was neither the window nor `FXOEM2X.DLL` (skipped with
`FX_GLIDE_REQUIREOEMDLL=249691887`, i.e. `GR_SKIP_OEMDLL` 0xee1feef in
decimal — get that conversion from the shell; a wrong value silently loads
the DLL): the second init came from another process, above.

**Ruled out.** The RAM command FIFO (`ramfifo=off` hangs the same way, and
the DOS program in `tools/voodoo-guest-test.py` drives the same FIFO
through hundreds of thousands of packets without trouble); the rasterizer
recompiler (`recompiler=off` unaffected); our own test program (the stock
`TESTS\GLIDETEST.EXE -noreopen` hangs identically); the requested
resolution (`-res 8`, 800x600, hangs the same way — an early guess that
Glide skips re-programming when the mode is unchanged, which the trace
disproved).

**Guard it when fixed.** Intermittent wants repetition: a loop of N boots
of the `GLIDETEST` run above, and `VOODOO=1 tools/setup-guest-test.sh` for
the driver side. The DOS FIFO phases in `tools/voodoo-guest-test.py` stay
the deterministic floor.

### B. Dither subtraction in 86Box's two recompilers — done (patch 64)

**What.** On a blend the chip reads the pixel underneath, which was written
dithered, and subtracts that position's dither offset again when
`fbzMode`'s `DITHER_SUB` (bit 19) is set. 86Box's interpreter does it
(`vid_voodoo_render.c` ~1310, tables `dithersub_rb` / `dithersub_g` and the
2x2 pair, gated also by `voodoo->dithersub_enabled`, our `dither-sub=on`
default). Neither code generator mentioned `dithersub`, and
`recompiler=on` is the default, so in practice it never happened.

**Done 2026-09-16, `patches/qemu/64-voodoo2-dither-sub-recompilers.patch`**,
a patch on the overlaid `hw/voodoo/86box/` copy (the vendored files stay
verbatim; `prepare-qemu.sh` rsyncs the overlay before the queue applies).
x86-64: at the read-back, `rgb565[pixel]` goes into ECX and the three
lookups (`dithersub_rb[B]`, `dithersub_g[G]`, `dithersub_rb[R]`, row
`y*stride + x` with `x` from the same `state->x`/`x_tiled` the dither
write reads and `real_y` from R14) are packed back with the entry's alpha
into XMM4 — RCX, RSI and R8 are free there. ARM64: the same after `LDR w6,
[x26, w6, UXTW #2]`, in x7/x10/x11/x13/x16. 114 bytes / 28 instructions
per block that has the bit.

**How it was checked.** Before any guest: a scratch harness emitted both
sequences with the generators' own code and macros and compared them with
the C tables — the x86-64 bytes executed natively (52k cases: both dither
sizes, tiled and not, every byte through each channel), the ARM64 ones
under Unicorn (14k cases, garbage in the upper halves of the inputs and in
every scratch register). Then the guest: the `voodoo-guest` dither phase
now **requires** every `DITH COL` to equal `DITH REF` and the screendump
to be one 4x4 dither tile (0 pixels off; the old recompiler frame is
10,240 off, the interpreter's 0). `RECOMP=off` runs the interpreter.
**Still to run: the ARM64 build on the Air** (the `voodoo-guest` check
there is the proof on that generator).

**Upstream.** The same change is worth offering to 86Box; when a sync
brings it in, patch 64 stops applying and is dropped.

## Next steps, in order

1. **A guest driver — done 2026-09-12** (the user installed 3dfx's
   reference driver in `win98-2`; Device Manager binds `121a:0002`, and
   Glide finds the card once siProcess counts down, above). Which Glide a
   machine with the card gets is **settled in `SETUP.EXE`** (2026-09-13,
   doc 21 §10): with a 3dfx device present it leaves 3dfx's
   `GLIDE*.DLL`, `FXMEMMAP.VXD` and `GLIDE2X.OVL` alone, and `/GAME 6` /
   `/GAME 7` put the pass-through's next to one game; `VOODOO=1
   tools/setup-guest-test.sh` holds it (PASS on Win98 and XP; the files
   it protects were stand-ins, since no image here has 3dfx's driver).
   **The open bug is left-over state between Glide windows** (above): a
   second game after one has quit sometimes starts glitched (GLIDETEST's
   close hang is gone, 2026-09-13). Trace with `VOODOO2_TRACE=1`: what
   the teardown burst leaves in `videoDimensions`, `fbiInit*` and the
   FIFO, and whether dropping `0x200000`-window writes with the FIFO off
   cures both.
2. **The numbers** (doc 21 §9) — games run now (Quake II, UT, Porsche,
   above): Quake II's `timedemo demo1` (the software renderer as the
   control) and **NFS Porsche**, the one that felt slow, through
   `tools/win98-game-test.sh EXTRA='-device voodoo2,addr=0x05'` on a copy
   of `base98-br`: the 5 s log line's frames against the game's own
   counter (is the rasterizer keeping up, or the vCPU?), `perf` on the
   vCPU thread with the MMIO handlers named, `threads=4` as the A/B, and
   the same run on the M1 Air. Does the 3dfx driver enable the command
   FIFO? (the log will say: `fbiInit7` bit 8, and the wr/tex counters).
   Then **Diablo II** — the title the route was chosen for.
   **First numbers, this box, 2026-09-15** (raw copy of `base98-br`,
   640×480, 2 render threads; the frame rate is the 5 s line's new
   `(N new)` column, doc 21 §9, unless the game counts its own):
   - **Quake II** `timedemo demo1`, `gl_swapinterval 0`: **40.7 / 41.3 /
     42.2 fps** (game's own counter, three runs), ~3.3 M register writes
     and ~10 k status reads a second while it plays — every write a trap
     on the vCPU thread, which makes step 3's command FIFO in RAM the first
     thing to try, before anything on the rasterizer's side.
   - **UT** CityIntro flyby (no input, loops ~85 s): **33.6 fps** mean
     over 32 windows, 18–57; 0.9 M writes a second.
   - **Porsche**: its front end holds a flat **30 fps** (its own cap). No
     race number: the keyboard never reaches its menu headless, and
     `relclick` steers by `d3dpt-vga`'s cursor registers, which do not
     move while the Voodoo has the monitor — a race wants a hand or a
     tablet. It first needs its **3D Setup** run once (the user's image
     never finished it: "Run 3D Setup before running the game"), and on
     a Voodoo 2 that setup picks Direct3D, not Glide (above).
   Patches 47–49 (x87 PC=64) change neither Quake II nor UT. Next: the
   same runs on the Air, and `perf` on the vCPU thread here.
3. **The two performance steps, if the profile asks for them**: the
   command-FIFO window as RAM (a `MemoryRegion` alias into `fb_mem`, the
   doorbell on `cmdFifoDepth` / the wake timer; no trap per dword), and
   the region without the BQL (`memory_region_clear_global_locking` plus
   a device lock between the MMIO handlers and the display timer).
   **The first is done (2026-09-15, `ramfifo=on|off`, doc 21 §9)** —
   without a doorbell, since Glide keeps hole counting on: poisoned
   consumed words and a packet walk at every other access to the card.
   **Quake II 41.1 → 147.5 fps, UT's flyby 33.6 → 40.7.** The second is
   not worth it now: what still traps is mostly status polls while the
   guest waits for the chip.
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
  or into the shim — or, when it has to be in 86Box's own code, into a
  QEMU queue patch on the overlaid `hw/voodoo/86box/` copy (patch 64),
  dropped once upstream has it; `scripts/sync-86box-voodoo.sh` must keep
  working.
- Every claim about a game or a driver comes from a run with its log
  and screendump in `build/`; "the driver should find it" is not a
  state.
- One TCG guest at a time on the box; end scripted Win98 runs with the
  ACPI power button (CLAUDE.md).
