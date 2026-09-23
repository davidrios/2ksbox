# 9. Reference hardware rig

A real period machine is the ground truth the emulated stack is judged
against: **Pentium 4 1.7, GeForce 6200, dual-boot Windows 98 / Windows
XP, a CRT monitor, a real optical drive.** Each pillar gets concrete
comparisons instead of guesses. What it has produced so far lives in
`reference/` (the Direct3D goldens in `reference/d3d/rig-2026-09-03/`, the
benchmarks in `reference/benchmarks/rig-2026-09-04/`); the CRT photo set
and the ATAPI traces are still to be taken. The rig stays stock — it is
an oracle, not a dev machine.

## The monitor

A **Samsung SyncMaster 753DFX**: 17" (≈16" viewable, ≈320×240 mm of
picture), DynaFlat flat glass, and a **delta dot-trio shadow mask at
≈0.20 mm horizontal pitch**, not an aperture grille — no damper wires, no
vertical stripes, so a Trinitron-style grille is the wrong default for it
(doc 03). `shaders/syncmaster-753dfx.slangp` approximates it from that
geometry. **Unverified:** the pitch and viewable width are recalled from
the model's class, not read off the monitor, and everything derived
scales with them — measure the picture width and check the pitch in the
manual before treating them as fixtures. The photo set below turns the
approximation into a calibration.

## What it validates

- **Display pipeline (doc 03) — the biggest win.** CRT photographs of
  known content to tune the presets against; the real aspect of the tricky
  modes (320×200 on a 4:3 tube, 720×400 text, double-scanned low-res
  modes) against the mode table; 70 Hz DOS and 60/75/85 Hz SVGA on the
  tube as the reference for our frame pacing.
- **CD-ROM (docs 05, 17).** Golden ATAPI traces — the MMC command,
  response and sense sequences a protected title (SafeDisc, SecuROM)
  issues during its disc check, captured on the rig or the same drive in
  a modern box — as fixtures for the virtual drive; known-good dumps with
  subchannel and error data from discs that verifiably pass on the rig;
  and the A/B: a title that fails in the VM but passes on the rig is a
  backend bug by definition.
- **3D (docs 04, 14).** Screenshots of the acceptance titles on the
  GeForce 6200 (native D3D8/9 and OpenGL) diffed against every emulated
  path; `D3DGAME9`'s golden frames are the executor's oracle (doc 14
  P0a). The 6200 is a 2004 DX9 card: the right oracle for the XP era and
  late-98 Direct3D titles, with no Voodoo oracle for early Glide output
  (86Box and community references cover that).
- **Performance (docs 06, 22).** Super PI, 7-Zip and `SSEBENCH` have run
  on the rig (`reference/benchmarks/README.md`); 3DMark 99/2001 SE/03 and
  game timedemos are still to come. In-app expectations then read "X % of
  the reference P4", a testable claim.
- **OS behaviour.** Install-flow quirks, control-panel behaviour and
  autorun checked against real Win98/XP when a VM looks suspicious.

## The CRT photo set

The patterns are `guest-tools/src/crtcal.h`, one definition compiled into
both sides: `TESTS\CRTCAL.EXE` on the guest-tools ISO puts them on the
tube at the exact mode through an exclusive full-screen DirectDraw
primary — no blit, no stretch, because a scaler is what would otherwise
be measured — and `build/crtcal-render` (`tools/crtcal-render.c`, the
`crtcal` check) writes the same pixels as BMPs, which `player --shader
<preset> --calib <dir>` runs through a preset. One photograph, one shaded
frame, side by side; adjust the preset; repeat.

On the rig: `CRTCAL.EXE [w h [bpp]]`, then SPACE / 1–8 to step patterns,
`M` for the next mode, `L` to hide the legend, ESC to quit.

| # | Pattern | What it settles | The shot |
|---|---|---|---|
| 1 | `grid` | does the mode fill 4:3, what falls off each edge, is the geometry linear | whole screen, straight on, lens level with the tube's centre |
| 2 | `scanlines` | the beam's vertical profile, and **how many scanlines the tube draws** | macro on a band centre, plus one whole-screen frame |
| 3 | `mask` | mask kind, pitch in mm, stagger | macro, as close as the lens focuses, **ruler in frame** |
| 4 | `bloom` | how much the beam widens as it brightens | macro across the stack, one exposure for all rows |
| 5 | `sharp` | horizontal spot size, where the video bandwidth gives out | macro on the bar bands, at every mode |
| 6 | `halation` | how far light spreads into black | whole screen, dark room, exposure unchanged between shots |
| 7 | `gamma` | the tube's gamma against a dithered reference | whole screen, straight on; slightly defocused is right |
| 8 | `colour` | phosphor primaries and colour temperature | whole screen, fixed daylight white balance |

The two that pay for the trip: **2 at 320×200**, the direct answer to
whether the tube draws 400 scanlines for a 200-line mode (doc 03 rule 3),
and **3 with a ruler**, which turns the ≈0.20 mm pitch from a
recollection into a measurement. Doc 03's "Mode analysis" says which
shader parameters each pattern feeds; the preset file says which of its
values are derived and which wait on these photographs.

### 720×400, which needs DOS

Windows 98 cannot put its desktop at 720×400: it is the VGA *text* mode
(80×25 cells of 9×16, 400 lines, 70 Hz). So `TESTS\TEXTCAL.COM` is a DOS
`.COM` — run it from FreeDOS or a "Restart in MS-DOS mode" screen. It
draws its patterns with a custom character generator (a periodic pattern
tiles exactly from 256 glyphs of 9×16). Two exist nowhere else:

- **Pattern 2, the 9th column.** A cell is 9 pixels wide and a glyph 8;
  for codes 0xC0–0xDF the 9th column repeats the 8th, otherwise it is
  background. A solid glyph below 0xC0 against the block at 0xDB shows
  stripes on one half and continuous white on the other — doc 03 rule 2's
  "9-dot characters", shown.
- **Pattern 6 against pattern 1.** Text mode's 400 lines are scanned
  once, mode 13h's 200 twice, so the same one-on-one-off pattern repeats
  every 2 lines on the tube in pattern 1 and every 4 in pattern 6. Two
  photographs at one camera setting settle doc 03 rule 3.

Checked under our own emulator: FreeDOS, `TEXTCAL.COM` from
`FDAUTO.BAT`, a QMP screendump reporting **720x400**, and the player
logging `720x400 VGA text 80x25 (9-dot) — 4:3 picture, pixel aspect
0.741, 400 scanlines`.

### Getting the shot

- **Shutter ≥ 2 frame periods.** A CRT is lit only where the beam is; a
  fast shutter photographs a band. At 85 Hz use 1/30 s or slower, never a
  flash. This one mistake ruins a whole set.
- **Manual everything:** exposure, daylight white balance, ISO, focus.
  Halation and bloom are comparable only if exposure did not move; write
  it down.
- **Tripod, straight on, dark room.** For `grid` the lens is level with
  the tube's centre; off-axis makes a linear picture look pincushioned.
- **A ruler in the macro frames**, taped flat to the glass and in focus
  with the phosphors: without a scale a mask photo gives shape, not pitch.
- **RAW if available**, and record the OSD settings (brightness, contrast,
  colour-temperature preset); **moiré reduction off** — it defocuses the
  beam on purpose.
- **Warm up twenty minutes**; a cold tube has not settled its geometry.
- One whole-screen frame per pattern per mode, plus the macros, into
  `reference/` with the capture settings in the file name or a sidecar.

## `reference/`

CRT photo sets (with capture settings), ATAPI trace fixtures, benchmark
results and real-hardware screenshots are versioned there beside the
tests that consume them; disc dumps and disk images are not committed.
Still to capture: the CRT photo set (M2's calibration) and the ATAPI
traces of protected titles (doc 17's open items).
