# Track M4: the paravirtual Direct3D device (doc 14, ADR-006/007)

The DLL path for Direct3D 8/9: the SysBus `d3dpt` device, the decoder and
executor over DXVK, and the guest `d3d9.dll` / `d3d8.dll`. The milestone
closed on 2026-09-04 (P0–P4, `docs/08-roadmap.md`). This doc keeps the
track's scope, test loop and open items; the design, protocol and numbers
are in doc 14. On XP the M7 display driver (doc 15, ADR-008) replaced the
per-game DLLs, which remain the Win98 path and the executor's harness.

## Scope and files

- Protocol and host side:
  - `d3dpt/d3dpt_proto.h`: bump `D3DPT_PROTO_VERSION` on any wire change.
  - `d3dpt/d3dpt_enc.h`
  - `d3dpt/exec/` (`libd3dpt_exec`; the Wine host program is M15's)
  - `d3dpt/hw/d3dpt_mm.c` + `d3dpt_exec_load.c` (patch 40)
  - the presenter in `embed/embedfx.c`
- Guest DLLs in `guest-tools/src/d3dpt/`: `d3d9.c`, `d3d8.c` (wraps d3d9),
  the vtable generators `gen_vtbl.py` / `gen_vtbl8.py`, and the `DDRAW.DLL`
  and `DINPUT.DLL` shims.
- Test programs: `guest-tools/src/d3d9test.c`, `d3dfeat9.c`, `d3dgame9.c`
  and `d3dgame8.c`, built into the ISO by `guest-tools/build-wrappers.sh`.
- DXVK: `third_party/dxvk` + `patches/dxvk/`, `scripts/*dxvk*` and
  `scripts/build-d3dpt-exec.sh`.
- Host tests: `tools/d3dpt-exec-test.cpp`, `tools/d3dgame9-native.cpp`,
  `tools/d3dfeat9-native.cpp`, `tools/bmpdiff.py` and the rig goldens in
  `reference/d3d/`.
- Shared with M7 and M15: `d3dpt_proto.h`, `d3dpt/exec/` and `d3dpt/hw/`.
  Rebase first, edit minimally, and name the track in the commit.

## Test loop

```sh
scripts/build.sh      # QEMU, DXVK, the executor and the guest-tools ISO
scripts/test.sh       # host checks: d3dpt-exec, d3dgame9-nat, d3dfeat9-nat
scripts/test.sh all   # + the guest stage: XP on the device
```

- **Guest stage.** Boots `~/vms/winxp.qcow2` with `snapshot=on` and runs
  `DDVMTEST`, `D3DGAME9`, `D3DGAME8` and `D3DFEAT9`. D3DGAME9/8 must be
  pixel-identical to the native frame outside the HUD and within
  `D3D_GOLDEN_BUDGET` of the rig golden; D3DFEAT9 must be byte-identical to
  the native frame, with the same query and getter lines.
- **After a protocol bump,** rebuild the executor and the ISO, or the
  suite fails with `protocol mismatch` or a guest that never attaches.
- Tool detail is in `docs/testing.md`; the env knobs
  (`D3DPT_DUMP_DIR`/`D3DPT_DUMP_EVERY`, the guest's `d3dpt_trace.on`) in
  `docs/development.md`.

### A game on the device

`tools/xp-game-test.sh` runs a game headless. Discs go on the player's IDE
slots (`CDS=`); `FRESH_DLLS=1` puts the ISO's DLLs next to the EXE.

| Option | What it catches |
|---|---|
| `SHOTS=` | launchers and error boxes on the VGA surface |
| `DUMP_EVERY=` | the frames the game presents |
| `DRW_AFTER=` | every thread's stack, through Dr. Watson |
| `PAGEHEAP=1` | heap overruns, faulting where they happen |
| `TRACE=1` | the DLL's call trace |

A "frozen" game and KVM `-cpu host` breaking Max Payne are in
`docs/00-status.md` "Gotchas".

## What stayed open

- **The DLL path's stubs** (doc 14 has the list): P8 textures (Vice City's
  menu background is grey noise; plan: expand to A8R8G8B8 on upload in the
  guest DLL, re-upload on a palette change), volume textures, swap-chain
  objects, `GetFrontBuffer` (Max Payne calls it), `ProcessVertices`,
  `LockRect` on render targets and depth surfaces, and the lost-device
  protocol. They matter for Win98 titles; XP titles go through the display
  driver's DDI.
- **Hand play in the player.** Max Payne's tutorial level and Vice City's
  menu were reached headless; input, fps and sound by hand were never
  recorded on this path.
- **Performance, when a game asks for it:** zero-copy present through
  DXVK's Vulkan interop instead of GetRenderTargetData, Present pacing
  against the player's vsync, and a decoder thread off the vCPU (doc 14
  defers it until a measurement asks). Measure first with
  `PLAYER_LATENCY=1`.
- **A real-workload x87/SSE number.** A D3D title with and without
  `-cpu pentium3,x87-fast=off,sse-fast=off`. Shared with M8.
