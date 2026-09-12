# M14 — Glide 3

Opened 2026-09-12. The pass-through already carried Glide 3 end to end
except for one piece: qemu-3dfx's guest `GLIDE3X.DLL` is on the
guest-tools ISO and `SETUP.EXE` installs it, and `hw/3dfx` dispatches it —
but to a host library that did not exist. OpenGLide stops at Glide 2.x,
so a Glide 3 game's `grGlideInit` found none of the new entry points and
every draw was blocked as a null pointer. **Diablo II decides it**: its
Glide renderer was "a lot better" than its Direct3D one when the user
played it under an emulated Glide (`patches/openglide/README.md`, "Glide 3
— where it would come from").

## The route (decided 2026-09-12)

**Our own Glide 3 layer on OpenGLide**, not a Voodoo 2 device. The README's
decision procedure wanted Diablo II measured in 86Box's Voodoo 2 on the Air
first; the user picked the wrapper without the number, for the reasons
that were already on the table: the dispatcher and the guest DLL exist, so
this is only the host half; it renders on the GPU, on every host OpenGLide
already builds for; and it stays the fast path even if a Voodoo 2 device
ever comes, which would sit beside it rather than replace it. The chip is
still "later, if a game demands it".

## How it is built

`hw/3dfx/glide2x_impl.c`'s `init_glide2x("glide3x.dll")` looks up every
entry point as `wrap3x_<name>` first and `<name>` second, and the
dispatcher already marshals everything Glide 3 changed (the vertex bytes
by the layout it was told, the 3x texture tables, `grLfbWriteRegion`'s
extra argument, the context). So **one library serves both APIs**:

- what Glide 3 kept with the same meaning is OpenGLide's own export,
  untouched;
- what Glide 3 re-encoded under an old name is a `wrap3x_` export in
  `glidept/host/glide3.cpp`, translating into OpenGLide's Glide 2 calls —
  the vertex (the game's own layout, walked into a `GrVertex`), the texture
  level of detail and aspect ratio (log2, counting the other way), the
  texture tables (no TMU argument), `grSstWinOpen`'s context,
  `grLfbWriteRegion`'s pixel-pipeline flag, the saved state (Glide 2's
  `GlideState` plus ours, inside hw/3dfx's default 312-byte area);
- what Glide 3 added is a plain export there: `grVertexLayout`,
  `grDrawVertexArray[Contiguous]` (points, lines, strips, fans, polygons,
  triangles, and the two `_CONTINUE` modes that pick up where the last call
  left off), `grGet` / `grGetString` / `grReset` / `grQueryResolutions`,
  `grCoordinateSpace` / `grViewport` / `grDepthRange` (clip coordinates
  divided by w and mapped through the viewport and depth range, colours
  from [0,1], s and t scaled by the source texture's aspect — 3dfx's rules),
  `grEnable`/`grDisable`, `grGlideGet/SetVertexLayout`, `grSelectContext`,
  `grFinish`/`grFlush`.

`build-glide.sh` makes `libglide3x` a link to `libglide2x`, the name
hw/3dfx's own search tries for a Glide 3 guest; `QEMU_GLIDE_LIB` and the
player's `companions.rs`, which name the one file, are right for both.
`glidept/glide3.h` is the ABI: only what Glide 3 added to the 2.4 header,
plus, for a guest program, prototypes for the calls whose signature changed
(bound to the real `_name@N` export by an asm label). 3dfx's released Glide
3 source is the specification — its licence is not GPL-compatible, so it is
read, never copied.

What the wrapper claims to be: a **Voodoo Graphics** with one TMU, 256-texel
textures, 16-bit 565 colour and depth, 4 MB of frame buffer — what
OpenGLide is, not a board it is not. No extension string yet
(`grGetProcAddress` has nothing to hand out).

## Owned files

`glidept/glide3.h`, `glidept/host/glide3.cpp`, `guest-tools/src/glide3test.c`,
the Glide 3 half of `tools/glide-host-test.cpp` and of
`tools/glide-guest-test.sh` (`TEST=glide3`), `scripts/build-glide.sh`'s link.
Shared with M3: `patches/openglide/`, `glidept/host/*`.

## State

**Step 1 (2026-09-12): the layer, and the checks that guard it.**

- `glide3-host` in `scripts/test.sh` — `build/glide-host-test 3`: the
  wrapper loaded through hw/3dfx's own dispatcher as Glide 3; every one of
  the 94 exports of the guest's `glide3x.def` resolves (and the 21 whose
  meaning changed resolve to a `wrap3x_`); `grGet` answers before
  `grGlideInit`; then scenes checked in the frame the player receives — a
  16-byte packed vertex (x, y, 1/w, ARGB), a fan plus a strip whose lower
  half exists only if `GR_TRIANGLE_STRIP_CONTINUE` carried the vertices
  over, clip coordinates with w = 2, a 32×16 texture in log2 encoding
  (`grTexTextureMemRequired` must say 1024 bytes; untranslated it is an 8×2
  texture of 32, and the four quadrants must land where they belong), and
  the viewport surviving `grGlideGetState`/`SetState`. 9 cases, 0 failed on
  the first run; `glide-host` (Glide 2) unchanged.
- `TESTS\GLIDE3TEST.EXE` on the guest-tools ISO and
  `TEST=glide3 tools/glide-guest-test.sh <image>`: the same scenes in a
  Win98 guest, through the guest DLL's own vertex copying and size
  arithmetic, read back through `grLfbLock`, plus a close/reopen.
  **Passes, 2026-09-12**, on an overlay of `~/vms/win98.qcow2` in the
  player: `glide3test: 7 cases, 0 failed`, no `nullptr call blocked` in
  the player log. The wrapper log shows what the dispatcher asks at
  `grGlideInit` — `init_g3ext` requests the Voodoo 3 `…Ext` entry points
  through `grGetProcAddress`, and gets none, which is right until they
  are claimed.

## Next steps, in order

1. **A real Glide 3 title.** Diablo II is a Battle.net download on this box
   (`Downloader_Diablo2_enUS.exe`): the user installs it into a Win98
   image, then `PLAYER=1 tools/win98-game-test.sh` with `-3dfx`, and the
   wrapper log (`GLIDE_HOST_LOG`) says which `grGet` pnames and
   `grVertexLayout` parameters it used that nobody has exercised. Unreal
   Tournament second, NFS Porsche (`voodoo2z.dll`) third.
2. **What a title will find missing**, most likely first: a second TMU
   (OpenGLide renders TMU0 only, and says `GR_NUM_TMU` = 1, which the
   Unreal engine answers with extra passes); the extension set a Voodoo 2
   had (`TEXMIRROR`, `CHROMARANGE`, `PALETTE6666`, `FOGCOORD`) through
   `grGetProcAddress` — hw/3dfx's `init_g3ext` already asks for their
   entry points; `grLoadGammaTable` (logged once, not applied: OpenGLide has
   one gamma value, not a ramp); `GR_PARAM_FOG_EXT`.
3. Strip winding under culling: `strip_add` reverses every other triangle
   so a strip faces one way, which nothing has checked with `grCullMode` on.
4. The macOS wrapper is built from the same sources and inherits all of
   this, proven by nothing yet — M3's open item, now for both APIs.
