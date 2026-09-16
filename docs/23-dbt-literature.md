# 23. The parked optimizations against the literature (survey of 2026-09-15)

A user request: look at what we optimized (`patches/qemu/README.md`, docs
13, 16, 18, the M8/M9 track docs), at what we could *not* do or parked,
and search the literature — arXiv first — for the ideas behind those
parked items. This is the result, kept here so the next session on TCG
performance starts from it rather than searching again. Doc 22 §7 is the
short form.

**Order of events, for the record.** None of this was read before the
patches were written. The queue was built from profiles of the workloads
by Claude, the AI assistant that did the implementation work, from what it
already knew; this survey came afterwards. Where a patch matches a paper
below, the two arrived at the same place independently.

**Coverage note.** arXiv carries almost nothing on dynamic binary
translation: a full-text search for "dynamic binary translation" returns
nine papers, of which three touch our problems. The field publishes at
VEE, CGO, TACO, ASPLOS, PLDI, DATE and ISP RAS, so most entries below are
venue papers. The arXiv-native ones are marked.

## What we parked, and who has worked on it

### 1. The softmmu TLB chain

Our profile puts the four-load TLB lookup at ~43 % of generated-code
samples on integer code (7-Zip). The M9 track designed and probed the
structural fix — run TCG's output inside a Hypervisor.framework VM at EL1
with the guest's x86 page tables mirrored into stage 1, so a guest load
is a host load — measured every primitive on the Air (1.4–2.6x per load,
helpers must run inside the VM, one risk: the nested-TLB penalty on
working sets beyond ~12 MiB) and parked it at "4–8 weeks". The
literature has built that design four times:

- **Captive** — Spink, Wagstaff, Franke, *Hardware-Accelerated
  Cross-Architecture Full-System Virtualization*, [TACO 2016](https://dl.acm.org/doi/10.1145/2996798);
  the retargetable version *A Retargetable System-Level DBT Hypervisor*,
  [USENIX ATC 2019](https://www.usenix.org/system/files/atc19-spink.pdf).
  The DBT runs in ring 0 inside a KVM VM, the guest MMU is mapped onto
  nested paging, device I/O and interrupts cross the VM boundary. ARM
  guest on x86 host: 2.5x average, 5.88x peak over QEMU on SPEC CPU2006.
  The closest published relative of our probe's verdict, including the
  device/interrupt half our probe left as a mailbox protocol.
- **Rodzevich, Batuzov, Koltunov, Cheremnov, Shlyapin**, *Efficient MMU
  Emulation in Case of Cross-ISA Dynamic Binary Translation*,
  [ISPRAS 2024](https://ieeexplore.ieee.org/document/10899135/): an
  AMD64 guest on an AArch64 host with shadow page tables built for the
  Arm MMU. Our exact ISA pairing, the only paper with it.
- **Poletaev, Dovgalyuk, Teys, Kostin**, *Hardware acceleration of Qemu
  MMU for aarch64 on x86-64 full system emulation*,
  [Proc. ISP RAS 37:6 (2025)](https://www.mathnet.ru/php/archive.phtml?wshow=paper&jrnid=tisp&paperid=1089&option_lang=eng):
  inside QEMU, an extra mmap view of the guest virtual address space so
  translated loads use a fixed offset instead of the TLB lookup; the
  abstract's speedup figure is truncated at "271" (read: up to ~2.7x).
- **ESPT** — Chang, Wu et al., *Efficient Memory Virtualization for
  Cross-ISA System Mode Emulation*, [VEE 2014](https://dl.acm.org/doi/10.1145/2576195.2576201)
  (a kernel module); **HSPT** — Wang et al., *Practical Implementation
  and Efficient Management of Embedded Shadow Page Tables for Cross-ISA
  System Virtual Machines*, [VEE 2015](https://www.semanticscholar.org/paper/HSPT:-Practical-Implementation-and-Efficient-of-for-Wang-Li/fa8e9614381ed39e6b978c691aa81e5365984a20)
  (mmap only, 1.98x average on QEMU, ARM on x86-64).

Their numbers agree with our probe's per-load gain, which is the
strongest confirmation the design gets. The user-space mmap variants are
closed to us on the Air: macOS maps at 16 KiB and the guest's pages are
4 KiB; only the VM has the 4 KiB granule (the probe measured `TGran4=0`).
**Read Captive and the 2024 ISPRAS paper before reopening the track.**

Cheaper cuts at the same cost that we did take: patch 16 (TLB floor) and
patch 44 (retire instead of flush). Also relevant: Tong, Koju, Kawahito,
Moshovos, *Optimizing Memory Translation Emulation in Full System
Emulators*, [TACO 2015](https://dl.acm.org/doi/10.1145/2686034) — SoftTLB
resizing, a victim TLB (which QEMU adopted), helper threads to flush.

### 2. Indirect branches past patch 20

Patch 20 inlines QEMU's jump-cache probe; what remains is a hash, a
compare, and the conflicts of a 4096-entry cache (11.7 M hash-table
lookups per 10 s in 3DMark 99 before patch 42).

- **Tiaozhuan** — Li, Guo, Lan, Xue, Han, Niu, Zhang, *A General and
  Efficient Indirect Branch Optimization for Binary Translation*,
  [TACO 2024](https://dl.acm.org/doi/10.1145/3703355): a table indexed
  directly by guest PC in a large reserved address space ("full address
  mapping"), so a lookup is 1–2 host instructions, plus
  "exception-assisted branch elimination" for the correctness check.
  In LATX (x86 guest on LoongArch): +3.9 % average, +19.4 % peak on SPEC
  CPU2006. For a 32-bit guest the table is 32 GiB of lazily populated
  virtual address space.
- **MAMBO-X64** — D'Antras, Gorgovan, Garside, Luján, *Low Overhead
  Dynamic Binary Translation on ARM*, [PLDI 2017](https://research.manchester.ac.uk/en/publications/low-overhead-dynamic-binary-translation-on-arm/),
  and *Optimizing Indirect Branches in Dynamic Binary Translators*,
  [TACO 2016](https://dl.acm.org/doi/10.1145/2866573): translated
  call/ret pairs that push host addresses so the hardware return-address
  predictor works; `ret` is most of our indirect branches.
- **SPIRE** — Jia et al., *SPC-Indexed Indirect Branch Redirecting*,
  [VEE 2013](https://dl.acm.org/doi/10.1145/2517326.2451516): trampolines
  in the source code space.

Generic, exact, and on top of patch 20: the cheapest candidate here.

### 3. Pinned guest registers (patch 21, opt-in with two open bugs)

- **Zurstraßen, Bosbach, Reimann, Leupers**, *Static Global Register
  Allocation for Dynamic Binary Translators*,
  [DATE 2025](https://ieeexplore.ieee.org/document/10993060/): static
  guest-to-host register mappings, up to 1.4x over block-local allocation
  (RISC-V on ARM64). Doc 18's idea; confirms the size of the prize
  (ours: 7-Zip decompress +16 %).
- **Batuzov**, *Global register allocation during dynamic binary
  translation*, [Proc. ISP RAS](https://ispranproceedings.elpub.ru/jour/article/view/178?locale=en_US):
  per-block pre/post conditions in QEMU, 29.6 % on a synthetic example.
- *Low-Compilation-Cost Register Allocation in LLVM-Based Binary
  Translation*, [EuroSys 2026](https://dl.acm.org/doi/abs/10.1145/3767295.3803591).

### 4. Cheap IEEE flag checks (the deferred `fp-relaxed` mode)

- **Cota**, hardfloat, [QEMU 2018](https://patchew.org/QEMU/20181124235553.17371-1-cota@braap.org/):
  use the host FPU when the inexact flag is already sticky — the rule
  patches 06 and 11 apply and extend.
- **Zurstraßen, Bosbach, Jünger, Leupers**, *Rapid RISC-V Floating Point
  Vector Simulation*, [RAPIDO 2025](https://dl.acm.org/doi/10.1145/3721848.3721853):
  the same corner cases as patch 11's per-op checks; add and sub cannot
  underflow, so that check is dead; the checks vectorised. May trim doc
  16's "~25 instructions per packed check" without giving up exactness.

### 5. x87 at 64-bit precision, exact

Patches 48/49 keep the x80 as the shadow and do `*` and `fst m32`
inline; add, sub and div are still 128-bit integer helpers.

- **Zhang, Aiken**, *High-Performance Branch-Free Algorithms for
  Extended-Precision Floating-Point Arithmetic*,
  [SC '25](https://dl.acm.org/doi/10.1145/3712285.3759876): branch-free
  double-word add, sub, mul, div and sqrt on floating-point expansions,
  with machine-verified error bounds, 11.7x over QD. An x80 mantissa
  fits a pair of doubles exactly, so this is a possible FPU-side route
  for the remaining helpers; the final rounding to 64 bits still needs
  its own argument. Speculative.

### 6. Self-modifying code residue

Patch 24 refuses stores on the second page of a page-straddling block;
M9 item 4b wants two-page blocks linkable.

- **Dehnert et al.**, *The Transmeta Code Morphing Software*,
  [CGO 2003](http://www.xsim.com/papers/transmeta-code-morphong-software.dehnert-cgo03.pdf):
  self-checking translations that verify their own source bytes on entry
  instead of being invalidated on write, plus fine-grain write protection
  of code/data mixed pages. Old, but the exact technique.

### 7. Interrupt checks per block

- **Niu, Zhang, Li**, *Eliminate the overhead of interrupt checking in
  full-system dynamic binary translator*, [SYSTOR 2022](https://dl.acm.org/doi/10.1145/3534056.3534939).
  Patch 43 took the larger part of that cost (the block ends that forced
  the main loop).

### 8. Memory barriers on the Arm host

- **Risotto** — Gouicem et al., *A Dynamic Binary Translator for Weak
  Memory Model Architectures*, [ASPLOS 2023](https://dl.acm.org/doi/10.1145/3567955.3567962):
  verified fence placement for x86 on Arm, +6.7 %. Measured free on the
  M1 (doc 16's follow-ups); noted so nobody measures it again.

## The arXiv-native papers

- [2402.09688](https://arxiv.org/abs/2402.09688) — *A System-Level
  Dynamic Binary Translator using Automatically-Learned Translation
  Rules* (2024): learned rules inside system-mode QEMU, 1.36x over QEMU
  6.1 on SPEC CINT2006. The only arXiv paper about speeding up
  full-system QEMU. Rules learned from a modern compiler's patterns may
  not fit MSVC-6-era code.
- [2512.00487](https://arxiv.org/abs/2512.00487) — *Partial
  Cross-Compilation and Mixed Execution for Accelerating DBT* (2025):
  offloading whole functions to native host code, up to 13x. User-mode,
  and what our guest DLLs already do for Direct3D and Glide.
- [2501.03427](https://arxiv.org/abs/2501.03427) — *Boosting
  Cross-Architectural Emulation Performance by Foregoing the
  Intermediate Representation Model* (2025): a 35x claim from a proof of
  concept. Not credible for full-system work.
- [2605.08419](https://arxiv.org/abs/2605.08419) — Elevator, static
  x86-64 → AArch64 translation (2026). Wrong shape for us.
- [2009.03846](https://arxiv.org/abs/2009.03846) — x86 ↔ ARM
  concurrency mappings (2020); [1905.06825](https://arxiv.org/abs/1905.06825)
  — TLB simulation on QEMU (2019).

Nothing, on arXiv or elsewhere, addresses the Voodoo 2's per-dword MMIO
trap; doc 21 §9's RAM-backed FIFO window remains the plan.

## Ranking, by fit and evidence

1. The hardware-MMU papers (they de-risk the parked design and share our
   ISA pairing).
2. Tiaozhuan plus the return-address trick (cheap, exact, on top of
   patch 20).
3. The DATE 2025 register paper as a cross-check for patch 21's open
   items.
4. The RAPIDO 2025 flag-check paper for doc 16's follow-ups.
5. SC '25 double-word arithmetic for x87 PC=64 (speculative).
