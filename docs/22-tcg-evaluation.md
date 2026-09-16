# 22. Exact fast paths for x86-on-Arm emulation: 2ksbox's QEMU patch queue, measured

*2ksbox project, 2026-09-16. Everything here is reproducible from the tree:
`tools/specbench/build-guest.sh` builds the benchmark suite,
`tools/specbench/run.sh <image> all` runs the matrix,
`tools/specbench/report.py --md` prints its tables, and the `tools/w98-*.sh`
runners take the game measurements. Numbers are from one machine, an M1
MacBook Air, at the commit that carries this document.*

## Abstract

2ksbox runs Windows 98 and XP guests on Apple Silicon through QEMU's Tiny
Code Generator (TCG), the pure-software x86 translator, because no
hardware on an Arm Mac can run x86 code directly. Stock TCG is exact but
slow on exactly the code these guests run: x87 floating point goes through
an 80-bit software float library, every guest memory access walks a
software TLB, every `ret` leaves generated code, and the era's software
renderers rewrite their own inner loops so often that translation
dominates. We describe a queue of sixty patches over QEMU 9.2.4, of which
about twenty are performance work, every one behind its own switch and
every one bit-exact (the guest computes the same bits with the switch on
and off), and measure them in two tiers. The first is **reproducible
benchmarks** — nbench (BYTEmark), 7-Zip's built-in benchmark, Super PI and
our own SSE/x87 kernel set, run headlessly inside the XP guest with their
output checked — against pristine QEMU 9.2.4, against our tree with every
switch off, and with each switch removed from the default: 2.34x geometric
mean, 5.3x on x87 code, 3.4x on SSE, 1.1x on plain integer code, with
identical outputs everywhere. The second is the **games** the patches were
written for — 3DMark 99 Max, 3DMark2001 SE, Blood, Moto Racer and Quake
II — on one Windows 98 machine with the same A/B, every number checked
against a screendump of what was on screen: 2.2x to 30x, because the
patches for self-modifying code and for Windows 98's memory manager are
invisible to a benchmark program and decisive under a game. A
SPEC-CPU2006-derived integer suite, measured once, gains 1.07x, which says
the patches do not target compiled integer code. We then asked what the
literature's remaining ideas are worth on this tree: each candidate was
implemented behind a switch and measured — a pc-indexed jump table and a
back-edge-only interrupt check within noise, return prediction through a
host call/return pair a loss — and the largest parked design, running the
translator inside a hardware VM with the guest's page tables mirrored so
that a guest access is a host access, was priced without building it: a
memory census of every workload times microbenchmarks of the access
sequences projects 1.1–1.2x, with the design's one risk (a working set
beyond the nested TLB) absent from every workload measured. The tree is,
on these programs, at the point where the remaining generic ideas buy a
fifth at most. The evaluation's first
finding was about its own method: on macOS/arm64 a third of QEMU launches
place TCG's code buffer 8 GiB from the helpers and run helper-heavy code
35–45 % slower, pristine QEMU included; a load-time reservation (patch 63)
makes every launch the near one.

## 1. Introduction

**The problem.** A Windows 98 or XP guest wants an x86 processor. On an
Arm host that means dynamic binary translation: QEMU's TCG decodes guest
x86 into an intermediate representation and emits AArch64 code for it, one
*translation block* (TB, a straight-line run of guest instructions) at a
time, caching the result. TCG is a generic, retargetable translator that
is correct for hundreds of guest/host pairs, and it pays for that
generality on every guest instruction: an x86 memory operand becomes a
software TLB lookup of ten host instructions; an x87 `fmul` becomes a call
into a C library that multiplies 64-bit mantissas by hand; a `ret` becomes
a call into a C helper that hashes the target address. On the M1 Air the
guest runs at roughly the speed of a Pentium 4 of 2002, which is enough
for the desktop but, on a game, is the difference between playable and
not.

**What we did.** The patches were written profile-first: run a real
workload (7-Zip, Super PI, Moto Racer, Blood, 3DMark 99, 3DMark2001 SE),
find where the vCPU thread's time goes with a sampling profiler mapped
back to guest instructions, and fix the largest thing the profile named.
The fixes are patches on the pinned QEMU submodule (`patches/qemu/`,
applied by `scripts/prepare-qemu.sh`; the README there has each patch's
full story). Two rules shaped them: **only optimizations that could help
any guest** (nothing title-specific), and **exact before fast** — a fast
path is taken only when the answer it computes is the bit pattern the
slow path would have computed, and the one inexact mode that exists
(`x87-pc64-as-53`, §3.1) is opt-in and labelled as inexact in the
launcher.

**What this paper adds.** (a) A reproducible benchmark set inside the XP
guest, measured on pristine QEMU, on our tree with every switch off, on
our default, and with every single switch removed from the default; (b) a
check that every configuration computes the same output; (c) the games
the patches were written for, measured on one Windows 98 machine with the
same A/B and each number checked against a screendump; and (d) what a
SPEC-style integer suite makes of the tree, so a reader from the
binary-translation literature (§7) can place the numbers; and (e) what
the literature's remaining ideas are worth on this tree, each tried or
priced (§8). It is written
for developers: the mechanisms are explained at the level needed to reason
about them, and every claim points at the patch or tool that carries it.

## 2. Background: how TCG runs x86 on AArch64

A reader who knows QEMU can skip this section.

**Translation blocks and the code cache.** TCG translates guest code
lazily. Execution starts at a guest PC; if the code cache has a TB for
(PC, code segment base, CPU mode flags), it jumps in; otherwise the
translator decodes guest instructions until a branch and emits host code
for them. Direct branches between TBs are *chained* (the host jump is
patched to point at the target TB, so no lookup happens again); indirect
branches (`ret`, `call *`, `jmp *`) cannot be chained and go through a
lookup — a small direct-mapped *jump cache* first, then a hash table.
When a lookup fails or an interrupt is pending, execution returns to the
C main loop, which is a few hundred host instructions each way.

**The guest register file.** TCG keeps the guest's registers in memory
(the `CPUX86State` struct, reached through a reserved host register). The
register allocator loads them into host registers inside a TB and stores
them back at the TB's end, before any helper call that might read them,
and at any instruction that might fault. Across a chain of TBs the
registers therefore go through memory at every block boundary.

**Memory: the softmmu TLB.** The guest has its own page tables. Every
guest load or store is translated by a *software TLB*: generated code
computes an index from the guest address, loads the TLB entry's tag and
compares it, and on a hit adds the entry's offset and does the host load.
That is about ten host instructions on the fast path, with a dependent
load chain in the middle, per guest memory operand. On a miss a helper
walks the guest page tables. The TLB is flushed whenever the guest
switches address spaces (a `mov cr3`), and QEMU 9.2 sizes it dynamically
from the number of entries used since the last flush.

**Helpers.** Anything the translator does not inline becomes a call into a
C function: x87 and most SSE floating point (through `softfloat`, which
implements IEEE arithmetic on integers, exactly, including the 80-bit x87
format), integer division, flag computation for a few instructions, I/O,
and every TLB miss. A helper call costs the register sync described
above plus the call itself.

**Self-modifying code.** The code cache must be invalidated when the guest
writes to a page it has translated code from. QEMU tracks that per page:
a store into such a page takes a slow path that finds every TB
overlapping the written bytes and throws it away; the next execution
retranslates. Windows 9x-era software renderers write their inner loop's
operands into the instruction stream per span, so this path can run
thousands of times per frame.

**Floating point.** x87 arithmetic is at 64-bit precision by default but
Windows sets it to 53 bits at process start and Direct3D sets 24 bits for
the life of a device; the result of each operation must then be rounded
to that precision, with IEEE flags. QEMU implements all of it in
`softfloat` on an 80-bit representation. SSE is inlined only for a few
instructions; the rest are helpers. The host FPU computes binary64 and
binary32 natively, with the same rounding, so for the common modes the
same bits can be produced by one host instruction — *if* the mode and the
operands are checked.

## 3. The optimizations

Fifteen switches cover the queue's performance patches. Four are CPU
properties (`-cpu pentium3,<name>=off`): `x87-fast`, `sse-fast`,
`simd-fast`, `rep-fast`, plus the opt-in `x87-pc64-as-53`. The rest are
accelerator properties (`-accel tcg,<name>=off`): `smc-same-value`,
`soft-imm`, `inline-lookup`, `tb-invalidate-fast`, `tlb-floor`,
`tls-hot-paths`, `jump-cache-keep`, `eob-chain`, `tlb-retire`, plus the
opt-in `pinned-regs`. The launcher exposes all of them ("Emulation
optimizations" in the machine form), and `scripts/test.sh`'s
`optimizations` check proves the wiring from the checkbox to the QEMU
command line. A switch is the oracle: a guest that misbehaves is
diagnosed by turning switches off, not by bisecting patches. Each
subsection names the profile fact that motivated its patches; the
measurements are §5 and §6.

### 3.1 Floating point on the host FPU (`x87-fast`, `sse-fast`, `simd-fast`, `x87-pc64-as-53`)

**x87 at 53 and 24 bits (patches 05, 06, 37, 45; doc 13).** Patch 05
computes an x87 operation on the host FPU whenever the guest's control
word says 53- or 24-bit precision with round-to-nearest, which is what
Windows and Direct3D set; the result is bit-exact against softfloat
because a binary64 (or binary32) operation on operands that are
themselves representable at that precision *is* the correctly rounded
result. Patch 06 goes further and keeps the x87 stack as host doubles
*inside* generated code across instructions — the "shadow" stack — with
the conversion back to the 80-bit format only at TB exits, before
helpers and on faults. Patch 37 removed the per-operation inexact-flag
computation when the flag was already sticky (the state every program is
in after its first inexact operation), and patch 45 made the shadows
binary32 at 24-bit precision so a Direct3D game's x87 operation is one
host instruction.

**x87 at 64 bits (patches 47, 48, 49).** Code that keeps the default
64-bit precision (3DMark2001 SE's Lobby, compilers that never touch the
control word) got none of the above, because no host format holds a
64-bit mantissa. Patch 48 keeps the stack as the x80 values themselves
(mantissa in an i64 global, sign and exponent in an i32) and does `+ - *
/` with exact 128-bit integer arithmetic, `*` and `fst m32` inline since
patch 49. Patch 47 is the one inexact switch, `x87-pc64-as-53`, off by
default: it runs 64-bit code at 53 bits, which drops the low 11 bits of
every mantissa, and no sampling of a program's operands can prove that
harmless. What runs at 64-bit precision in 3DMark2001 SE is its physics:
the *high detail* variants of the Lobby and the Car Chase simulate their
debris dynamically, the low-detail ones do not. So the pair — high
detail, switch off and on — measures what the inexact mode buys on a
physics-style workload, and the low-detail scene beside it is the control
that should not move (§6.2).

**SSE and MMX inline (patches 11, 12, 36, 39; doc 16).** SSE float
operations are inlined on the host vector unit when MXCSR is
round-to-nearest with no flush-to-zero or denormals-are-zero, all
exceptions masked, and the inexact flag already sticky — the state a game
is in a microsecond after starting. Each operation checks its operands
and result for the cases where host and softfloat could differ (NaNs,
denormals, the flags) with a vector compare and one branch; the slow
path is the old helper. Patch 12 inlines the MMX and SSE integer and
permutation instructions (a new TCG vector opcode, `tbl_vec`, for the
byte permutes). Patch 36 fixed a load stall: a 16-byte SSE memory operand
was loaded as two halves through `env` and reloaded as a vector, and
patch 39 made the lane check branch on the vector directly instead of
through a store and two loads. Measured on the SSE battery
(`tools/sse-guest-test.py`): packed 12x, scalar 3.6x per instruction on
the Air, results identical over 546,425 lines.

### 3.2 Memory: the TLB and string instructions (`tlb-floor`, `tlb-retire`, `rep-fast`)

**The TLB floor (patch 16).** QEMU resizes the software TLB at every
flush from the number of entries used since the previous one. Windows XP
flushes at every context switch (450 a second on an idle desktop), the
measured working set was a few hundred pages, and the table sat at 64–256
entries — where two live pages share an index all the time and every
access to either is a miss. The minimum is now 4096 entries.

**Retiring instead of flushing (patch 44).** Windows 98's memory manager
writes CR3 with the *same value* 2,400 times a second while 3DMark 99
runs (its map/unmap of one page flushes the whole TLB). Each flush cost
~185 page walks to refill entries nothing had changed. Now a CR3 write
re-validates the entries filled since the last flush against the guest
page tables and keeps the ones that still match: 95 % of the walks gone.

**REP MOVS/STOS as a host memcpy (patch 17).** Moto Racer's blit is one
`rep movsd`, 37 host instructions per dword with ESI/EDI/ECX going through
`env` at every iteration; Windows' own `memcpy` and `memset` are the same
instruction. The repeat now runs as a host `memcpy`/`memset` per page run
inside the guest's TLB rules, with the architectural state (the count and
the pointers at a fault) reconstructed exactly. 2.15 → 0.07 ns per
element; 536 cases of widths, direction flag, page crossings and overlaps
in `tools/rep-guest-test.py` agree with a Python model of the instruction.

### 3.3 Code that rewrites itself (`tb-invalidate-fast`, `smc-same-value`, `soft-imm`)

**Invalidation that finds its target (patches 15, 35).** Four cuts on the
path a store into a code page takes: the option-ROM TPR stubs of XP's
vAPIC sat below a data structure the APIC writes at every interrupt, and
a range rounded down to the page start retranslated them every time; the
per-page byte range of translated code was wrong for a Win9x module
(code at both ends, data between), so every data write walked the page's
whole TB list — 57 % of QEMU's time in 3DMark 99, invalidating nothing.

**Same-value stores (patch 18).** 94 % of the era's self-patching stores
write the value that is already there (a renderer re-patching the same
texture pointer). A store whose bytes equal the memory's bytes now
invalidates nothing.

**Soft immediates (patch 24).** For the other 6 %, a block that has been
thrown away by writes into its own immediates is retranslated with those
immediates and displacements emitted as *loads from the guest's code
bytes* rather than constants — the field lives where the guest writes
it, so the guest's store *is* the update and the block survives. A block
that still gets invalidated four times goes back to constants. In Blood's
starting room the translations in a 3 s window went from 117,254 to 34.

### 3.4 Control flow (`inline-lookup`, `jump-cache-keep`, `eob-chain`)

**The jump-cache probe inline (patches 20, 38).** Every `ret`, `call *`,
`jmp *` and every jump leaving its page ended its TB with a call to
`helper_lookup_tb_ptr`: a register sync, the CPU state's flags recomputed,
the breakpoint check, then the jump-cache compare — ~70 host
instructions plus the call, 13.9 % of the vCPU's self time on 7-Zip. The
probe is now TCG ops in the block, and patch 38 made the mode-flag half
of it constants known at translation.

**The jump cache survives a TLB flush (patch 42).** A CR3 write emptied
the jump cache too, and after each of Windows 98's 2,400 flushes a second
every indirect branch paid the hash table. Entries are kept (they are
keyed by physical code address and validated on use) and the cache grown.

**Chaining past non-jump block ends (patch 43).** After every `mov ds/es`
in 32-bit code, `sti`, `popf`, `iret` and every x87 or MXCSR control-word
change the translator ended the block and left for the main loop:
Windows 98's ring-0 entry alone was five round trips per VxD call, 1.2 M
main-loop entries a second in 3DMark. The main loop is only needed when
an interrupt was pending while IF was clear; everything else now chains.

### 3.5 Host-side overheads (`tls-hot-paths`, patches 14, 41)

**JIT write-protect toggles (patch 14).** macOS flips the MAP_JIT code
buffer between writable and executable per thread; QEMU toggled it before
every TB run from the main loop and around every patch without
remembering the state — 12 % of Super PI's vCPU thread. Tracked per
thread now.

**Thread-local storage (patch 19).** On macOS every `__thread` access is a
call into dyld; the store slow path took five nested RCU read locks per
store into a code page, 8.8 % of Moto Racer's vCPU. The hot paths read
the lock counters once.

**A 13.6 KB memset per translation (patch 41).** QEMU builds with
`-ftrivial-auto-var-init=zero`, and the translator's context struct had
grown to 13.6 KB with patch 06's slow-path blocks: 8.7 GB of memset in one
3DMark 99 run. The variable opts out.

### 3.6 Pinned guest registers (`pinned-regs`, patch 21, doc 18; opt-in)

`eip` and the eight general registers live in AArch64's callee-saved
x20–x28 for the life of a chain of TBs: loaded by the prologue, stored by
the epilogue, stored before a helper that may read them and reloaded
after one that may write them. It removes the loads and stores at block
boundaries. It is off by default because of one unexplained boot crash
with eight registers pinned (reproducible, §5.2) and a stall at the
flags-helper call boundary; both are open.

## 4. Methodology

### 4.1 Two tiers of numbers

A benchmark is **reproducible** here when the same program does the same
work every run, the run is started and timed by a script, and its output
can be checked. A game measurement is **ambiguous** in a way no harness
removes: the frame rate depends on where the camera points, on the
harness's clicks landing, on a frame cap, on which five seconds of a run a
number was read from. Both kinds are reported, in separate sections; a
game number carries its protocol, and every game window was checked
against a screendump of what was on screen when it was taken (§6.1).

### 4.2 The reproducible tier

Four programs, chosen because their hot paths are the ones the patch queue
touches, each with a fixed workload:

| Benchmark | What it exercises | Workload | Number |
|---|---|---|---|
| **nbench 2.2.3** (BYTEmark, `tools/specbench/build-guest.sh`) | ten self-timed kernels: seven integer (sort, bitfield, IDEA, Huffman, assignment, an FP *emulation* in integer code) and three x87 double (Fourier, neural net, LU decomposition) | each kernel runs until its rate is statistically stable | iterations/s per kernel; the headline is the geometric mean of the ten ratios to stock |
| **7-Zip 26.03**, `7zr b 2 -mmt1 -md=22` (public domain, [7-zip.org](https://www.7-zip.org/a/7zr.exe)) | LZMA compress and decompress, one thread, two passes at dictionary 22 (4 MB): integer, `ret`-heavy, memory-bound — the 7-Zip the control-flow patches were profiled on | its built-in benchmark | MIPS ratings (compress, decompress, total) |
| **Super PI mod 1.5 XS**, 1M digits (on the image) | x87 at 53-bit precision, the Windows default: the classic x87 workload, and the one patch 14 was profiled on | the 1M run, started by keys sent from the host | CPU seconds of the run, from the job object that contains the process, so the seconds spent waiting for its keys do not count |
| **SSEBENCH.EXE** (ours, guest-tools ISO, doc 16) | the handful of SSE and x87 kernels every Direct3D-era game does — vector transform, normalise, dot products, clamps and compares, int↔float conversions — with a checksum per kernel | `-iter 20` | ns per op per kernel; the SSE score is the mean of the SSE kernels |

nbench is cross-compiled for the guest with the guest tools' toolchain
flags (`-O2 -march=pentium3`, msvcrt, `-std=gnu89`): `-march=pentium3`
keeps `double` on the x87, which is what a 1998–2003 compiler produced.
The other three are binaries as shipped.

### 4.3 Running

`tools/specbench/run.sh` boots the project's own Windows XP image (SP3,
512 MB, `-cpu pentium3`, Cirrus VGA, `snapshot=on` so nothing is written)
once per emulator configuration, with the benchmark ISO as D: and a fresh
floppy as A:. It knocks on the Run dialog over QMP until `GO.BAT` has
copied the suite to C: and started `SPECRUN.EXE`
(`tools/specbench/specrun.c`), which runs each command through `cmd.exe /c`,
puts it in a job object, times it with `QueryPerformanceCounter`, reads the
job's CPU time, CRC-32s its standard output, and appends one line per run
to the floppy and to COM1. Super PI is a GUI program: the runner announces
each start on COM1 and the host sends the keys that pick 1M (`alt+c`, six
`down`, `ret`, `ret`); a batch file waits
for `pi_data.txt`, which Super PI writes when it is done. The short
programs run twice per boot and nbench once; the tables report the
**best** repetition, because macOS sometimes parks the vCPU thread on an
efficiency core for a run (a uniform ~2x, seen in the SSE work). The
guest's clock under TCG follows host time, so guest milliseconds are wall
milliseconds.

**Every emulator launch goes through `tools/specbench/noaslr.c`**, a
20-line launcher that spawns the process with address-space layout
randomisation off, because of the finding in §5.0: a third of launches of
the same binary ran helper-heavy code 35–45 % slower, TCG's code buffer
having landed 8 GiB from the helpers. With ASLR off the layout is the same
every launch and, on this machine, the near one, so the tables compare
like with like — for pristine QEMU too, which has no reservation of its
own.

Super PI's digits are CRC-checked across configurations (its output is a
computation); the other three print their own timings, so their CRCs are
recorded but not compared.

### 4.4 Configurations

| Name | Binary | Meaning |
|---|---|---|
| `stock` | pristine QEMU v9.2.4, same configure flags, no patches | the baseline the literature compares to |
| `stock-pic` | pristine, built with our `-fPIC` flags | the diagnostic that separates the cost of position-independent objects (ours are linked into the embed library too) from the cost of the patches |
| `off` | ours, all fifteen switches off | what the tree costs with nothing enabled: the patches that have no switch, and the switched patches' *off paths* |
| `default` | ours, as the launcher writes a machine | what a 2ksbox user runs |
| `no-<switch>` | default with one switch off | the contribution of that switch, as `default / no-<switch>` |
| `pinned` | default + `pinned-regs=on` | the opt-in register pinning |
| `pc64as53` | default + `x87-pc64-as-53=on` | the opt-in inexact mode; expected to change nothing here (nothing runs at 64-bit precision) |

### 4.5 Machine

Apple M1 (4 performance + 4 efficiency cores), 16 GB, macOS 26.6.2, Apple
clang 17 for QEMU, the tree at the commit that carries this document, QEMU
submodule v9.2.4. One guest at a time, and **nothing else running**: a run
taken beside other work on the machine was discarded and redone.

## 5. Results: the reproducible tier

### 5.0 First finding: where the code buffer lands decides a third of launches

Before the matrix could be run, the same binary had to give the same
number twice, and it did not. The same configuration came out at 0.65x to
0.73x of itself between launches — with byte-identical code, on an idle
machine that was not throttled (7-Zip's own host-speed calibration read
the same ~3100 MHz in fast and slow runs), not on an efficiency core and
not thermal — while both repetitions inside a launch always agreed to
0.5 %. The regime is decided at launch, and sampled launches lined up on
one variable: **where `mmap(NULL)` put TCG's 1 GiB code buffer**.

| launch | SSEBENCH SSE score (ns/op) | x87 kernel | code buffer | distance from the text |
|---|---|---|---|---|
| plain 1, 3, and the caught one | 13.00, 13.04, 13.04 | 44.7–45.1 | 0x3_0000_0000 | ~8 GiB |
| plain 2, 4 | 7.82, 7.92 | 33.7 | 0x1_0ca4…, 0x1_45dc… | 0.1–1.1 GiB |
| no ASLR 1–3 | 7.97, 7.80, 7.89 | 32–34 | 0x1_07ba…, 0x1_45b4… | 0.1–1.1 GiB |
| `stock` runs (Super PI, fast) | — | — | 0x1_074f…, 0x1_09e2… | 0.1 GiB |

Every slow launch had the code buffer 8 GiB from the executable; every
fast one had it within 1.1 GiB. The AArch64 backend emits a helper call
as one `bl` within ±128 MiB of the target, as `adrp+add+blr` within
±4 GiB, and beyond that as four `movz/movk` instructions and a `blr` —
and the same holds for every host address a translation block
materialises (the helper for an indirect jump, the exit constants). At
8 GiB every helper call in every TB is the long form, which is why the
slow launch's profile is the fast launch's profile scaled: the extra
instructions sit in generated code, in front of every helper, and a
sampler attributes their stall to the helper's first instruction. Whether
the buffer lands near or far is decided by how the guest RAM and the
libraries have fragmented the space below the dyld shared cache by the
time TCG initialises; with ASLR off the fragmentation happens to leave
the near gap free every time on this machine.

For the tables, every launch is pinned to slide 0 (`noaslr`). For the
product — and for upstream QEMU on Apple Silicon, which has the same
lottery — the fix is to reserve the buffer's address space next to the
helpers at load time, before anything else fragments it: a constructor in
`tcg/region.c` takes a `PROT_NONE` `MAP_JIT` reservation within 2 GiB of
its own image and `alloc_code_gen_buffer_anon` uses it (a hint to `mmap`
at TCG-init time is too late: by then the near windows are partly
occupied and macOS returns the next free gap, 8 GiB away). Its A/B
against pristine QEMU, six ASLR-on launches each:

| launch (ASLR on, SSEBENCH only) | pristine 9.2.4: SSE score, buffer | with patch 63: SSE score, buffer |
|---|---|---|
| 1 | 7.73 ns, +0.12 GiB | 7.81 ns, +0.11 GiB |
| 2 | 7.74 ns, +0.12 GiB | 7.69 ns, near |
| 3 | 7.81 ns, +0.12 GiB | 7.74 ns, +0.10 GiB |
| 4 | 7.76 ns, +1.01 GiB | 7.77 ns, +0.12 GiB |
| 5 | **12.99 ns, +8.01 GiB** | 7.73 ns, +1.12 GiB |
| 6 | **12.86 ns, +8.00 GiB** | 7.86 ns, +1.04 GiB |

Pristine went far in two launches of six here and in three of five in the
earlier probe series; with the reservation, none of six (and none of six
more paused launches whose maps were read directly: the reservation was
1 GiB in five and 512 MiB in one, always within 1.2 GiB of the image).

The next step past "near" was measured and is not worth taking: capping
the buffer at 120 MiB and reserving it directly above the image puts
every block within 128 MiB of every helper, so every call is one `bl`
instead of `adrp+add+blr`. Four launches each, ASLR on, the buffer's
distance read from the samples (107–127 MiB against 480–820 MiB):

| | SSE score (ns/op), four launches | x87 kernel (ms), four launches |
|---|---|---|
| 1 GiB reservation, `adrp+add+blr` | 8.15, 7.87, 7.75, 7.80 (mean 7.89) | 980, 907, 949, 924 (mean 940) |
| 120 MiB, every call a `bl` | 7.73, 7.74, 7.70, 7.62 (mean 7.70) | 956, 906, 1019, 945 (mean 957) |

About 2 % on the SSE helpers and nothing on the x87 ones, for a buffer an
eighth of the size: the five-instruction far form was the whole story,
and relocating helpers into `bl` reach, or a two-tier buffer, is not
worth building.

The patch is `patches/qemu/63-jit-buffer-near-helpers`. It has no
switch — it runs before the command line exists — so it is the one
performance patch in the queue whose A/B is a pristine build rather than
a property; its effect on a user's machine is the *removal* of a 35–45 %
loss on a third of launches, not a speedup of the median launch.

### 5.1 The headline

Best of two repetitions per boot (nbench once), every launch pinned to the
near-buffer regime, Super PI's digits identical in every row. Ratios are
against pristine QEMU 9.2.4; higher is better everywhere (the seconds and
nanoseconds columns are inverted for the ratio).

| configuration | Super PI 1M (s CPU) | 7-Zip (MIPS) | SSEBENCH (ns/SSE op) | nbench (geomean of 10 kernels) | geomean |
|---|---|---|---|---|---|
| `stock` (pristine 9.2.4) | 393.6 | 1413 | 7.84 | 1.00 | 1.00x |
| `stock-pic` (pristine, our `-fPIC` flags) | 393.0 (1.00x) | 1439 (1.02x) | 7.67 (1.02x) | 1.00 | 1.01x |
| `off` (ours, all switches off) | 423.2 (0.93x) | 1437 (1.02x) | 8.04 (0.98x) | 0.97 | 0.97x |
| **`default` (ours, as shipped)** | **74.8 (5.26x)** | **1573 (1.11x)** | **2.30 (3.41x)** | **1.50** | **2.34x** |

Three things to read off it. Position-independent code costs nothing
(`stock-pic`). The tree with every switch off is 3 % slower than
pristine: that is the whole cost of the switched patches' off paths plus
the unswitched patches, and it is all that a build with the switches
compiled out would buy back. And the default is 2.3x pristine on this
set: 5.3x on x87 code, 3.4x on the SSE kernels, 1.5x on nbench's mix,
1.1x on 7-Zip's integer code.

### 5.2 Ablation: the default with one switch removed

Each row is the default minus one switch. Some rows were taken on the
build before patch 63 (the same code without the reservation, every
launch pinned near by `noaslr`) and `default` was rerun on the patch-63
build as the control (74.4 → 74.8 s, 2.36x → 2.34x). Run-to-run noise on
this set is about ±2 %, so a difference under 3 % means nothing.

| switch off | Super PI (s) | 7-Zip (MIPS) | SSEBENCH (ns) | nbench | geomean | what moved |
|---|---|---|---|---|---|---|
| — (default) | 74.8 | 1573 | 2.30 | 1.50 | 2.34x | |
| `x87-fast` | **395.2** | 1587 | 2.42 | **1.17** | 1.44x | Super PI back to pristine (1.00x); nbench's three x87 kernels back to 1.0–1.07x |
| `sse-fast` | 74.7 | 1576 | **7.90** | 1.51 | 1.72x | the SSE kernels back to pristine (xform 1.06x, normalise 0.91x) |
| `simd-fast` | 74.5 | 1594 | 2.49 | 1.52 | 2.31x | MMX blend 4.66x → 2.45x, packed xform 6.2x → 4.6x |
| `rep-fast` | 76.0 | 1573 | 2.43 | **1.30** | 2.22x | nbench string sort 3.9x → 0.9x (its `memmove`) |
| `inline-lookup` | **81.5** | **1495** | 2.35 | 1.49 | 2.25x | Super PI −8 %, 7-Zip −5 % (every `ret` back through the helper) |
| `eob-chain` | **80.2** | 1569 | 2.35 | 1.50 | 2.28x | Super PI −7 % (its `fldcw`s leave for the main loop again) |
| `tb-invalidate-fast` | **80.5** | 1517 | 2.39 | 1.47 | 2.24x | Super PI −7 %, 7-Zip −4 % |
| `tls-hot-paths` | 75.3 | 1556 | 2.38 | 1.51 | 2.31x | within noise |
| `tlb-floor` | 75.1 | 1589 | 2.28 | 1.51 | 2.35x | within noise |
| `tlb-retire` | 74.8 | 1600 | 2.27 | 1.51 | 2.36x | within noise |
| `jump-cache-keep` | 74.5 | 1589 | 2.30 | 1.51 | 2.35x | within noise |
| `smc-same-value` | 74.8 | 1553 | 2.30 | 1.51 | 2.33x | within noise |
| `soft-imm` | 74.4 | 1547 | 2.31 | 1.51 | 2.33x | within noise |
| + `x87-pc64-as-53` (opt-in, inexact) | 74.3 | 1589 | 2.30 | **1.70** | 2.42x | nbench LU 3.0x → 5.3x, neural net 2.7x → 4.9x (§5.4) |
| + `pinned-regs` (opt-in) | — | — | — | — | — | XP crashed and rebooted during Super PI; no numbers (doc 18's open item; `run.sh <image> pinned` reproduces it) |

Six switches show nothing here, and that is expected: `smc-same-value`
and `soft-imm` exist for self-patching renderers, `tlb-floor`,
`tlb-retire` and `jump-cache-keep` for Windows 98's 2,400 CR3 writes a
second under a game, `tls-hot-paths` for the store slow path those
renderers hit — none of which a benchmark program does. Their evidence
is §6.

### 5.3 Per benchmark

The full per-kernel tables for every configuration are in
`docs/22-data/report.md`; the raw `RESULT` lines and each program's own
output in `docs/22-data/<configuration>/`. The rows that carry the story:

**nbench** (iterations/s, ratio to pristine):

| kernel | `default` | `no-x87-fast` | `no-rep-fast` | `pc64as53` | what it is |
|---|---|---|---|---|---|
| Numeric sort | 1.03x | 1.05x | 1.03x | 1.01x | integer |
| String sort | **3.93x** | 3.94x | 0.92x | 3.86x | `memmove`: the `rep movs` fast path |
| Bitfield | 1.00x | 1.01x | 1.01x | 0.98x | integer |
| FP emulation | 1.02x | 1.02x | 1.02x | 1.00x | integer (software float) |
| Fourier | **1.44x** | 1.07x | 1.18x | 1.42x | x87, transcendental-heavy (`fsin`/`fcos` stay helpers) |
| Assignment | 1.02x | 1.02x | 1.00x | 1.02x | integer |
| IDEA | 1.02x | 1.02x | 1.02x | 1.02x | integer |
| Huffman | 1.27x | 1.02x | 1.26x | 1.34x | integer with x87 in its inner loop |
| Neural net | **2.69x** | 1.02x | 2.21x | **4.92x** | x87 `exp` and multiply-accumulate |
| LU decomposition | **3.04x** | 0.99x | 2.98x | **5.35x** | x87 `+ - * /` at 64-bit precision |

**7-Zip** (MIPS): pristine 1235 compress / 1590 decompress; default 1458 /
1738 (1.18x / 1.09x). The integer, `ret`-heavy path gains from the three
control-flow patches together: without `inline-lookup` 1322 / 1668,
without `tb-invalidate-fast` 1305 / 1650, without `eob-chain` 1354 / 1716.

**SSEBENCH** (ns per op, ratio to pristine):

| kernel | `default` | `no-sse-fast` | `no-simd-fast` | `no-x87-fast` |
|---|---|---|---|---|
| packed transform (mul/add) | **6.2x** | 1.06x | 4.6x | 5.4x |
| packed normalise (`rsqrtps`) | **5.6x** | 0.91x | 4.4x | 5.3x |
| scalar chain (`ss` ops) | 1.19x | 1.00x | 1.19x | 1.13x |
| clamp + compare (min/max/cmpps) | **4.0x** | 1.01x | 3.9x | 3.8x |
| convert (`cvttss2si`/`cvtsi2ss`) | 2.85x | 0.98x | 2.85x | 2.84x |
| C transform, x87 | **11.9x** | 11.9x | 11.8x | 0.98x |
| C normalise, x87 `fsqrt`/`fdiv` | **11.5x** | 11.5x | 11.5x | 1.00x |
| denormal decay (the slow path) | **0.78x** | 0.92x | 0.78x | 0.80x |
| MMX blend | **4.7x** | 4.7x | 2.45x | 4.7x |

One row is a finding rather than a result: on the **denormal slow path**
the default is 22 % *slower* than pristine (59 vs 46 ns per op). That is
patch 11's inline path detecting a denormal result, undoing its work and
calling the helper pristine would have called directly — the price of the
check, paid on every operation of a kernel that is denormal on every
operation. Real code meets denormals in decays and fades, briefly; a
program that lived there would be better off with `sse-fast=off`. The
scalar-chain row (1.19x) is the other modest one: a dependent chain of
`addss`/`mulss` gains little from inlining because it is
latency-bound either way.

### 5.4 Reading the numbers

- **The x87 patches are the largest single lever on this tier**: Super PI
  5.3x, the x87 kernels 11.5–11.9x, and they are exact (identical digits in
  every configuration). They cover 53- and 24-bit precision; nbench turned
  out to run at 64-bit precision, because mingw's C runtime sets the x87
  control word to extended precision at startup (MSVC's sets 53 bits), so
  its three float kernels are the first measurement of patches 48/49's
  exact 64-bit path: LU 3.0x, neural net 2.7x, Fourier 1.44x over
  pristine. The opt-in inexact mode (`x87-pc64-as-53`) takes the same
  kernels to 5.3x and 4.9x — the same shape as 3DMark2001's high-detail
  physics (§3.1, §6.2), and the same bargain: faster, and a different low
  mantissa.
- **SSE inlining is worth 3.4x on the SSE kernels** and its two halves
  separate cleanly: `sse-fast` carries the float ops, `simd-fast` the MMX
  and packed permutes (MMX blend 4.7x vs 2.45x without it).
- **The control-flow patches are worth 10–20 % on integer code**:
  `inline-lookup`, `eob-chain` and `tb-invalidate-fast` each cost Super PI
  7–8 % and 7-Zip 4–5 % when removed, which is the shape the M9 profile
  predicted (the `ret` lookup at 14 % of 7-Zip's vCPU time, the x87 control
  word's block ends in Super PI).
- **`rep-fast` is one benchmark's whole gain**: string sort 3.9x, the only
  kernel that is a `memmove`.
- **The switches' off paths cost 3 %**, and the cost is in the x87 and SSE
  translators' mode checks (Super PI 0.93x, SSEBENCH 0.98x, 7-Zip 1.02x):
  small enough that a single switchable build is the right product.
- **Not visible here, by construction**: six switches (§5.2, §6.3).
- **What is still slower than pristine**: the denormal slow path (0.78x,
  §5.3), and nothing else on this tier.

## 6. The games

The patches were written against games, and that is where the largest
gains are — because a Windows 98 game is not a benchmark program: it
patches its own code, it spends a quarter of its frame in x87 at 24-bit
precision, it flips the TLB 2,400 times a second through the VMM, and it
enters ring 0 five times per VxD call.

### 6.1 Protocol

Every game on one machine, `base98-us` (Windows 98 SE, our display driver,
`-cpu pentium3`, an SB16), on the tree of §5 with patch 63 in, each run on
a fresh raw copy of the image under a bare `qemu-system-i386`, one guest
at a time, driven headless by a runner per game (`tools/w98-3dmark.sh`,
`w98-3dmark2001.sh`, `w98-blood.sh`, `w98-moto.sh`, `w98-quake2.sh`; the
CLAUDE.md table has each one's protocol). The A/B is the matrix's own:
**every switch off** against **the default** — pristine QEMU cannot be
the baseline here, since none of these runs without the paravirtual
adapter. What each runner measures:

- **3DMark 99 Max** (800×600×16, triple buffer, Pentium III
  optimizations): the Benchmark clicked, a screendump every 5 s, and the
  executor's 5 s frame-rate windows placed by test from the screendumps —
  a window counts for a test only when it lies wholly inside that test's
  shots, because a window that spans a loading screen reads as a frame
  rate that is not one. The two game tests run at a 60 Hz flip cap, so
  they are taken with the vertical blank off as well.
- **3DMark2001 SE** (1024×768×32, DXTC, pure hardware T&L): the
  Benchmark, and the per-test frame rates as 3DMark itself reports them
  in its Details dialog, paged and screendumped. Each game runs at low
  and high detail; every low-detail scene sits at the 60 Hz cap, and the
  high-detail Car Chase and Lobby are the inexact switch's benchmark
  (§3.1), so the same run is taken a third time with `x87-pc64-as-53`.
- **Blood** (DOS Build engine in a Win98 DOS box, 640×480 VESA):
  `-quick -map e1m1` puts the first level up with no menu to drive; the
  game flips pages by writing the VBE display start, so QEMU's trace of
  those writes per second is its frame rate. No frame cap; the view is
  the level's first, unmoved.
- **Moto Racer 1997** (640×480): the menus walked to a practice race on
  Speed Bay, Direct3D turned off in the game's options so the game's own
  software renderer runs (the workload the self-modifying-code patches
  were found on), the throttle held from the standing start, and the rate
  read from the driver's flip chain, vertical blank off.
- **Quake II 3.20** (640×480, `ref_soft`): `timedemo 1` on demo1, the
  game's own `frames, seconds: fps` line. A compiled software renderer
  that patches nothing — the control for the claim that the large game
  gains come from the self-modifying-code and VMM patches.

Every number was checked against a screendump of what was on screen when
its window was taken — the 3DMark 99 game tests by their own in-frame
counters, 3DMark2001's by its "Now Testing" splash and in-frame counter,
Blood and Moto Racer by the frame itself. The screendumps and the raw
rate lines are under `docs/22-data/games/`.

### 6.2 Results

| Workload | all switches off | default | default + `x87-pc64-as-53` | Note |
|---|---|---|---|---|
| **3DMark 99 Max**, Game 1 (race), vertical blank off | 39.3 fps | **89.2 fps** | — | 800×600×16, triple buffer, Pentium III optimizations; the executor's windows wholly inside the test's shots (`tests.txt`); 3DMark's own counter in the shots: 43–45 / 104 |
| 3DMark 99 Max, Game 2 (first person), vertical blank off | 35.0 fps | **79.6 fps** | — | counter in the shots 26–29 / 67 (instantaneous, the window is a 5 s mean) |
| 3DMark 99 Max, 3DMarks / CPU 3DMarks, vertical blank off | 3901 / 5728 | **8686 / 16609** | — | with the vertical blank on both game tests sit at the 60 Hz cap (59.8 / 59.2 fps) and the score is 5987 / 16614 |
| **3DMark2001 SE**, Game 1 Car Chase, low / high detail | 29.5 / 1.3 fps | 59.0 / **16.9** fps | 59.7 / **24.8** fps | 1024×768×32, DXTC, pure hardware T&L; 3DMark's own per-test figures from its Details dialog; low detail is at the 60 Hz cap |
| 3DMark2001 SE, Game 2 Dragothic, low / high | 53.1 / 28.4 | 60.7 / **52.9** | 60.0 / **56.1** | |
| 3DMark2001 SE, Game 3 Lobby, low / high | 41.5 / 14.9 | 60.1 / **37.3** | 60.0 / **42.9** | the high-detail debris physics is x87 at PC=64 (§3.1): +15 % from the inexact switch here, +47 % in the Car Chase |
| 3DMark2001 SE, Game 4 Nature | 51.5 | 64.0 | 60.1 | |
| 3DMark2001 SE, 3DMark score | 3163 | **5222** | 5476 | run to run ±3 % (a first default run scored 5072) |
| **Blood** (DOS Build engine in a Win98 DOS box), the crypt's first view | 4.2 fps | **127.9 fps** | — | 640×480 VESA; frames counted as the game's VBE display-start writes over 80 s; no frame cap |
| **Moto Racer 1997**, software renderer, the race, vertical blank off | 2.9 fps | **83.5 fps** | — | 640×480, Direct3D turned off in the game's Options; page flips through the driver's chain over 21 s of throttle; the two runs are not at the same point of the lap (the slow one covers 12 s of race in 21 s) |
| Moto Racer 1997, Direct3D (our HAL), vertical blank on | — | 59.7 fps | — | the flip cap, for the record |
| **Quake II** 3.20, software renderer, `timedemo 1` demo1 | 34.8 fps | **50.0 fps** | — | 640×480 (`ref_soft`, mode 3); the game's own `689 frames, 13.8 seconds: 50.0 fps` line; a compiled software renderer with no self-modifying code, so the CPU tier's kind of gain (1.4x) |

The switches are worth 2.3x on the CPU tier and **2.2x to 30x on the
games** — 3DMark 99's game tests 2.3x, 3DMark2001's high-detail scenes
1.9–13x, Blood 30x, Moto Racer's software renderer 29x — because the
game-only patches (§6.3) are exactly what the "all off" column loses.
The two game tests of 3DMark 99 and every low-detail scene of 3DMark2001
sit at the 60 Hz flip cap on the default build, which is the number a
user sees; the uncapped figures are what is left in hand. **The inexact
PC=64 switch** (patch 47) is worth +47 % in the scene that is all debris
physics (the Car Chase, high detail), +15 % in the Lobby and +6 % in
Dragothic, and nothing where there is none — the low-detail controls are
at the cap with it off and on, as they should be.

### 6.3 What the two tiers say together

Where the tiers agree is direction: the same tree gains on both, and
Quake II — compiled code, no self-patching, the one game whose renderer
is a program of the benchmarks' kind — gains the benchmarks' 1.4x. Where
they disagree is scale, and the reason is structural. Three of the largest patches are invisible to any
fixed-workload CPU benchmark by construction: self-modifying code (§3.3)
never happens in a compiled benchmark; the CR3 storm and the ring-0
round trips (§3.2, §3.4) are properties of Windows 98's VMM under a
game's allocation pattern, not of user-mode code; and x87 at 24-bit
precision (patch 45) is a Direct3D device's state. So `smc-same-value`,
`soft-imm`, `tlb-floor`, `tlb-retire`, `jump-cache-keep` and
`tls-hot-paths` show nothing in §5.2 and are the difference between
4 fps and 128 in Blood, between 3 fps and 84 in Moto Racer. A benchmark
number for this tree is therefore a lower bound on what a 2ksbox user
sees — and the literature's SPEC numbers for hardware-MMU designs (§7)
are, symmetrically, taken on workloads that never touch the paths those
designs make slower (page-table churn).

### 6.4 Threats to validity

- **One host.** Everything is one M1 Air on one macOS. The same games on
  the project's x86-64 machine are still to be taken.
- **The games' baseline is "every switch off", not pristine QEMU**: none
  of them runs without our adapter. §5 says the off paths are 3 % slower
  than pristine on the CPU tier, so the games' ratios overstate the gain
  over upstream by about that much.
- **A game's frame rate depends on where the camera is.** Each runner
  fixes the view it can (3DMark's tests are fixed animations, Blood's is
  the first view of the level, Quake II's is a demo); Moto Racer's is a
  throttle held from the standing start, and the slow run covers less of
  the lap in its window.
- **The tables are the near-buffer regime** (§5.0). A launch of a QEMU
  without the reservation is that regime about two times in three; the
  rest is 0.55–0.9x of these numbers on helper-heavy code, for pristine
  QEMU and for ours alike.
- **The guest compiler is not the era's.** MSVC 6 and Watcom emitted more
  x87, fewer SSE, and different call shapes than GCC 16 at `-march=pentium3`.
  The direction of every effect should carry over; the magnitudes will
  not. The games are the era's binaries and do not have this problem.
- **Small workloads.** A minute or two per program under TCG, chosen so a
  configuration fits in ten minutes; nothing here has a working set beyond
  the caches except 7-Zip's dictionary.
- **Two repetitions, minimum reported.** The minimum is the right
  estimator for the efficiency-core hazard but hides variance; the raw
  `results.txt` files keep both. The games are one run each (3DMark2001's
  score moves ±3 % between two default runs).
- **Stock QEMU's firmware is unpatched** (`prepare-qemu.sh` stamps the BIOS
  date and swaps the VGA BIOS in ours); neither touches a running XP.
- **The CRC check covers Super PI's digits** (and the appendix suite's
  outputs); nbench, 7-Zip and SSEBENCH check themselves (a checksum per
  kernel in SSEBENCH, CRCs of every block in 7-Zip's benchmark, which
  aborts on a mismatch). The games check nothing but the screen.
- **Super PI is timed by CPU time**, not by its own stopwatch; the two
  agree to within its startup, but the number is not the one the program
  displays.

## 7. Related work

The techniques in §3 have cousins in the dynamic-binary-translation
literature. The literature was not consulted before they were built: the
patches were written from profiles of the workloads (by Claude, the AI
assistant that did the implementation work in this project), and the
survey was made afterwards, to find what had been done before and what
prior art the parked items have (`docs/23-dbt-literature.md` has the full
list with links and the ranking). Where a patch and a paper coincide,
that is convergence, not citation:

- **Hardware-MMU-backed guest memory** — ESPT (Chang et al., VEE 2014) and
  HSPT (Wang et al., VEE 2015, 1.98x on QEMU) embed a shadow page table in
  the translator's address space so a guest access is a host load at a
  fixed offset; Captive (Spink, Wagstaff, Franke; TACO 2016, ATC 2019)
  runs the translator in ring 0 inside a KVM virtual machine with the
  guest MMU on nested paging (2.5x average, 5.88x peak over QEMU on SPEC
  CPU2006); Rodzevich et al. (ISPRAS 2024) do it for an AMD64 guest on an
  AArch64 host, our pairing; Poletaev and Dovgalyuk (ISP RAS 2025) inside
  QEMU with an mmap view. We measured the same design on
  Hypervisor.framework (1.4–2.6x per load, `tools/hvf-el1/`) and parked
  it as weeks of work; patches 16 and 44 are the cheaper cuts at the same
  cost — and §8.2 prices the design on our workloads at 1.1–1.2x.
- **Indirect branches** — SPIRE (Jia et al., VEE 2013), MAMBO-X64's
  hardware return-address prediction (D'Antras et al., PLDI 2017) and
  Tiaozhuan's full address mapping (Li et al., TACO 2024; +3.9 % average,
  19.4 % peak on SPEC with an x86 guest). Patch 20 inlines QEMU's own
  jump-cache probe; those go further, and §8.1 tried them: the table
  within noise, the return prediction a loss.
- **Global register allocation** — Zurstraßen et al. (DATE 2025, static
  guest-to-host mappings, up to 1.4x over block-local) and Batuzov (ISP
  RAS) in QEMU. Patch 21 is the same idea on AArch64's callee-saved set.
- **Floating point on the host** — Cota's hardfloat (QEMU, 2018: use the
  host when the inexact flag is already sticky) is the rule patches 06
  and 11 apply, extended to x87 and to keeping values in host format
  across instructions; Zurstraßen et al. (RAPIDO 2025) vectorise the
  same checks for RISC-V.
- **Self-modifying code** — Transmeta's Code Morphing Software (Dehnert et
  al., CGO 2003) kept translations that verify their own source bytes on
  entry; patch 24's soft immediates make the block read the bytes
  instead.
- **Interrupt checks** — Niu, Zhang and Li (SYSTOR 2022) remove the per-block
  pending-interrupt test; patch 43 removes the block ends that forced the
  main loop.
- **On arXiv**: learned translation rules in system-mode QEMU (2402.09688,
  1.36x), function offload to native code (2512.00487), IR-less
  translation (2501.03427).

## 8. What the remaining ideas are worth

The related work of §7 leaves two kinds of candidate: techniques the queue
has not tried, and the one design it parked for size. Both were priced on
the same tier on 2026-09-16 (the full account is doc 23's last section and
the M9 track doc's "Gauging the gain"; the diffs, scripts and runs are
under `docs/22-data/spikes/` and `docs/22-data/hwmmu/`).

### 8.1 The techniques, each tried

Each was implemented behind its own `-accel tcg` switch on one build,
exact (every battery of the guest test stage passes; Super PI's digits
identical), and measured against a `default` run of the unchanged binary
taken the same morning. Noise on this tier is ±2 %.

| candidate | Super PI (s) | 7-Zip (MIPS) | SSEBENCH (ns) | verdict |
|---|---|---|---|---|
| control (the tree as committed) | 75.2 | 1553 | 2.37 | |
| jump cache indexed by the pc itself (Tiaozhuan's full address mapping: 32 GiB reserved, populated on demand) | 73.5 | 1588 | 2.30 | within noise |
| the exit-request check at back edges and indirect branches only (Niu et al.) | 74.3 | 1583 | 2.29 | within noise |
| return prediction: a return-address ring, `call` through a host `bl`, `ret` through a host `ret` (MAMBO-X64), on top of both | 75.5 | 1574 | 2.45 | a loss, 2–6 % |
| every SSE result check removed — inexact, the ceiling of any cheaper check (RAPIDO) | | | 2.17 | ≤ 6 % of the SSE score, none of it in the packed kernels |
| pinned registers capped at seven (the DATE 2025 cross-check of patch 21) | XP rebooted in Super PI | | | the crash is the pinned path, not the eighth register |

The two structural ones show nothing because the control-flow patches
already took the part that mattered on an out-of-order host: the hash the
jump table removes was not on the critical path of a probe that ends in
five dependent loads, and the per-block interrupt check is an L1 load
the core never waits for. Return prediction loses because the ring push
at every call and the compare at every `ret` cost more than the
hardware's prediction of a `ret` saves — which says the M1's indirect
predictor was already right about most `br` targets. The back-edge check
also taught something about the tree: patch 34 delivers the PIT's overdue
interrupt on the `in` instruction and relied on the next block's check to
take it before the next guest instruction; a block ended by an I/O
instruction therefore keeps the check.

### 8.2 The hardware-MMU design, priced without building it

The design (the M9 track doc): run TCG's output inside a
Hypervisor.framework VM whose stage-1 page tables mirror the x86 guest's,
so a guest load is one host load instead of the softmmu chain — the
Captive / ESPT / HSPT family of §7, 2–2.5x in their SPEC numbers. A probe
had priced its primitives (1.4–2.6x per load); what a workload would gain
was inferred from the profile's 43 % of samples on the chain. Two
measurements replaced the inference:

- **a memory census of each workload** (`tools/hwmmu/census.c`, a TCG
  plugin): instructions, loads, stores, the distinct 4 KiB pages touched
  per second and per window of 65,536 accesses (the nested TLB holds
  3072), each access's page reuse distance, and first touches and first
  writes (a mirror fill and a dirty upgrade each);
- **workload-shaped kernels in the probe**: an independent load with
  four or twelve ALU ops behind it, and a load-plus-store pair, as
  today's softmmu sequence and as the mirrored access, at working sets
  from 64 KiB to 32 MiB.

| workload | acc / insn | stores | pages per 64K-access window, mean / max | windows over 3072 | accesses re-touching a page within 64 accesses |
|---|---|---|---|---|---|
| Super PI 1M | 0.55 | 38 % | 70 / 1,600 | 0 of 449,891 | 97 % |
| 7-Zip | 0.34 | 34 % | 240 / 1,150 | 0 of 286,139 | 97 % |
| nbench | 0.29 | 34 % | 8 / 926 | 0 of 570,340 | |
| Quake II timedemo (Win98, software renderer) | 0.35 | 45 % | 52 / 334 | 0 of 605,108 | |
| Blood (Win98 DOS box) | 0.32 | 16 % | 94 / 367 | 0 of 242,234 | |

With ALU work to hide behind, the chain costs 0.3–0.6 ns per access
rather than the 2–5 ns of a dependent chase, and a store is the better
half (a plain `str` against a chain of its own). Charging each workload's
full-speed access rate those differences, each access on the kernel row
its own reuse distance puts it on:

| workload | M accesses / s | projected gain |
|---|---|---|
| 7-Zip | 483 | 1.20x |
| Super PI 1M | 392 | 1.16x |
| Quake II timedemo | 372 | 1.16x |
| Blood | 257 | 1.10x |
| nbench, SSEBENCH | 146–188 | 1.05–1.07x |

The design's one risk — random working sets beyond the nested TLB's
12 MiB, where the probe measured ~21 ns per miss — does not occur: over
2.3 million windows of 65,536 accesses on six workloads, none touched
more than 1,600 distinct pages. The projection leaves out what would also
change under the mirror (TLB refills at 40 k/s and flushes at 500/s on
this tier, XP's 3.9 % idle in CR3 switches, the 2,400 CR3 writes a
second of a Windows 98 game, whose largest costs patches 42 and 44
already took), and it cannot separate dependent-address accesses, so
7-Zip's number is a floor. The literature's 2–2.5x on SPEC does not
transfer to these programs, and the reason is visible in the census: a
1998 game or a 2003 benchmark re-touches its pages within a few dozen
accesses, so the softmmu chain it pays is the throughput of nine
overlapped instructions, not the latency of a dependent walk.

**Decision (2026-09-16).** A fifth, for a freestanding build of the
vCPU core, a rewrite of `cputlb.c` as a fault-driven mirror and a mailbox
protocol between the VM and the device model, is not worth it at the
speed the tree already reaches: the design is abandoned for the time
being, with the census, the kernels and the projection kept in the tree
for the day a workload changes the number.

## 9. Conclusion

The queue does what it was written to do, and the matrix says by how much
on the programs the literature would measure: 2.3x geometric mean over
pristine QEMU 9.2.4 on the Air, 5.3x on x87 code, 3.4x on SSE, 1.1x on
plain integer code, with identical outputs. The ablation attributes it:
the floating-point patches carry the bulk, the three control-flow patches
10–20 % on integer code, `rep-fast` one kernel — and six switches show
nothing there because their workloads are Windows 98 games, where the
same tree is 2.2x to 30x over itself with every switch off: 3DMark 99's
game tests 2.3x, 3DMark2001's high-detail scenes 1.9–13x, Blood 30x, Moto
Racer's software renderer 29x, and Quake II's compiled renderer the
benchmarks' 1.4x. The one inexact mode buys 47 % on the scene that is all
64-bit physics and nothing where there is none.

The evaluation's own first result is the one an upstream reviewer would
want: on macOS/arm64 a third of QEMU launches put the code buffer 8 GiB
from the helpers and run helper-heavy guest code at 0.55–0.65x, pristine
or patched, and a load-time reservation next to the image (patch 63)
removes it.

And the remaining ideas are priced (§8): the literature's untried
techniques each move this tier by less than its noise or lose, and the
hardware-MMU design that the field's SPEC numbers make look like 2x is
worth 1.1–1.2x on these programs, because their accesses re-touch a page
within a few dozen accesses and pay the softmmu chain as overlapped
throughput rather than dependent latency. On the workloads this project
runs, the tree is at the point where what is left to gain generically is
a fifth at most, and the decision was to stop here.

Open, in order: the games of §6 on the project's x86-64 machine; the
x86-64 form of the placement question there, where the far form is the
always case; patch 21's crash, reproducible by `tools/specbench/run.sh
<image> pinned`, set aside; the denormal slow path (§5.3); and the "all
off plus one switch" family for a reader lifting a single patch.

## Appendix A. The SPEC-CPU2006-derived suite (measured once, not the headline)

SPEC CPU2006 is what the literature reports, it is licensed, and SPEC
retired it in 2018, so `SPEC=1 tools/specbench/build-guest.sh` builds
what it was made of: the open-source ancestors of five CINT2006
benchmarks (bzip2 1.0.8 ← 401.bzip2, GNU Go 3.8 ← 445.gobmk, HMMER 2.3.2
← 456.hmmer, Sjeng Free 11.2 ← 458.sjeng, libquantum 1.1.1 ←
462.libquantum), cross-compiled with the same flags, with fixed inputs and
the same small fixes SPEC made (a pinned seed, no learning file, no stdin
polling), every output CRC-checked. Measured on 2026-09-15 on the Air,
seconds, best of two:

| config | bzip2 -9 | bzip2 -d ×3 | shor | hmmer | gnugo | sjeng | geomean vs stock |
|---|---|---|---|---|---|---|---|
| stock 9.2.4 | 10.2 | 6.1 | 105.8 | 27.7 | 17.2 | 15.5 | 1.00x |
| ours, all off | 10.6 | 8.3 | 110.6 | 29.8 | 23.5 | 17.2 | 0.88x |
| ours, default | 9.9 | 5.5 | 103.0 | 27.3 | 13.2 | 14.9 | **1.07x** |
| default, `x87-fast` off | 9.7 | 5.2 | 106.5 | 27.5 | 21.5 | 14.6 | 1.00x |
| default, `sse-fast` off | 9.7 | 5.9 | 102.9 | 27.4 | 13.2 | 14.7 | 1.06x |

Every configuration produced identical output. The reading: compiled
integer code gains a uniform few percent (the TLB floor, the inline
lookup, the chained block ends), GNU Go 1.30x because it uses floating
point in its influence functions, and the rest of the queue never runs.
These numbers say little about the patches, which is why they are an
appendix; they are kept for the one question they do answer — what an
upstream reviewer would see on their own benchmark — and for the `off`
row, the first evidence that the switched patches' off paths are slower
than upstream's code (0.88x; GNU Go 0.73x, the x87 path at
`x87-fast=off` slower than stock's, §5).

## Appendix B. Reproducing

```sh
scripts/build.sh                                   # our QEMU (build/qemu)
# pristine baseline: a worktree of the submodule at v9.2.4, same configure flags
git -C qemu worktree add --detach build/qemu-stock-src v9.2.4
(cd build/qemu-stock && ../qemu-stock-src/configure --target-list=i386-softmmu \
   --disable-werror <the --disable flags of scripts/configure-qemu.sh> && ninja qemu-system-i386)
tools/specbench/build-guest.sh                     # the suite → build/specbench/sb.iso (SPEC=1 adds appendix A's)
tools/specbench/run.sh ~/vms/winxp.qcow2 all       # ~2 h; build/specbench/runs/
tools/specbench/report.py build/specbench/runs --md
# the games (base98-us; each runner's env in the CLAUDE.md table): the default,
# then the same with every switch off, then the inexact switch where it applies
FRESH=1 TABLET=0 IMG=<base98-us disk> TDM_DIR='\PROGRA~1\3DMARK~1' DDFLAGS=32768 tools/w98-3dmark.sh 99-def-nv
FRESH=1 tools/w98-3dmark2001.sh 2001-def; FRESH=1 CPU=pentium3,x87-pc64-as-53=on tools/w98-3dmark2001.sh 2001-pc64
tools/w98-blood.sh blood-def; SOFT=1 DDFLAGS=32768 tools/w98-moto.sh moto-soft-nv; tools/w98-quake2.sh q2-def
# every switch off: CPU="pentium3,x87-fast=off,sse-fast=off,simd-fast=off,rep-fast=off"
#   QEMU_TCG_OPTS="smc-same-value=off,soft-imm=off,inline-lookup=off,tb-invalidate-fast=off,tlb-floor=off,tls-hot-paths=off,jump-cache-keep=off,eob-chain=off,tlb-retire=off"
```

## Appendix C. Raw result lines

The raw lines of every configuration — `RESULT` lines with wall and CPU
milliseconds, CRC and exit code per repetition, and each program's own
output — are committed under `docs/22-data/<configuration>/`, with the
report as `docs/22-data/report.md`; the A/B of patch 63 under
`docs/22-data/runs-ab3/` and the 120 MiB experiment under
`docs/22-data/runs-exp120/`. The `pinned` directory has the screendump of
the rebooting guest. The games are under `docs/22-data/games/<run>/`: the
rate lines, the per-test placement (`tests.txt`), 3DMark's score and
Details screendumps, Blood's per-second counts, the game frames the
numbers were checked against, and Quake II's console log. §8's material
is under `docs/22-data/spikes/` (the spike diff, the scripts that produce
it, every run's result lines, the rebooted `pinned-7` guest) and
`docs/22-data/hwmmu/` (the census of every workload, the reuse-distance
census, the probe's clean run). The profiler
samples (one per configuration, inside Super PI) are not committed:
`build/specbench/samples/` on the Air.


