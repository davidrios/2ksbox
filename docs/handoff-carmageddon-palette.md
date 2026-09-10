# Handoff: Carmageddon on the Win98 driver — the 8 bpp palette

Written 2026-09-10 on branch `win98-wip`. This is a working handoff for the
next session, not a design doc; when the issue is closed, fold the
conclusion into doc 19 and delete this file.

## TL;DR

- **The blue screen the user hit is fixed and pushed** (`6363253`, doc 19
  §30): a GDI heap overrun at the 16→8 bpp mode switch. Not this document's
  subject; it is done.
- **Carmageddon still does not display correctly.** It is a software
  renderer that draws into a `DDSCAPS_SYSTEMMEMORY` primary at 320×200×8.
  Its frame **does reach our VRAM** (measured). The problem is the **8 bpp
  palette**: the game's own palette does not reach the device's DAC
  (`REG_PALETTE`), and separately the device only re-applies the palette to
  its scanout lookup table on a `pal_dirty` write, which mistimes against
  when the palette is actually valid.
- There is an **uncommitted device experiment** in the working tree that I
  reverted before writing this (its diff is in the appendix). It made the
  320×200 frame show *content* instead of solid black, which is the
  strongest lead — but the content looked like the halftone-palette desktop,
  not the game, so it is not a fix yet.

## What is solid (measured, trust these)

1. **The game runs.** On our driver it sets 320×200×8 and runs there (23 s
   in one run, ~1 s in another — the hold time varies between runs, itself
   worth noting). It is not crashing or failing init.

2. **The game's frame reaches our VRAM.** Reading the adapter's VRAM at
   BAR0 from the host *while the game runs* (`scratchpad/game-vram.sh`)
   showed the visible 320×200×8 region fill from all-zero to **57 775 /
   64 000 bytes non-zero, 137 distinct palette indices**. Rendered with a
   grey ramp (`PIL.Image.frombytes("L",(320,200),bytes)`) it is a real,
   structured 8 bpp frame. So the DX runtime *does* present a system-memory
   primary to our frame buffer — the earlier doc 19 §30 wording ("the
   presentation path is not wired") was wrong and has been corrected.

3. **The screen is black because the 8 bpp DAC is wrong/empty.** The device
   looks VRAM indices up through `REG_PALETTE` (`s->pal[]`,
   `fb_apply_palette` in `d3dpt/hw/d3dpt_vga.c`). A screendump of a
   black-looking run is 265 bytes (solid). The frame is there; the colours
   are not.

4. **The DX runtime does not route a system-memory primary's palette to any
   driver callback.** I published `CreatePalette` / `SetEntries` HAL
   callbacks (in `d3dpthal.c`) that write `REG_PALETTE`, wired them through
   the 16-bit `BuildCallbacks` (`d3dpt9dd.c`), and advertised
   `DDCAPS_PALETTE`. Result: **zero** palette callbacks fired and the frame
   stayed black. The 16-bit GDI `SetPalette` export (ordinal 22, which does
   write `REG_PALETTE`) does not fire in exclusive mode either. All of that
   was reverted — it changed nothing here, though it may be correct for a
   *video-memory* 8 bpp primary and is worth revisiting for those.

5. **It works on the inbox Cirrus driver.** `VGA=cirrus` reaches
   Carmageddon's 320×200 menu in colour (a 320×400 line-doubled
   screendump). So a palette-to-DAC route exists there and not on ours.

## The device has two separate palettes (this is the core confusion)

`d3dpt-vga` embeds a full QEMU `VGACommonState vga`. There are therefore
**two** palettes and they are not connected:

- `s->vga.palette[768]` — the VGA core's DAC, 6-bit RGB, programmed by the
  standard VGA DAC I/O ports 0x3C8/0x3C9, scaled with `c6_to_8()`.
- `s->pal[256]` — our own `REG_PALETTE` (MMIO 0x400, 8-bit x8r8g8b8), the
  only one `fb_apply_palette` uses for linear-mode 8 bpp scanout.

On a real card the 8 bpp palette *is* the VGA DAC; we split them. So a
palette written to one is invisible to the other.

**The experiment that produced the strongest signal** (appendix) added, to
`fb_apply_palette`: a one-line log of how many entries each palette has, a
fallback to the VGA DAC when `REG_PALETTE` is empty, and — the part that
mattered — re-applying the palette **every frame** at 8 bpp instead of only
on `pal_dirty`. With it, a run logged:

```
d3dpt-vga: palette nz REG=255 DAC=63
```

and the 320×200 screendumps jumped from 265 bytes (black) to 3555 bytes
(content). **But** `REG=255` is almost certainly the **driver's default
halftone palette**, not the game's: the 16-bit `Enable(hardware)` at 8 bpp
programs `REG_PALETTE` from `DefaultColourTable` (system colours + a 6×6×6
cube + grey ramp — `d3dpt9x.c`), which is 255 non-zero entries. The
"content" that appeared looked like the Win98 desktop rendered through that
halftone palette, not Carmageddon. So the experiment revealed a **palette
*timing/application* bug** (the scanout was using a stale, empty indexed
table even though `REG_PALETTE` had a palette), but **not** the game's real
colours.

## The two distinct problems to solve

**Problem A — palette application timing (device side, tractable).**
`fb_apply_palette` rebuilds `s->indexed` only when `s->pal_dirty` is set
(on a `REG_PALETTE` MMIO write) and only gets *called* from the update path
under `if (s->pal_dirty && m.bpp == 8)`. The default palette written by
`Enable` at mode set can land such that the indexed table the scanout uses
is stale/empty — hence solid black even though `REG_PALETTE` is populated.
The experiment's "re-apply every frame" is a blunt confirmation, not the
fix; the real fix is to make `fb_apply_palette` run whenever the palette
*or* the mode has changed, ordered after the driver's `Enable` palette
write. Verify with `DDTEST` 8 bpp and Diablo (doc 19 lists both as working
8 bpp titles — check they still are).

**Problem B — the game's own palette never reaches `REG_PALETTE` (the real
blocker).** The game sets its palette on an `IDirectDrawPalette` attached to
a system-memory primary. The runtime keeps it in software and calls no
driver palette path (proven in fact #4). So even with Problem A fixed, the
frame shows in the *default* halftone palette, not the game's. Leads, in
order of promise:

1. **Find how the palette reaches the DAC on Cirrus.** Instrument or reason
   out what programs the hardware palette when Carmageddon runs on the inbox
   driver. Candidates: the VGA DAC I/O ports (0x3C8/0x3C9) written by the
   runtime or the game; a GDI `RealizePalette` path; or a driver callback
   Cirrus implements that we do not. If it is the VGA DAC ports, the fix is
   to **unify our two palettes** — mirror VGA-DAC writes into `s->pal` (or
   have `fb_apply_palette` read the DAC) so a standard 8 bpp palette
   programming reaches linear scanout. This is the single highest-value
   experiment. (Note: the "re-apply every frame + DAC fallback" experiment
   logged `DAC=63`, i.e. the VGA DAC *was* partly programmed — 63 entries —
   which hints the DAC path is live and worth chasing.)
2. **The game's caps-driven surface branch.** The game's DirectDraw init
   (`CARM95.EXE` fn `0x4bb104`, reached from `0x4bae1b`) branches on
   `[0x53fdcc]` between two sysmem-primary setups (`0xa18` flipping vs
   `0x4ba9a6`'s `0xa00` + `0x840` work surface). `[0x53fdcc]` is set from a
   caller flag that may key on caps. We advertise `DDCAPS_3D` (0x1); Cirrus
   does not. Reverse the setter, or A/B by temporarily dropping `DDCAPS_3D`,
   to see if the game takes a different, working path on ours.

## Tooling built this session (all in the scratchpad, local only)

- **`ddprobe.exe DDPROBE <w> <h> <bpp> [sys]`** — the DirectDraw mode test
  in `guest-tools/src/d3dptvid/w9x/ddprobe.c` (committed form has `[sys]`;
  the present-path and hold variants were reverted). Replays Carmageddon's
  exact surface chain (`sys` = `DDSCAPS_SYSTEMMEMORY` primary) with none of
  the game's code, which is what localised each finding.
- **`scratchpad/game-vram.sh`** — runs the actual game and polls the
  adapter's VRAM at BAR0 while it is at 320×200×8; how fact #2 was measured.
- **`build/venv-capstone/bin/python`** with pillow — renders a VRAM dump:
  `Image.frombytes("L",(320,200),open("v.bin","rb").read()[:64000])`.
- **`scratchpad/bsod-inspect.sh`, `spin.sh`** — decode the VGA text page and
  sample/disassemble guest code at a fault (from the blue-screen work).
- `tools/win98-game-test.sh` gained UTC timestamps and `TEXT_AT=`; the game
  machine is `~/.local/share/2ksbox/machines/claude98/`, copy never write.
  The user's own player often holds its lock — use
  `RAW=build/w98game/guest.raw FRESH=0` then.

## Reproduce the current state

```sh
# clean tree at the two pushed commits (6363253 crash fix, d087d54 doc)
scripts/build.sh            # or the driver + qemu subset
# the game, black frame, palette measured in VRAM:
FRESH=1 GUEST_CMD=$'cd \\ARQUIV~1\\GAMES\\CARMAG~1\nCARM95.EXE' \
  RUN_SECS=90 SHOTS=3 KEYS="20:spc,22:ret" \
  tools/win98-game-test.sh ~/.local/share/2ksbox/machines/claude98/disk.qcow2 carma
# the password dialog takes a single space (the "20:spc,22:ret" above)
```

**Device rebuild trap** (cost time this session): editing
`d3dpt/hw/d3dpt_vga.c` and running `ninja -C build/qemu` does **not**
recompile it — the device is rsynced into `qemu/hw/d3dpt/` by
`prepare-qemu.sh`. Redo those two rsync lines by hand first (mind the
`--exclude d3dpt_proto.h --exclude d3dpt_fb.h --exclude d3dpt_exec.h` and
the separate header copy — a plain `rsync --delete` deletes the shared
headers), then `ninja`.

## Appendix: the reverted device experiment

Re-apply this to `d3dpt/hw/d3dpt_vga.c` to reproduce the `palette nz` log
and the "content instead of black" result. It is a diagnostic, not a fix —
the "re-apply every frame" is wasteful and the DAC fallback is a probe.

```diff
diff --git a/d3dpt/hw/d3dpt_vga.c b/d3dpt/hw/d3dpt_vga.c
index 577c8ad..19cd21d 100644
--- a/d3dpt/hw/d3dpt_vga.c
+++ b/d3dpt/hw/d3dpt_vga.c
@@ -172,10 +172,28 @@ static void fb_drop_shadow(D3dptVgaState *s)
 
 static void fb_apply_palette(D3dptVgaState *s)
 {
-    int i;
+    int i, nz_reg = 0, nz_dac = 0;
 
     for (i = 0; i < D3DPT_FB_PALETTE_SIZE; i++) {
-        s->indexed->rgba[i] = 0xff000000u | (s->pal[i] & 0xffffffu);
+        if (s->pal[i] & 0xffffffu) nz_reg++;
+        if (s->vga.palette[i * 3] | s->vga.palette[i * 3 + 1] | s->vga.palette[i * 3 + 2]) nz_dac++;
+    }
+    {
+        static int said;
+        if (said < 4) { said++; info_report("d3dpt-vga: palette nz REG=%d DAC=%d", nz_reg, nz_dac); }
+    }
+    /* EXPERIMENT: when the linear-mode palette is empty, fall back to the
+     * VGA core's DAC — a game may have programmed the standard 8 bpp DAC
+     * (ports 0x3c8/0x3c9) rather than our REG_PALETTE. */
+    for (i = 0; i < D3DPT_FB_PALETTE_SIZE; i++) {
+        if (nz_reg == 0 && nz_dac != 0) {
+            s->indexed->rgba[i] = 0xff000000u |
+                ((uint32_t)c6_to_8(s->vga.palette[i * 3]) << 16) |
+                ((uint32_t)c6_to_8(s->vga.palette[i * 3 + 1]) << 8) |
+                (uint32_t)c6_to_8(s->vga.palette[i * 3 + 2]);
+        } else {
+            s->indexed->rgba[i] = 0xff000000u | (s->pal[i] & 0xffffffu);
+        }
     }
     s->pal_dirty = false;
 }
@@ -301,7 +319,7 @@ static void d3dpt_vga_gfx_update(void *opaque)
     if (!s->lin_on || memcmp(&s->lin, &m, sizeof(m)) != 0) {
         fb_switch(s, &m);
     }
-    if (s->pal_dirty && m.bpp == 8) {
+    if (m.bpp == 8) {  /* EXPERIMENT: re-apply every frame so a VGA-DAC palette shows */
         /* a new palette recolours every pixel: one full conversion */
         fb_apply_palette(s);
         s->full_update = true;
```
