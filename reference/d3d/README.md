# Direct3D golden captures

Frames of the reference workloads (`D3DGAME9.EXE` / `D3DGAME8.EXE`,
`guest-tools/src/`, doc 14) taken on the reference rig (doc 09): a
Pentium 4 1.7 GHz with a GeForce 6200 (ForceWare `nv4_disp.dll`),
Windows XP SP3, an 85 Hz CRT. Every emulated Direct3D path — the
paravirtual device on DXVK, the Wine executor, WineD3D in the guest — is
diffed against them with `tools/bmpdiff.py`; the guest stage of
`scripts/test.sh` does it with a budget (`D3D_GOLDEN_BUDGET`,
`docs/testing.md`).

```sh
tools/bmpdiff.py reference/d3d/rig-2026-09-03/d3dgame9-w300-ff.bmp candidate.bmp \
  --mask 0,368,270,112 --tolerance 8 -o diff.bmp
```

The BMPs are kept as the program writes them (24-bit 640×480, ~900 KB),
so a byte-identical candidate compares equal with no conversion.

**Always mask the HUD** (`--mask 0,368,270,112`): its frame-time bars in
the bottom left draw wall time and differ between any two runs, on the
rig too.

## rig-2026-09-03

Captured 2026-09-03 (the rig's clock reads a day ahead) with the EXEs of
commit 667ecac.

| File | Command line | Notes |
|---|---|---|
| `d3dgame9-w300-ff.bmp` | `D3DGAME9 -frames 600 -dump 300 d3d9_dump.bmp` | windowed 640×480 X8R8G8B8, hardware vertex processing, fixed function |
| `d3dgame9-w300-vs11.bmp` | `D3DGAME9 -shader -frames 600 -dump 300 d3d9_dump_shader.bmp` | the cubes through the vs_1_1 vertex shader with the **fixed-function pixel stage** (see below) |
| `d3dgame9.log`, `d3dgame8.log` | every run of the session | adapter and caps lines, fps per second |

- **The shader golden has no pixel shader.** The rig's d3dx9_36 HLSL
  compiler refuses ps_1_1 (X3539), so `CreatePixelShader` never ran;
  that build's log calls the case "fixed function", wrongly —
  `draw_cubes` keys on the vertex shader alone. Rendering is frozen at
  this build: an emulated run must draw what the rig drew, so the ps_1_1
  refusal stays until a new golden set is taken. With the HUD masked the
  two goldens differ in 9.7 % of pixels, all of them the cubes.
- The logs' "N frames, M ms" summaries are wrong in this build (they
  measure since the last fps report); later builds fixed the log only,
  no pixels.

Rates on the rig, vsync on (`-novsync` was not run):

| Run | fps |
|---|---|
| d3dgame9 windowed / full-screen 8888 / full-screen 565 | 85 (vsync) |
| d3dgame8 full-screen | 85 |
| d3dgame8 windowed (`D3DSWAPEFFECT_COPY_VSYNC`) | 43–44: the GeForce driver paces a windowed COPY_VSYNC present at half the refresh rate — real behaviour, remember it when reading d3dgame8 windowed numbers |
