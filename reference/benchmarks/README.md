# CPU benchmarks: XP under TCG against the reference rig

XP-on-Apple-Silicon performance as a fraction of the real rig (doc 09:
Pentium 4 1.7 GHz + GeForce 6200), from the same binaries run the same
way. It is the M1 exit criterion of doc 08 and the in-app expectation of
doc 06. This file keeps the rig comparisons of the x87 and SSE work (docs 13
and 16). The full evaluation of the patch queue, with a reproducible
harness, per-switch runs and the 3D workloads, is doc 22.

## Method

| Test | Why | Record |
|---|---|---|
| Super PI mod 1.5 XS, 1M digits | single-thread x87 FP, era-standard, runs on 98 and XP | seconds, best of 2 |
| 7-Zip benchmark (Tools → Benchmark) | integer and memory, one thread in a 1-CPU guest | compress / decompress GIPS |
| Boot to desktop | disk and interrupt heavy; what a user notices | power-on to a responsive Start menu |
| Feel check | Explorer scrolling, Notepad typing, Minesweeper, a window drag | anything that stutters |
| `SSEBENCH.EXE` (`guest-tools/src/ssebench.c`) | D3DX-shaped SSE1 kernels and the same maths in x87 C | ns per op |

Run everything twice and keep the best, with nothing else on the host.
macOS sometimes parks the vCPU thread on an efficiency core for a whole
run, a uniform ~2×, so compare within one boot's pair rather than across
boots.

The Air runs XP in the player:

```sh
PLAYER_LATENCY=1 target/release/player --shader third_party/slang-shaders/crt/crt-lottes.slangp -- \
  -L $PWD/qemu/pc-bios -machine pc -cpu pentium3 -m 512 -hda ~/vms/xp.qcow2 \
  -vga cirrus -net none -usb -device usb-tablet -device AC97,audiodev=embed0 \
  -cdrom ~/vms/bench.iso
```

Runs up to 2026-09-03 used `-vga std`, which XP drives as 640×480×16
VGA; that does not matter to Super PI and 7-Zip. The benchmark goes in on
an ISO (`hdiutil makehybrid -iso -joliet` on macOS, `mkisofs -J -r` on
Linux). The rig runs the same files from a CD-R on its XP partition, and
results come back through `tools/upload-server.py` (a plain upload form on port
8000 that IE6 can use, saving into `build/uploads/`). `SSEBENCH.EXE` runs
headless in XP through `tools/xp-ssebench.sh` (`docs/testing.md`), with
`-iter 20` at x87 PC=53.

## Super PI, 7-Zip and boot

| Machine | Config | Super PI 1M (s) | 7-Zip 26.02 GIPS (comp / decomp) | Boot (s) | Date |
|---|---|---|---|---|---|
| Rig, XP | Pentium 4 1.7 GHz (Willamette, CPUID family 15 model 1), 512 MB | 122 (2:02) | 0.742 / 0.776 | ~30 | 2026-09-02 |
| M1 Air, XP in the player | TCG `-cpu pentium3 -m 512`, macOS 26 | 589 (9:49) | 0.996 / 1.511 | ~30 | 2026-09-02 |
| M1 Air | + patch 05 (`x87-fast`) | 393 (6:33) | | | 2026-09-02 |
| M1 Air | + patch 06 (x87 stack as host doubles) | 117 (1:57), both runs | | | 2026-09-03 |
| Air ÷ rig | | 21 % → 31 % (05) → **104 %** (06) | **134 % / 195 %** | parity | |

- **Integer and memory (7-Zip).** The emulated XP is 1.3–2× a P4 1.7,
  so a 2.2–3 GHz P4 or an early Athlon 64.
- **x87 (Super PI).** On softfloat it is 21 % of the rig, Pentium II
  300–400 MHz territory, and that is what FP-heavy game code and
  software renderers hit. Patch 06 brings it to parity; `x87-fast=off` on the
  same build reproduces softfloat pace (0:35 after loop 1), so the whole
  gain is the patch. 7-Zip and boot were not re-run (integer path
  untouched).
- Boot is disk and interrupt bound; the host SSD hides the rest.
- M9's patch 14 (macOS W^X state tracked per thread) took a Super
  PI run from 1:36.2 to 1:25.3 under the profiler
  (`docs/tracks/m9-tcg-aarch64.md`).

In-app wording (doc 06): integer speed of a fast P4, floating-point
parity with the rig; without patch 06 the FP half is Pentium II class.

## SSE and MMX

The register-only DOS kernels (`SSEBENCH.COM`, run by
`tools/sse-guest-test.py`, 40M iterations, `sse-fast` on and off):

| Machine / config | packed 8-op chain (s) | scalar 7-op chain (s) |
|---|---|---|
| M1 Air, helpers (`-cpu pentium3,+sse2,sse-fast=off`) | 4.56 | 2.80 |
| M1 Air, patch 11 inline | 0.38 (**12×**) | 0.77 (**3.6×**) |
| M1 Air, after the x86-64 session's fixes | 0.33 (13.8×) | 0.82 (3.4×) |

That is ~1.2 ns per packed op and ~2.7 per scalar op inline, against ~14
and ~10 in the helpers. On the Air the MMX chain is 0.55 s against 1.98
(3.6×) and the `SSEBENCHC` clamp+cmp kernel 0.33 s against 2.86 (8.7×).
On the x86-64 box, with its native `fmin_vec` / `fmax_vec` / `fcmp_vec`
/ `mulsh_vec` / `*narrow_vec` opcodes (`docs/tracks/m8-tcg-fastpaths.md`),
the gains are packed 10.0×, MMX 2.1×, clamp+cmp 6.5×. Loops with memory
operands gain less, because the TLB lookup per operand is the same on
both paths.

`SSEBENCH.EXE` in XP, ns per op (lower is better), all 2026-09-04:

| Machine / config | xform | normalize | scalar chain | clamp+cmp | convert | C xform (x87) | C normalize (x87) | denormal | MMX blend |
|---|---|---|---|---|---|---|---|---|---|
| **Rig (P4 1.7), XP** | 1.28 | 1.93 | 4.09 | 1.59 | 2.50 | 0.74 | 4.02 | 1552 | 0.44 |
| Air, defaults (06 + 11 + 12) | 2.1 | 2.0 | 3.9 | 4.7 | 2.3 | 4.2 | 4.6 | 80 | 0.41 |
| Air, defaults, after the x86-64 fixes | 2.0 | 1.6 | 3.5 | 5.1 | 2.3 | 4.0 | 3.9 | 81 | 0.40 |
| Air, `simd-fast=off` | 2.4 | 2.4 | 3.8 | 4.6 | 2.3 | 4.2 | 4.3 | 83 | 0.80 |
| Air, `sse-fast=off` | 17.7 | 13.5 | 11.8 | 16.2 | 7.4 | 4.5 | 3.9 | 49 | |
| Air, `sse-fast=off,x87-fast=off` | 17.7 | 13.8 | 12.1 | 16.1 | 6.6 | 45.5 | 47.0 | 47 | |
| **Air ÷ rig** | 61 % | 97 % | 105 % | **34 %** | 109 % | **18 %** | 87 % | 19× | 107 % |
| x86-64 box (Ryzen 7 5700X), defaults | 2.6 | 2.2 | 3.5 | 3.7 | 1.9 | 4.5 | 4.1 | 63 | 0.36 |
| x86-64 box, `sse-fast=off` | 10.5 | 9.9 | 5.1 | 11.3 | 6.3 | 4.4 | 4.1 | 51 | 0.36 |
| x86-64 box, `sse-fast=off,x87-fast=off` | 10.4 | 9.8 | 5.1 | 11.1 | 6.4 | 27.9 | 31.1 | 49 | 0.36 |
| **x86-64 ÷ rig** | 49 % | 89 % | 116 % | **43 %** | 130 % | **16 %** | 99 % | 25× | 122 % |

The rig's log is `rig-2026-09-04/ssebench.log` (three runs, same mean;
SSE score 2.28 ns per op against the Air's 3.17). Every `check` value on
both hosts is identical to the rig's, so the inline paths reproduce the
P4 bit for bit on these kernels. The `sse-fast=off` rows leave patch 12
on (`simd-fast` is its own switch), hence the MMX blend stays fast.

What the table says:

- Patch 11 makes the SSE kernels 3.2–7.4× faster, packed code most;
  patch 06 makes the x87 kernels 10–12× faster; each switch touches only
  its own kernels.
- Patch 12 (MMX / SSE integer and permutes inline, `tbl_vec`) takes MMX
  blend 0.80 → 0.41, the packed transform's four `shufps` 2.38 → 2.10 per op,
  normalize 2.41 → 2.03. Scalar lane stores for the shuffles were
  *slower* than the helper (the next vector load stalled behind four
  small stores), hence the table-lookup opcode.
- **clamp+cmp is the outlier** (34 % of the rig on the Air, 43 % on
  x86-64). The XP loop does an aligned 16-byte load and store (two
  softmmu lookups) and a `movmskps`, still QEMU's helper, per iteration,
  so the gap is memory operands and `movmskps`, not `minps` / `maxps` /
  `cmpps` (M8 track, "What stayed open").
- **The x87 C transform is the other** (16–18 %). A P4 pipelines plain
  `fmul` / `fadd` at 0.74 ns per op, and the shadow-double translator
  costs ~4. The expensive ops (`fsqrt` / `fdiv` in normalize, Super PI) are at
  parity, so it is cheap-op throughput.
- The denormal kernel is the slow path by design (every multiply leaves
  the TB, 0.6× the plain helper); the P4's own denormal penalty is 1552
  ns per op, 19× slower than the emulated slow path.

Pitfalls that cost time building the benchmark:

- Keep kernel values in range. The first `convert` kernel overflowed
  past 2^31, so nearly every `cvttss2si` took the slow path (11.8M exits
  in the `info registers` counters).
- mingw's CRT starts x87 at PC=64, where no x87 fast path applies. The
  program sets PC=53 itself (`-pc24` / `-pc64` to change).
- gcc `-O2` auto-vectorises the "C" kernels into SSE unless a pragma pins
  them to x87.
