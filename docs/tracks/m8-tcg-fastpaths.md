# Track M8: CPU fast paths in TCG (docs 13 and 16)

This track made emulated floating-point code fast under TCG. It covers:

- the x87 fast path and shadow-double translator (patches 05/06, doc 13);
- SSE float inline (patch 11, doc 16);
- MMX/SSE integer and permutes inline (patch 12, doc 16);
- the TCG float and vector opcodes those patches added to both backends.

The track was merged to `main` on 2026-09-04 and its branch deleted. It is
bit-exact on aarch64 (the Air) and x86-64. This doc keeps the
track's scope, test loop and open items. The designs and measurements are
in docs 13 and 16, and each patch's row is in `patches/qemu/README.md`.
The later x87 patches came from other work, and doc 13 covers them too:
37 (PE sticky), 45 (binary32 at PC=24) and 47–49 (PC=64).

## Scope and files

- QEMU patches `05-x87-fast`, `06-x87-inline-tcg`, `11-sse-inline-tcg` and
  `12-simd-inline-tcg`. In the tree they touch:
  - `target/i386/tcg/`: `x87-fast.h`, `x87-shadow.c.inc`,
    `sse-fast.c.inc`, `sse-fast-lane.c.inc`, `simd-fast.c.inc`;
  - the new opcodes: `include/tcg/tcg-opc.h`, `tcg/tcg-op*.c`,
    `tcg/aarch64/`, `tcg/i386/`;
  - hooks in `translate.c`, `emit.c.inc`, `decode-new.c.inc`,
    `fpu_helper.c`, `cpu.h`, `cpu.c`.

  The SSE and SIMD patches were 07/08 on the track branch. Old commit
  messages still use those numbers.
- CPU properties: `x87-fast`, `sse-fast` and `simd-fast` (all on). Each one
  also has a switch in the launcher's optimizations list.
- Tests:
  - `tools/x87-fast-test.c` (the `x87-fast` check);
  - `tools/x87-guest-test.py` and `tools/sse-guest-test.py` (the
    `x87-guest` and `sse-guest` checks);
  - `tools/x87-unwind-test.asm`.
- Benchmarks:
  - `guest-tools/src/ssebench.c` (`SSEBENCH.EXE` on the ISO);
  - `tools/xp-ssebench.sh`, which runs it in XP per `-cpu` config and
    works on macOS;
  - the x87/SSE sections of `reference/benchmarks/README.md`, which has
    the rig row.

## Test loop

```sh
scripts/build.sh                    # re-applies the queue after a patch edit
python3 tools/x87-guest-test.py     # the slow blocks are shared by x87 and SSE:
python3 tools/sse-guest-test.py     #   run both after any change to either
scripts/test.sh all                 # before every commit
tools/xp-ssebench.sh ~/vms/winxp.qcow2   # default, sse-fast=off, both off
```

- **Pass criterion.** Each battery must end with its fast-path-on and
  fast-path-off outputs identical. The last counts were 382,251 lines for
  x87 and 546,425 for SSE. Tool detail is in `docs/testing.md`.
- **Slow-path counters.** `info registers` prints `SSE-fast slow paths:
  guard= handover= helper= cvt/comis=`. A workload whose counters grow by
  millions is running on the slow path.
- **Editing a patch.** Follow the recipe in `patches/qemu/README.md`.
  Patch 12 edits the same TCG files after patch 11, so a change to 11
  means regenerating 12 as well.

## What stayed open

- **A real-workload number.** A Direct3D title in XP with and without
  `x87-fast=off,sse-fast=off`, the first end-to-end number for patches 06,
  11 and 12 together. M4 lists the same item.
- **The rest of clamp+cmp** (43 % of the rig on x86-64, 34 % on the Air).
  First inline `movmskps`/`movmskpd`, which still go through the helper.
  Then reduce the per-operand TLB cost of memory operands.
- **Cheaper packed checks.** Hoist the vector constants: each packed op
  re-materializes them, at two instructions each on aarch64. A
  vector-to-scalar move opcode would remove the `env->sses_scratch` round
  trip from the scalar path.
- **Still on helpers:**
  - `cvtps2dq`/`cvttps2dq`/`cvtdq2ps`;
  - `cvtpi2ps`/`cvtps2pi`;
  - `pmuludq`;
  - SSE3 `haddps`/`addsubps`, SSSE3 and VEX forms, which era code does
    not use.
- **The x87 C transform** (16–18 % of the rig), limited by the shadow
  translator's throughput on cheap ops. Improving it is a doc 13
  redesign, not started.
