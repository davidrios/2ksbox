# Track: M9 — TCG on Apple Silicon: where the vCPU's time goes, then the fix

The handoff for a session that makes the emulated CPU run faster on the
Air (aarch64 host, TCG only — there is no accelerator for an x86 guest on
Apple Silicon). Read `docs/00-status.md` first for the global picture and
the track rules, then this file. Branch: `track/m9-tcg-aarch64` was
merged to `main` and **deleted 2026-09-06** — the work is on `main`;
branch fresh off it for the next M9 item (patch 21's two open bugs
first).

Where M8 (docs 13 and 16) attacked floating point — x87 and SSE on the
host FPU, the biggest single win so far — this track is data-first: a
profile of real XP workloads under TCG on the Air says what the rest of
the vCPU time is made of (generated code vs helpers vs softmmu vs
translation), and the optimization is chosen from that, not from a guess.
The candidates that were on the table when the track opened (2026-09-04,
morning discussion): keeping x86 flags live in NZCV inside the aarch64
backend (Box64/FEX style, plus FEAT_FlagM), eliding the per-access memory
barriers on a single vCPU, and the structural one — host page tables
mirroring the guest's so the softmmu TLB lookup disappears (only viable
inside a Hypervisor.framework VM, a multi-month project with an open
question about helper calls needing VM exits; not started).

## Scope and files (this track owns them)

- Tools: `tools/tcg-profile.sh` (the runner), `tools/tcg-profile.py` (the
  report), `tools/tcg-hot.py` (the second pass: host code of the hot
  guest instructions), `tools/tcg_profile_lib.py` (shared parser);
  `tools/hvf-el1/` (the Hypervisor.framework EL1 probe: a bare-metal
  Rust guest + host, `build.sh`, README there).
- QEMU patches: `patches/qemu/13-perfmap-darwin.patch` — `-perfmap` on
  every host (upstream gates the option and `tcg/perf.c` on Linux; the
  map writer is plain stdio); `14-jit-wx-state.patch` — the macOS JIT
  write-protect switch only when the state changes;
  `15-tb-invalidate-fast.patch` — TB invalidation on guest writes, four
  cuts (below); `16-tlb-floor.patch` — a 4096-entry floor for the dynamic
  softmmu TLB; `17-rep-fast.patch` — REP MOVS / STOS as a host
  `memcpy`/`memset` per page run (below); `18-smc-same-value.patch` — a
  store that leaves a code page's bytes unchanged invalidates no TB
  (below); `19-tls-hot-paths.patch` — the store slow path's nested RCU
  locks and the translator's `tcg_ctx` reads off macOS's TLS thunk
  (below); `20-inline-lookup.patch` — the jump-cache probe of indirect
  branches as TCG ops (below); `21-pinned-regs.patch` — the guest
  register file in callee-saved host registers across chained TBs
  (doc 18, below; **in progress, off by default**). Later patches of the
  track: 22+.
- `tools/rep-guest-test.py` (the patch 17 oracle: a DOS battery of
  `rep movs`/`stos` cases, `rep-fast` on/off identical and equal to a
  Python model; `rep-guest` in the guest stage of `scripts/test.sh`);
  `tools/smc-guest-test.py` (the patch 18 oracle, `smc-guest`);
  `tools/smc-diff.py` (two captures of a code page diffed and
  disassembled: what a game patches); `tools/xp-moto-race.sh`'s
  `RACE_SAMPLE=` / `RACE_MEMSAVE=` / `RACE_DELAY=` / `FPS_RATE=` and the
  runner's `PERFMAP=0`.
- `tools/tcg-fps.py` (the guest's VGA frame rate from outside: distinct
  QMP screendumps per second, `FPS=` in the runner).
- Docs: this file, the M9 row of the tracks table in `docs/00-status.md`,
  the M9 section of `docs/08-roadmap.md`; `docs/18-pinned-guest-registers.md`
  (patch 21's design); a design doc once the
  optimization is chosen.
- Shared (rebase first, edit minimally, say so in the commit):
  `tools/qmpc.py` (the typer gained `~ \` ^ @ # $ [ ] { } ?`),
  `scripts/test.sh`, `patches/qemu/README.md`.

## How the profile works

`tools/tcg-profile.sh <image> <name> ['guest command']` boots the image
headless as a snapshot with `-perfmap` (`CDS=a.mds:b.iso` adds game discs
as ide-cd devices with CD audio through an AC97 card, the slots the player
uses; `QEMU_BIN=` another binary for an A/B, `QEMU_EXTRA=` more arguments,
`FPS=<s>` the frame-rate probe after the sample), types the command into
the Run dialog over QMP, lets it settle, then samples the *whole* QEMU process
with macOS `sample` at 1 ms for 30 s (Linux: `perf record`). The report
(`tcg-profile.py`) splits the vCPU thread's self time into generated code
/ helpers by family / softmmu slow path / translation + TB lookup /
interrupts / waiting / other, and maps every generated-code sample
through the perf map (one entry per translated guest instruction) to a
guest address → kernel / win32k / drivers / EXE / DLLs, hot pages, hot
instructions. `--hot` prints a `-dfilter` list of the hottest pages; a
second run with `DFILTER=` logs those TBs' guest bytes, optimized TCG ops
and host code, and `tcg-hot.py` weights them by the samples: host
instructions per guest instruction and their class mix, TCG op kinds
(memory accesses = TLB lookups, condition-code traffic, helper calls,
barriers), guest mnemonics, the hottest instructions decoded (capstone
from a uv venv; hex without it).

Gotchas met on the way (2026-09-04/05):

- `sample` shows no thread names; the vCPU thread is the one running
  `cpu_exec_loop`. It cannot unwind through generated code, so those
  samples are leaves under `cpu_tb_exec`.
- The XP images are Brazilian: Program Files is `C:\Arquiv~1`, and the
  keyboard layout is US-International, where `" ' ^ ~ \`` are dead keys —
  a typed `"` swallows the following space. Type 8.3 paths without
  quotes (`~` + digit yields both characters).
- Never edit a running bash script (the runner) — bash reads it
  incrementally and the run dies mid-way with a syntax error.
- QEMU's `-d out_asm` prints raw hex (`OBJD-H:`) in a build without
  capstone; `tcg-hot.py` classifies aarch64 words by opcode field instead.
- System-mode TCG emits the guest's memory-order barriers (`mb` before
  every load/store, `dmb ish` on aarch64) unconditionally — upstream keeps
  them even on one vCPU for the i/o threads' sake (`tcg_gen_mb`). The
  `QEMU_TCG_NO_MB=1` switch used for the barrier measurement was a hack in
  the tree, not a patch (prepare removed it).
- The second pass (`DFILTER=`) logs every translation of the filtered
  pages; on a workload that retranslates them 20k times a second (the
  game below) that is 6 GB in 30 s and the log writer takes over the
  vCPU thread. Turn it off over QMP (`log none`) or filter a cold page.
- The perf map spans every code-buffer epoch (a `tb_flush` restarts the
  bump allocator), so a host address names a different TB per epoch: the
  report tools now pick the epoch of the sample window from the `info
  jit` flush counts taken before and after the sample and print which;
  a window that straddles a flush is mis-mapped for its earlier part
  (keep `SECS` short on workloads that flush every minute).
- A guest's *frame rate* is the only honest before/after number for a
  game; `%` of a 100 %-busy vCPU says where the time goes, not how much
  work gets done. `tools/tcg-fps.py` counts distinct VGA screendumps per
  second (a game blitting a back buffer to the primary changes the VGA
  surface once per frame). Moto Racer's attract-mode intro reads 8 fps on both the
  baseline and the fixed build (2026-09-05) although the fixed build runs
  four times more guest code per second — the demo replays inputs at a
  fixed tick; the *race* moves (4.9 → 7.1–7.3 fps, `tools/xp-moto-race.sh`).
  Measure the game where the player plays it, and keep a fixed-work
  guest-reported oracle (7-Zip, Super PI) next to it.
- macOS JIT W^X (patch 14): a thread's initial write-protect state is not
  knowable — the main thread is in execute mode when `tcg_prologue_init`
  asks for write, a vCPU thread is not — so the tracked state must start
  as *unknown* and make the real call in either direction. A skipped
  first write call faults on the prologue store and the process spins at
  100 % inside `tcg_prologue_init` (QMP never answers: that is the
  symptom).

## State (2026-09-05)

Two things exist: the profiles below (patches 13 and 14 came out of
them), and the HVF EL1 probe (its own section further down, raw output
in `tools/hvf-el1/results-m1air-2026-09-05.txt`). Nothing of the
optimization itself is written yet; the next session starts at "Next
steps" with the user's pick.

All on the M1 Air (macOS 26.6.2), `-cpu pentium3`, one vCPU, XP SP3, 30–40 s
samples at 1 ms (`build/tcg-profile/<name>/report.txt`, `hot.txt`).

**Idle desktop.** The vCPU thread is 90 % in the halt wait; of the busy
10 %, translation + lookup 3.9 % (`tcg_flush_jmp_cache` — every CR3 write
flushes the 64 KiB jump cache — and the translator itself), 4 % QEMU
housekeeping, generated code 1.8 %.

**7-Zip 26.02 `b -mmt1`** (integer, LZMA; compress 1101 / decompress 1616
MIPS at dict 22, the "1T CPU freq" calibration reads ~3050 MHz): the vCPU
thread is 100 % busy — **generated code 77 %**, **translation + lookup
14 %** (13 % of it *self time in `helper_lookup_tb_ptr`*: every `ret` /
indirect jump leaves the TB to look the target up, ~70 host instructions
plus the register sync around the call), condition-code helpers 3.5 %
(`helper_cc_compute_c` for a carry read with the cc_op unknown at TB
entry), softmmu slow path 2.5 %, rest 2 %. 97 % of the generated-code
samples are in `7z.dll` (0x100d2000–0x100ee000); no kernel time to
speak of. Second pass (`hot.txt`, 98.5 % of the samples covered): per
guest instruction 23 host instructions, 6.7 TCG ops; 54 % of the hot
instructions carry a memory access (a softmmu TLB lookup each), 52 %
condition-code traffic, 4.5 % a helper call; guest mix `mov` 39 %, `jae`
22 %, `sub` 7.5 %, `jne` 6.6 %. **Where the samples land, by the exact
host instruction hit: 76 % on loads** — 43 % inside the TLB lookup
sequence (`ldp` mask/table → `ldr` tag + `ldr` addend → the access: a
chain of dependent loads per guest access), **41 % on guest registers
being loaded from / stored to `env`** (every TB starts by reloading the
registers it uses and every register write is stored back before the
next memory op, so a value flows store → load through memory at each
block boundary), 9 % other env traffic, 4 % arithmetic, 2 % the guest
access itself, 0.6 % block exits, 0.5 % barriers. With `sample` the PC is
the oldest unretired instruction, so this is a latency picture, not an
instruction count: the emulated CPU waits on load chains, and the chains
are the TLB lookup and the register file living in memory.

**7-Zip without the memory barriers** (`QEMU_TCG_NO_MB=1`, the `dmb`
before every guest load/store): compress 1098/1064 → 1145/1187 MIPS
(dict 22/23), decompress 1616/1460 → 1644/1483 — **+2 to +10 %, ~4 %
typical**, single runs. Correctness caveat before it becomes a patch:
device code that reads guest RAM outside the BQL (none known: the D3D
executor and the audio ring run on the vCPU thread or copy under the
lock).

**D3DGAME9 `-novsync`** (the reference Direct3D scene on the M4 device,
executor over KosmicKrisp): the vCPU thread is only 42 % busy — 58 % in
`__psynch_cvwait`, the guest waiting on the device / present path, so the
CPU is not the bottleneck of that scene here. Of the busy part: generated
code 22 %, translation + lookup 8.7 % (`helper_lookup_tb_ptr` 7 %), x87
helpers 3.9 % (`fldt`/`fstt` — 80-bit loads and stores stay helpers —
`fsin`, `fxam`, `fwait`, `fpop`), softmmu 1.9 %. 69 % of the generated
code samples in the EXE, 15 % in DLLs. Second pass (84 % covered): the
scene's hot code is x87 (`fstp` 16 %, `fld` 9 %, `fmul`/`fadd`/`fsub`/
`fdiv` 12 %, `fxam`/`fnstsw` 2 %), and under patch 06's shadow translator
an x87 instruction with a memory operand is 30–50 TCG ops and 50–80 host
instructions (`fld dword [esp+0x24]` 34 ops / 49 host; `fsub dword [mem]`
52 / 78): 77 % of the hot instructions carry a memory access, 11.5 % a
helper call (`fxam`, `fnstsw`, the 80-bit loads/stores); samples land
41 % on arithmetic ("work"), 35 % in the TLB lookup, 15 % on the shadow
state in `env`, 6 % on guest registers; a third of the samples are in
unchained TBs (the x87 mode exits). That is M8's "cheap-op throughput of
the shadow translator" item, seen from the host side; not this track's
first target.

**Super PI mod 1.5 XS, 1M** (x87 under patch 06; `C:\Docume~1\David\Desktop\SUPER_~1.5`,
driven with `KEYS='alt+c,down×6,ret,ret'`): vCPU 100 % busy — generated
code 72.5 %, **`pthread_jit_write_protect_np` 12 %** (macOS's per-thread
W^X switch for the MAP_JIT code buffer: `cpu_tb_exec` asks for execute
before *every* TB it runs from the main loop, and this loop returns to
the main loop constantly because patch 06's out-of-line x87 helpers —
`frndint`, `fistll`, `fldcw`, `fprem`, `fptan`, `fwait` — end their TB
with an exit; `cpu_exec_loop` + `cpu_tb_exec` themselves 3.2 %),
`helper_lookup_tb_ptr` 5.7 %, x87 helpers 2.6 %, softmmu 1.1 %; 86 % of
the generated code in the EXE (0x417000–0x42e000), 12 % kernel. The
redundant toggles are gone with **patch 14** (a per-thread state):
**1M in 1:36.2 → 1:25.3 (−11 %)**, same binary otherwise, same day, the
timed runs in `build/tcg-profile/superpi-before` / `-after`; the
underlying exits are M8's: an x87 helper exit could chain through
`lookup_and_goto_ptr` or not end the TB at all. Second pass (93 %
covered; samples: arithmetic 42 %, TLB lookup 32 %, shadow state in
`env` 18 %, guest registers 6 %): typical shadow-mode costs are
`fstp qword [mem]` 18 ops / 30 host instructions, `fld st(3)` 9 / 11,
`fadd qword [mem]` 34 / 55, `fxch st(3)` **44 / 48**, `fdivp` **88 /
117**, `fcomp qword [mem]` 50 / 64 — M8 follow-ups (`fxch` and `fdivp`
first). Beware in `hot.txt`: the last guest instruction of a TB carries
patch 06's slow blocks (thousands of host instructions), so per-
instruction averages there are meaningless; read the per-sample lines.

**FIFA 2000 on the M7 driver** crashed at start on this Mac on the
2026-09-05 morning image (XP's own error box); the user's updated
`winxp-m7.qcow2` (same day, evening) runs FIFA 2000 and Moto Racer 1997,
both slow, Moto Racer slowest — profiled below with the player's disc
layout (`CDS=MOTO_RACER.mds:FIFA2000.ISO VGA=d3dpt MEM=1024`, the game
started from the Run dialog as `cmd /k cd /d C:\Arquiv~1\MotoRacer &
MOTO.EXE`; FIFA is `C:\Arquiv~1\EASPOR~1\FIFA20~1\fifa2000.exe`, the
WineD3D DLLs already renamed away in the image).

### Moto Racer 1997, the in-engine intro (2026-09-05, evening): two pathologies, not "TCG is slow"

The intro is the game's own software renderer at 640×480×16 into a
system-memory back buffer blitted to the primary (`d3dpt_ddraw.log`:
plain DirectDrawCreate, no Direct3D). Baseline build (patches ≤ 14), 30 s
sample, vCPU 100 % busy: **generated code 14 %, softmmu slow path 49 %,
translation + TB maintenance 25 %**, perf-map writer 2 % (the profiler's
own cost). 97 % of the slow-path samples are `victim_tlb_hit` /
`mmu_lookup` / `do_ld4_mmu` / `do_st4_mmu` under the loads *and* stores of
one guest loop, with no page walks (`tlb_fill` 0.2 %); `info jit`: 600k TBs
translated and 557k invalidated in the 30 s. Two separate causes:

1. **The dynamic TLB was 64–256 entries.** `tlb_mmu_resize_locked` sizes
   the direct-mapped table at every flush from the entries used since the
   previous flush, and XP flushes at every context switch (450–750/s here),
   so the "working set" it sees is a few hundred pages and the 30–70 %
   use-rate policy settles at 64–256 entries (`CPU_TLB_DYN_MIN_BITS` is 6).
   At that load two live pages share an index all the time — a sampled
   victim hit every 65536th: `page 0x77bf1000 evicts 0x5f1000, 256
   entries` (both index 0xf1), `0x10dcc000 evicts 0xcd0c000, 64 entries` —
   and every access to either page is a victim-TLB swap (~40 ns, three
   entry copies under the TLB spinlock): **1.64 billion victim hits in a
   4-minute run, ~6 million a second**. A 4096-entry floor (**patch 16**,
   `CPU_TLB_DYN_MIN_BITS`/`DEFAULT_BITS` 12): 1.5 million hits in the same
   run (1000× fewer), slow path 49 % → 1.3 %, generated code 14 % → 57 %.
   128 KiB of entries per mmu index, a memset per flush (~0.5 % at XP's
   flush rate). Upstream's policy would want a conflict counter; the floor
   is the one-line version.
2. **TB invalidation storms, three sources.** (a) Physical page 0xca000 is
   QEMU's *vAPIC option ROM* (`kvm aPiC`, `hw/i386/vapic.c`): XP's HAL has
   its TPR accesses patched into `call`s to stubs there (`out 0x7e,al;
   movzx eax, byte [0x800ca300]; ret` per register, a `lock cmpxchg` on the
   same dword for raises) — every IRQL change of the guest runs them, and
   the `VAPICState` the APIC writes at every interrupt (`apic_sync_vapic`
   → `cpu_physical_memory_write`) is at 0xca300, *the same page*. The DMA
   side's `tb_invalidate_phys_range` rounds the first page's range down to
   the page start (a 2023 upstream regression, still in master
   2026-09-05), so every interrupt invalidated all 17 stub TBs, each
   invalidation flushing the whole 64 KiB jump cache (CF_PCREL), and they
   were retranslated ~5k times a second — *on every XP run, idle
   included* (idle: 136k TBs in 40 s; 7-Zip: 50k). (b) Writes to pages
   that carry any TB take the slow path and, before this patch, the page
   collection lock (two g_tree lookups) plus a walk of the page's TB list
   *per write* even when nothing overlaps: the vAPIC lock word (23k
   writes/s), pool pages with a stale TB (45k/s) — ~12 % of the vCPU. (c)
   **The game is genuinely self-modifying**: page 0x436000 of MOTO.EXE holds
   four unrolled copies of the textured-span inner loop (`add edx, imm32;
   adc bh, imm8; dec esi; mov [edi], ax; lea edi,[edi+2]; jmp`) whose
   immediates (texture pointer, u/v deltas, carry bytes, 52 bytes per
   round) are rewritten per span: 187k writes and 20.8k retranslations a
   second, values that really change (five page dumps 0.5 s apart differ
   at exactly those 52 bytes). **Patch 15** takes (a) away (range clamped
   to the write), makes (b) nearly free (a per-page byte range of its TBs:
   a write outside returns before any lock; a write that cannot hit a TB
   shared with a neighbouring page is handled under the page's own
   spinlock, no collection; the precise-SMC g_tree lookup of the writing
   TB only when a TB is hit) and drops the per-invalidation jump-cache
   flush (`CF_INVALID` already makes a stale slot miss). (c) remains:
   after both patches the vCPU is **generated code 57–59 %, translation +
   TB maintenance 25 %** (the list walk of the SMC page 9 %, the
   retranslations ~10 %, `helper_lookup_tb_ptr` 4 %), softmmu 1.3–1.9 %,
   `_tlv_get_addr` 3 % (macOS TLS: every `tcg_ctx` access in the
   translator is a call), perf-map writer 3 % (profiling only).

The remaining cost of (c) is the game's design meeting TCG's page-granular
code protection; ideas ranked: compare-before-invalidate in the store
slow path (skip when the patched bytes are unchanged — measured values do
change between spans, so probably small), a per-page 64-byte code bitmap
to shorten the walk (the patches land inside the code, so partial), and
the real one, "soft immediates": retranslate a TB that keeps being
invalidated with its patched immediates read from guest memory at
runtime and let writes to those bytes skip the invalidation (a translator
feature, days). Not before the register pinning of step 2.

**Correction (2026-09-05, later in the day): the generated-code time is
not the span loop.** The profiler attributed samples through a perf map
that merges every code-buffer epoch: TCG bump-allocates TBs and every
`tb_flush` restarts at the buffer's start, so a host address holds a
different TB in each epoch, and `load_map`'s "later entries win" picked
whichever entry started nearest — the game's hottest instruction came
out as `0x4365f6/ff/600` in one run, `0x46054e` in another, an ntdll
address in a third, a kernel one in the race. Fixed the same day
(`tcg_profile_lib.load_map(path, epoch)` splits the map where the
addresses jump back; `tcg-profile.py` / `tcg-hot.py` take the epoch from
the `TB flush count` of `info-jit-before/after.txt` and say so in the
report; a sample window that straddles a flush is flagged). Regenerated,
every Moto Racer run agrees: **80–90 % of the generated-code samples
(≈ 47 % of the vCPU) are one instruction, `rep movsd` at `0x46054e`** —
the game's own rectangle blit (`mov edi,eax; mov esi,ebp; ecx = width/4;
rep movsd; rep movsb; next row`, source and destination both system RAM:
a second of `memory_notdirty_write_access` traces shows no VRAM page).
The span loop of page `0x436000` is 2 % of the generated code; its
retranslation storm (25 % of the vCPU, the SMC pathology above) stands.
Second pass on the blit page (`moto-blit-d`, `hot.txt`): one `rep movsd`
iteration is **37 host instructions** — two `dmb` barriers, two softmmu
chains, and ESI/EDI/ECX loaded from and stored to `env` every iteration
with `cx_next` and the loaded value spilled to the stack, because the
loop's back edge is a TCG basic-block boundary and globals are synced
there; the samples sit on the two chains' addend loads and the loop-end
spill. The other runs with a flush (Super PI, D3DGAME9, FIFA) were
re-checked: their sample windows had no flush after them, so their
attribution was right.

`tools/hvf-el1` got the blit as kernels (`bench.S`, `exp_movs` /
`native_movs`, results in `results-movs-m1air-2026-09-05.txt`): rows of
640 bytes copied five ways, ns per dword, M1 Air, source and
destination of 32 KiB / 512 KiB / 8 MiB each (the numbers do not move
with the size):

| kernel | native | in the VM (env in the window) |
|---|---|---|
| today: TCG's loop transcribed from `qemu-d.log` (37 insns, registers through env, spills, barriers) | 2.11–2.16 | 4.4 (see the hazard below) |
| the same without the barriers | 2.12 | 4.2 |
| pinned guest registers + the softmmu chains (step 2 alone) | 1.17 | 1.17–1.19 |
| pinned registers + direct window accesses (the VM design) | 0.40 | 0.40 |
| a REP MOVS fast path: one softmmu probe of the source and one of the destination per run inside a page, then a 16-byte vector copy | 0.09 | 0.09 |
| libc memcpy per row (the floor) | 0.05 | — |

So on this loop the barriers cost nothing, pinned registers 1.8×, the
VM's mirror 5×, and a fast path 23× — without a VM. The fast path is a
translator change (patch 17 below): `rep movs`/`stos` with count ≥ N
probe both pages once per page-run (`probe_access`-style, refusing
MMIO / not-dirty / watchpoint pages and overlapping runs to the
per-element loop), copy with host memcpy, and restart the instruction
at the page boundary with ECX/ESI/EDI updated; the per-element loop
stays for the rest. Windows' own `memcpy`/`memset` (`RtlCopyMemory`,
`RtlFillMemory`) are `rep movsd`/`stosd`, so every guest benefits.

**A hazard the VM design must respect (found on the way).** In the VM
the pinned kernels first ran at 2.5 ns instead of 0.40 (`results-movs`,
the `env identity` lines): a store to `env` followed by a store through
the window costs ~2.2 ns extra per pair when `env` lives in the
payload's identity map (2 MiB blocks, global) and the window is 4 KiB
non-global pages — the `diag3` kernels isolate it (env store + window
load: fine; env store + window store: 2.5 ns; the same through the
identity view of the same RAM: fine; every single-mapping pattern,
stack included: fine). With `env` placed inside the window the loops run
at native speed; "today" stays at 4.4 because its stack spills still go
to the block-mapped image. Rule for the port: everything translated
code stores to — `env`, the TCG stack frame, the TLB, the TB cache —
must be mapped with the same kind of stage-1 entry as the guest window
(4 KiB pages; whether the global/non-global mix or the block/page mix is
the trigger is a one-hour follow-up in the probe). The verdict below
stands with that constraint added.

**The race, A/B** (`tools/xp-moto-race.sh`: demo → title → Play Solo →
Practice → Speed Bay, throttle held, `tools/tcg-fps.py` for 15 s;
`moto-race-base` / `moto-race-fix`): **4.9 → 7.1–7.3 fps** (+45–50 %, two runs of the fixed build). Less than
the four-fold rise of the generated-code share because the frame now
pays the SMC retranslations (25 %) and, above all, the blit's `rep
movsd` (the correction above: 90 % of the race's generated code too). Two leads beyond this track: the game's options screen says
`D3D: NOT DETECTED` — Moto Racer has a Direct3D 5 renderer and its HAL
probe fails against our M7 driver; with it detected the software
rasterizer (and all of the above) would be bypassed (an M7 item). And the
attract-mode intro reads 8 fps on *both* builds while the race moves, so
the demo is paced by the game (replayed inputs at a fixed tick).

**FIFA 2000 under the runner** (`fifa-fix`): `fifa2000.exe` started from
the Run dialog exits within 45 s, no error box on the screendumps, vCPU
idle. Under the player the user runs it fine, with the discs in another
slot order (the runner's `CDS` puts the guest-tools ISO on ide.1/0 and the
games on the slave slots, so FIFA's disc is F:, not the letter the
install recorded); `tools/xp-fifa2000.bat` / `tools/xp-fifa-match.sh` (M7)
know its start sequence. Not profiled yet — next steps.

**7-Zip A/B, same day** (`7zip-ab-base` / `7zip-ab-fix`, dict 22 / 23,
single runs): compress 1112 / 999 → 1137 / 1049 MIPS, decompress 1582 /
1504 → 1621 / 1547 (+2–5 %). Its working set never conflicted in the small
TLB (softmmu 1.4 % in both), so this is the vAPIC storm and the
jump-cache flushes alone; the games are where patch 16 bites.

Runs: `build/tcg-profile/moto-intro` (baseline), `-fix1` (range clamp),
`-fix2` (+ TLB floor), `-fix3` (+ jump cache), `-fix5` (+ page ranges,
single-page path), `moto-ab-base` / `moto-ab-fix` (A/B with the frame
probe), `moto-intro-d/hot-tbs.txt` (the vAPIC stubs' bytes),
`moto-intro-d/trace-1s.log` (one second of `translate_block` and
`memory_notdirty_write_access` events, taken over QMP with
`trace-event-set-state` on a kept guest).

**What the data says.** On integer code the cost is (1) the register
file in memory at block boundaries, (2) the four-load TLB chain per
guest access, (3) the out-of-line TB lookup for every return and
indirect jump, then (4) the carry helper at TB entry and (5) the
barriers. Flags computation itself is cheap (lazy cc_op, inline compare
+ branch); the x87/SSE work of M8 does not show up any more except the
80-bit `fld`/`fst` and the transcendental helpers on the D3D scene.
Structural fastmem (host page tables) is the only fix for (2); (1) and
(3) are translator work with ARM's register budget: the aarch64 backend
allocates from x20–x28 first (nine callee-saved registers, x19 = env),
enough to pin the eight GPRs plus `eip` for the life of a chained run
of TBs and store them only before helper calls, in the memory slow-path
stubs and in the epilogue.

**Where the Hypervisor.framework "fastmem" idea stands after the profile
(2026-09-05).** HVF cannot run x86 code on Apple Silicon; its only use is
to run TCG's Arm output inside an Arm VM whose stage-1 tables mirror the
guest's x86 page tables, so the software TLB lookup disappears. The data
says that is worth at most the TLB chain's share (43 % of generated-code
samples on 7-Zip, ~a third of the vCPU), and nothing of the other half
(registers through memory at block boundaries, the TB-lookup helper,
block exits). Against it: generated code calls C helpers constantly
(4.5 % of the hot instructions on 7-Zip, 11 % on the D3D scene, plus
every TLB miss and interrupt), and inside a VM each is an exit of about
a microsecond — more than the TLB lookup costs today — unless the
helpers, i.e. most of QEMU, run inside the VM at EL1 with no macOS
underneath. That was the open question; the probe below (same day)
measured every primitive of that design on the Air.

## The HVF EL1 probe (2026-09-05): what the VM design would cost

`tools/hvf-el1/` (README there) is a 37 KiB bare-metal Rust guest that
runs at EL1 under Hypervisor.framework with its own MMU, vectors and a
fault-driven mirror of an x86-style page table, plus the host that maps
a 64 MiB arena into it **at the same address in the host process, as IPA
and as guest VA** (pointers are shared, so `env`, the TB cache and guest
RAM could stay where QEMU has them), services its exits and runs the
same load kernels natively. M1 Air, macOS 26.6.2, two runs, the arena at
0x6_0000_0000 and 0xb_0000_0000; ~2 s. The hardware facts first: the VM
exposes a 4 KiB granule (`TGran4=0`; the x86 pages need it), 8-bit ASIDs,
36-bit IPA, no hardware A/D flag updates, `CTR_EL0.DIC/IDC=0` (cache
maintenance needed after JIT writes).

**Exits vs in-VM work** (the question the paragraph above left open):

| Primitive | ns |
|---|---|
| VM exit, `hvc` round trip (guest → host → guest) | 845–870 |
| VM exit, MMIO load (stage-2 miss → host emulates → retry) | 830 |
| in-VM exception (`svc` → EL1 vector → full frame incl. q0–q31 → `eret`) | 40 |
| in-VM helper call (`bl`) | 0.9 |
| host thread kick → `hv_vcpus_exit` → guest IRQ handler | 1.9–2.2 µs median (1.0–1.2 of it until `hv_vcpu_run` returns) |

An exit is ~900 helper calls or ~21 in-VM exceptions. On 7-Zip a
helper runs every ~20 ns of guest work (4.5 % of the hot instructions
at ~0.9 ns per guest instruction), so a design that exits for helpers
would be ~40× slower than today: **helpers must run inside the VM**,
which is settled. What the probe adds is that the runtime this needs is
small and works: exception vectors, page tables, a walker, an allocator,
`core::fmt` — the whole guest is 37 KiB and boots in a page of assembly.

**The memory model** (the x86 page tables mirrored in stage 1, one root
per CR3 with an ASID, pages mirrored read-only until the x86 D bit is
set — `tlb_set_page`'s rule):

| Event | ns |
|---|---|
| HVF populating stage 2 on the guest's first touch of a RAM page (once per page, in the kernel, no exit) | 745 / 4 KiB |
| first touch through the window: abort → x86 walk → PTE → `eret` (the softmmu refill) | 110–160 |
| hardware TLB miss with the tables warm (nested stage-1+2 walk) | 6–6.5 |
| x86 #PF delivered to the resume point (the in-VM `cpu_loop_exit`) | 67 |
| first write to a clean page: permission fault → D → remap RW → `tlbi` | ~495 (`vae1` non-broadcast is no cheaper) |
| CR3 switch, tables kept (`msr ttbr0_el1` + ASID) | 19 |
| the flush model instead (drop the mirror, refault 1024 pages) | 112 / page |
| JIT: patch + `dc cvau`/`ic ivau`/`isb` + call, in the VM | 134 (native, with the W^X toggle pair: 173) |

Correctness checks pass: A and D bits are set by the walk as x86 would,
a read-only page's write yields error code 3, an unmapped page's read
yields 0, two CR3s see their own pages through one window, code written
by the host process executes in the VM after cache maintenance.

**The loads**, ns per load, the kernels of `bench.S`: `direct` is one
`ldr w, [x_win, w_addr, uxtw]` (what translated code would emit with the
mirror), `softmmu` the exact `prepare_host_addr` sequence tcg/aarch64
emits today (`ldp` mask/table, index, comparator, addend, compare,
branch, load) against a TLB sized as QEMU's dynamic one and always
hitting, `pinned` the same with mask/table already in registers (next
step 3). "native" is the host process on macOS's 16 KiB pages — QEMU's
situation today; "VM" is 4 KiB guest pages under stage 2.

| set | chase VM direct | VM softmmu | native softmmu | native direct | indep VM direct | VM softmmu | native softmmu | native direct |
|---|---|---|---|---|---|---|---|---|
| 64 KiB | 1.24 | 3.22 | 3.23 | 1.25 | 0.31 | 0.64 | 0.64 | 0.31 |
| 4 MiB | 3.50 | 5.80 | 4.76 | 2.37 | 0.80 | 1.12 | 1.14 | 0.71 |
| 8 MiB | 5.55 | 10.4 | 10.2 | 5.10 | 0.89 | 1.37 | 1.45 | 0.86 |
| 16 MiB | 28.1 | 33.1 | 12.3 | 7.3 | 2.57 | 4.82 | 4.11 | 2.52 |
| 32 MiB | 29.0 | 35.1 | 14.3 | 7.7 | 3.27 | 6.02 | 5.44 | 2.81 |

Reading it: while the working set fits the L2 TLB (3072 entries → 12 MiB
at 4 KiB), the mirror removes the whole chain — a dependent guest load
costs 1.2–5.5 ns instead of 3.2–10.4, independent ones 0.3–0.9 instead
of 0.6–1.4: **1.4–2.6× per load, against native softmmu**, which is the
43 %-of-samples chain the profile found. `pinned` (the mask/table pair
in registers) buys nothing on dependent loads and 10–20 % on
independent ones, so next step 3 is worth less than hoped. Beyond
the TLB reach the picture inverts on random dependent access: a 4 KiB
page miss under stage 2 costs ~21 ns (16 and 32 MiB chase: 28–29 ns vs
native softmmu's 12–14 on 16 KiB pages), while independent loads still
win (2.6–3.3 vs 4.1–5.4) because the core overlaps the walks. This is
the one real performance risk of the design and it is measurable
before building anything: the vCPU's distinct-page working set per
second on 7-Zip / FIFA / D3DGAME9 (a counter in `tlb_fill`). Mitigations
exist — XP's large pages (PSE) become 2 MiB stage-1 blocks, contiguous
x86 physical runs can take the Contiguous hint (64 KiB entries) — but
they are workload-dependent.

**What "most of QEMU inside the VM" actually is** (inventory of the
tree, 2026-09-05). The code that must run in the VM is the vCPU core,
~45 KLOC of C that is already self-contained: `tcg/` (tcg.c 6.6 K, the
optimizer 3 K, the aarch64 backend 3.5 K, region.c), `accel/tcg/`
(cpu-exec, translate-all, translator, tb-maint: 3.5 K), `target/i386/tcg/`
(translate + decoder + emitter 11.8 K, the helpers 8 K incl. fpu 3.5 K
and seg 2.5 K, `sysemu/excp_helper.c` = `mmu_translate`), `fpu/softfloat.c`
5.3 K, `util/qht.c`. Its outside surface, by grep of those files:
glib = `g_malloc/g_free/g_new` (≈35 sites), `g_hash_table` (a handful,
tb-maint/region), `g_assert`; `qemu_log*` (~60); `qemu_mutex/spin` (page
and TB locks, ~30) and `bql_lock/unlock` (20: interrupt delivery and
device access — the host handshake); `rcu` reads (10); `sigsetjmp/
siglongjmp` (4); `mmap/mprotect` (region.c, the code buffer);
`qemu_thread_jit_write/execute` (the W^X toggles — gone); the memory
API only in the page walker (`address_space_ld*`, 6 sites → loads through
the identity map); `cpu_get_apic_*`, `cpu_get_pic_interrupt`,
`cpu_get_tsc`, `qemu_clock_get_ns` (device and clock access → the
mailbox, `cntvct` with a host-provided scale). The one deep change is
**`cputlb.c` (2.9 K) replaced by the fault-driven mirror** — this probe's
`mmu.rs` is 230 lines of the idea — which re-expresses every `TLB_*`
flag as a stage-1 permission: `TLB_NOTDIRTY` for pages holding TBs
(write-protect, fault → `tb_invalidate`), dirty logging for the
framebuffer (write-protect, fault → dirty bit), `TLB_MMIO` (not mapped
→ fault → exit to the device model, 0.83 µs — the same slow path as
today's `io_readx`, and qemu-3dfx's / d3dpt's FIFOs are RAM, only their
doorbells are MMIO). The ~70 `cpu_ld*/st*` sites in the helpers become
window accesses under the same fault rule. Devices, the memory API,
block, display, audio, QMP, the main loop stay in the player process;
the player binary carries the hypervisor entitlement (ad-hoc signed, as
QEMU's own build does).

**Verdict.** The "months, uncertain" of the paragraph above is now
"weeks, with one known risk": every primitive of the design works on
the Air and is cheap (the whole probe took a day), the in-VM runtime is
tiny, the shared-address arena lets the vCPU core and the device model
keep sharing QEMU's data structures by pointer, and the port is a
mechanical freestanding build of ~45 KLOC plus a rewrite of cputlb's
2.9 K and a mailbox protocol (I/O, interrupts = kick + a pending word,
TB invalidation for DMA, clocks). A first XP boot inside the VM is a
4–8 week effort for a focused session series; the gain ceiling on
TLB-resident code is the 33 % of the vCPU the profile attributes to the
chain (1.4–2.6× per load), and the CR3 switch cost (today: TLB flush +
the 64 KiB jump-cache memset + refills; 19 ns with ASIDs) comes on top —
XP idle spends 3.9 % there. The risk is the nested-TLB penalty on
random working sets beyond 12 MiB, to be measured on the real workloads
before deciding. Steps 1 and 2 of the list below (inline TB lookup,
pinned registers) are orthogonal and carry over into the VM unchanged;
step 3 (pinned mask/table) is not worth a patch by itself.

## Build / test loop

```sh
scripts/prepare-qemu.sh && scripts/configure-qemu.sh          # after any patch edit
ninja -C build/qemu qemu-system-i386 libqemu-embed-i386.dylib
tools/tcg-profile.sh ~/vms/winxp.qcow2 idle                          # ~2 min
CDROM=~/vms/bench.iso tools/tcg-profile.sh ~/vms/winxp.qcow2 7zip \
    'cmd /k C:\Arquiv~1\7-Zip\7z.exe b -mmt1 40'
CDROM=~/vms/FIFA2000.ISO VGA=d3dpt BOOT_WAIT=120 WARM=150 SECS=40 \
    tools/tcg-profile.sh ~/vms/winxp-m7.qcow2 fifa 'cmd /k cd /d C:\Arquiv~1\EASPOR~1\FIFA20~1 & ... & fifa2000.exe'
python3 tools/tcg-profile.py build/tcg-profile/7zip --hot            # the -dfilter line
DFILTER=... tools/tcg-profile.sh ... ; venv/bin/python tools/tcg-hot.py build/tcg-profile/7zip
tools/hvf-el1/build.sh && build/hvf-el1/hvf-el1 build/hvf-el1/payload.bin   # the HVF probe, ~2 s
scripts/test.sh all                                                  # before every commit
```

## Patch 17: the REP MOVS / STOS fast path (2026-09-05, evening)

The table above said a per-page-run `memcpy` was 23× the loop and nothing
else came close, so it went first. The shape (`patches/qemu/17-rep-fast.patch`,
`target/i386/tcg/mem_helper.c` + `do_gen_rep` in `translate.c`):

- `do_gen_rep` emits, before its per-element loop and only for `movs` /
  `stos` with at least 8 elements to go (and never with TF, single-step
  or icount, where one element per step is the architecture), a call to
  `helper_rep_movs_fast` / `helper_rep_stos_fast` with the linear source
  and destination (the same `gen_lea_v_seg` the loop uses: segment base,
  a16/a32 wrap), the masked count and `ot | mmu_idx`.
- The helper takes the run that stays inside the current page of both
  (direction from `env->df`, an element straddling the page end ends the
  run before it), probes the source page for a load and the destination
  for a store with `probe_access_flags(nonfault=true)` — that fills the
  TLB, marks the destination dirty and invalidates the TBs under the run
  exactly as the per-element stores would, and says no for MMIO,
  watchpoints and unmapped pages — then `memcpy` (disjoint), `memmove`
  (the overlap the guest's order reads before it writes) or an element
  loop in the guest's order (the overlap that replicates a pattern);
  `stos` is `memset` when the value's bytes are equal, a store loop
  otherwise. It returns the elements done and writes nothing to `env`:
  a longjmp out of the probe (the write hit the current TB) resumes at
  the instruction's start with the right state, and a fault is left to
  the loop, which raises it at the right element.
- The translator advances ESI/EDI/ECX by the count (`gen_op_add_reg`,
  so a16 deposits into the low halves), exits if ECX is zero, and
  otherwise re-enters the instruction with RF set — the same path the
  loop takes between iterations — so the next page gets its own probe;
  a 0 from the helper falls into the loop, now capped at 15 iterations
  per entry instead of 65535 so that after a straddling element or an
  MMIO page the fast path is tried again.

Oracle: `-cpu …,rep-fast=off`. `tools/rep-guest-test.py`: a DOS program
runs 536 cases (movs/stos × b/w/d × a16/a32 × DF, counts 0–17 at the
threshold and the page's end, aligned and misaligned runs over one and
two page boundaries, elements straddling a page, every overlap of
source and destination in a run that crosses a page, fill values with
equal / distinct / zero bytes) over a page-aligned 16 KiB region and
prints a hash of the region plus ESI/EDI/ECX after each; both logs are
identical and every line equals a Python model of the instruction.
`tools/string-bench.py` (real mode, 8 KiB buffers): MOVSD 2.15 → 0.07
ns per element, STOSD 2.08 → 0.07, the byte forms below the bench's
one-tick resolution (it was written for the 12 % of patch 09), SCASB
unchanged at 2.08. Moto Racer's race (`tools/xp-moto-race.sh`,
`winxp-m7`, the A/B on one binary): **7.3 fps off, 7.5 fps on — unchanged.**

That number needed explaining, and the runner's own profile (a 1 s sample
during the attract demo) explained the wrong thing: there the path *is*
active (generated code 96.6 % → 44 % of the vCPU, `memmove` + the two
probes + the helper 35 %) — but the demo is not the race.

## Patch 18: the pacer — same-value stores into self-modifying code (2026-09-05, night)

"Find the pacer": what sets the race's frame rate when a 2–3× cheaper blit
changes nothing. Ruled out first (each with a run): the M7 driver's
`DdWaitForVerticalBlank` spins on the device's FRAMES register, which only
console refreshes advance — headless, only the fps probe's own screendumps
— with a 50 ms give-up; the probe at 60 dumps/s instead of 25 (`FPS_RATE=`)
left the race at 7.4 fps, so the game is not waiting there. (A device-side
vblank timer is still the right shape for M7; it was written, falsified
as the pacer, and reverted.) Then the race itself was sampled
(`RACE_SAMPLE=15`, the sample taken with the throttle held, report in
`<out>/race/`), and it looks nothing like the demo:

| vCPU, in the race (`moto-race-prof`) | |
|---|---|
| translation + TB lookup (of it `tb_invalidate_phys_page_range__locked` 17 %, `liveness_pass_1` 6 %, `sys_icache_invalidate` 4.4 %, `tcg_optimize` 3.5 %) | 53 % |
| other (`_tlv_get_addr` 8.8 % — macOS TLS for `tcg_ctx` in the translator) | 26 % |
| the perf map writer (`-perfmap`, the runner's own cost) | 12 % |
| generated code | 4.8 % |
| the blit fast path (`_platform_memmove`) | 1.9 % |

`info jit` over the 16 s: **58,000 TB invalidations a second**. The game is
CPU-bound in *translation*: its rasterizer patches its own code and every
patch throws the block away. Two captures of the hot pages a second apart
(`RACE_MEMSAVE=0x482000:0x2000,0x436000:0x1000,0x460000:0x1000`, QMP
`memsave`) diffed with the new `tools/smc-diff.py` (capstone) show exactly
what: page `0x436000` is a texture-mapping span loop unrolled four times —
`mov ax, [ebx*2 + disp32]` (the texture base), `add ecx, imm32` / `adc bl,
imm8` / `add edx, imm32` / `adc bh, imm8` (the u/v steps and their
carries) — 44 bytes in 28 runs patched per span setup; the other hot pages
(`0x482000`–`0x483000`, the rasterizer of the race; `0x460000`, the blit)
do not change at all. A stats build (counters in the store slow path and
the invalidation walk, not committed) then gave the number that decided
the fix, in the race:

| per second | |
|---|---|
| stores into pages holding code | ~700,000 |
| of those, writing the value already there | **94 %** |
| invalidation walks / TBs visited by them | ~700,000 / ~55,000,000 |
| TBs invalidated | ~62,000 |

The game rewrites its whole span parameter block per span and most of it
lands unchanged; QEMU invalidated on every write and walked ~80 TBs per
write to find the one it hit. The track doc's earlier guess ("measured
values do change between spans, so probably small") was wrong by a factor
of 16.

**The fix** (`patches/qemu/18-smc-same-value.patch`, `accel/tcg/cputlb.c`):
the store slow path already knows the value; `MMULookupLocals` carries the
bytes about to be written in address order (set by `do_st{1,2,4,8}_mmu`,
memop endianness applied), `mmu_watch_or_dirty` compares them with memory
(each page's part for a crossing store) and `notdirty_write` skips
`tb_invalidate_phys_range_fast` when nothing changes — the dirty bits are
still set, and the page stays "with code" so later stores keep taking the
slow path. Exact by construction: a TB translated from bytes B is valid
while memory holds B. 16-byte stores, the probe paths (`probe_access*`,
so the REP fast path's `memcpy` into a code page) and the atomic path do
not know the bytes and invalidate as before. `-accel tcg,smc-same-value=off`
restores the old behaviour; `tools/smc-guest-test.py` (nine DOS cases:
immediates patched from another block and inside the executing block,
same-value rewrites, an opcode flip, 16-bit and 8-bit partial patches,
`rep movsd` over a routine with new and identical bytes, an imm32
straddling a page boundary written by one crossing store) is right on and
off, and sits in the guest stage of `scripts/test.sh` as `smc-guest`.

**Result: Moto Racer's race 7.3 → 21.7 fps at the standing start** (the
25-dumps/s probe nearly saturated, 325 distinct in 354 dumps; **39.2 fps at 60 dumps/s**, 588 distinct in 710, so still near the
probe's ceiling — over 5× from 7.3), **mid-race 12.1 → 19.0 fps** (`RACE_SAMPLE=15` then the
probe, the perf map on both times). The race's vCPU after (`moto-smc18-prof`):

| vCPU, in the race, patch 18 | before | after |
|---|---|---|
| translation + TB lookup (the invalidation walk) | 53 % (17 %) | 29 % (4.2 %) |
| generated code | 4.8 % | 25.6 % |
| the perf map writer | 12 % | 6 % |
| TB invalidations a second (`info jit`) | 58,000 | 24,000 |
| the store slow path (every code-page store still takes it, with the compare) | 2 % | 7 % |

The 24,000 invalidations a second left are the 6 % of patches that do
change a value (each retranslating the span loop's blocks), so translation
is still the largest item: that is where "soft immediates" (or a cheaper
retranslation) would go next; `_tlv_get_addr` stayed at 8.8 %.

Also learned on the way, for anyone measuring this game: the fps depends
on the track section (the user's observation from playing it: standing
start 7.4, 13 s in 11.6 before the patch — `RACE_DELAY=` picks the
moment); `-perfmap` costs 12 % of the vCPU on a retranslation-bound
workload (`PERFMAP=0` on the runner for fps runs); `tcg-profile.sh`'s
sample is of whatever runs at WARM, the race needs `RACE_SAMPLE=`.

What remains of the SMC cost after patch 18: the 6 % of patches that do
change a value still invalidate and retranslate (~4,000 TBs/s), and every
code-page store still takes the slow path with the compare; `_tlv_get_addr`
(macOS TLS for `tcg_ctx`, 8.8 % in the race before the patch) (patch 19: 8.8 → 2.5 %) and
`sys_icache_invalidate` per translation are the next translation-side
items; the "soft immediates" translator feature (item 6 below) is no
longer worth its days for this game.

## Patch 19: thread-local reads off the hot paths (2026-09-05, afternoon)

The race after patch 18 still had 8.8 % of its vCPU in `_tlv_get_addr`,
dyld's TLS thunk (every `__thread` access on macOS is an indirect call
into it: two dependent loads to find the thunk, then the key, `mrs
TPIDRRO_EL0`, the slot, the offset). Its callers, summed from the sample's
tree (`build/tcg-profile/moto-smc18-prof/race/sample.txt`):

| caller of `_tlv_get_addr`, in the race | share |
|---|---|
| `get_ptr_rcu_reader` — `rcu_read_lock()`/`unlock()` inside `cpu_physical_memory_get_dirty_flag`, `set_dirty_range`, `is_clean` (three flags), i.e. **five nested lock pairs per store into a page holding code**, all from `notdirty_write` | 39 % (+ 24 % attributed to `thread_start`, the same frames with the unwind lost) |
| `init_ts_info` — the optimizer's `temp_idx(ts)` = `ts - tcg_ctx->temps`, once per operand of every op | 11 % |
| `tcg_op_alloc` + `tcg_emit_op` — two reads per emitted op | 12 % |
| `cpu_tb_exec` — patch 14's per-thread JIT state, once per main-loop TB | 4 % |
| `tcg_gen_*` argument conversion, `tcg_constant_*`, `tcg_temp_new_*` | ~8 % |

A microbenchmark (`scratchpad`, not kept) put the thunk at 1.6 ns per call
back-to-back and showed no pipeline drain on the `mrs`; the cost is the
count: `cpu_exec` holds the RCU read lock for the whole run
(`RCU_READ_LOCK_GUARD()` in `cpu_exec`), so the nested pairs were pure
depth++/-- through TLS, ten thunk calls per code-page store at ~2 million
such stores a second after patch 18 (the game runs 3× faster, so 3× the
stores of the patch-18 count).

**The fix** (`patches/qemu/19-tls-hot-paths.patch`): `include/exec/ram_addr.h`
gets `_rcu_locked` variants of `cpu_physical_memory_get_dirty`,
`get_dirty_flag`, `is_clean` and `set_dirty_range` (the bodies; the
existing names wrap them in the guard), `notdirty_write` uses them, and
`cpu_exec_step_atomic` — the one path that ran translation and the store
slow path *outside* an RCU read section (an upstream hole: `tb_gen_code`
and `tlb_fill` there read RCU-protected memory topology too) — gets a
`RCU_READ_LOCK_GUARD()` declared before its `sigsetjmp`, so the invariant
"the TLB slow paths run inside a read section" holds for every caller.
In `tcg/tcg.c`, `tcg_op_alloc` takes the context, `tcg_emit_op` /
`tcg_gen_callN` / the `tcg_temp_new_*` and `tcg_constant_*` wrappers read
`tcg_ctx` once and pass it down (`tcg_temp_new_ctx`, `tcg_constant_ctx`,
`temp_tcgv_ctx`); `optimize.c`'s `init_ts_info` uses `ctx->tcg->temps`. No
behaviour change, so no property switch: the DOS batteries and the XP
guest stage are the regression guard. Left on purpose: the `tcgv_*_arg`
read in each `tcg_gen_op*_i32/i64` wrapper (one per op, CSE'd; removing
it means a context-taking twin of every `tcg_gen_opN`), `cpu_tb_exec`'s
per-thread JIT state (the per-CPU alternative is wrong under round-robin
with several vCPUs), and `tcg-op-ldst.c`.

**Result**, same settings as `moto-smc18-prof` (perf map on, `RACE_SAMPLE=15`
then the 25-dumps/s probe; `build/tcg-profile/moto-p19-prof/race/`):

| vCPU, in the race | patch 18 | patch 19 |
|---|---|---|
| `_tlv_get_addr` | 8.8 % | 2.5 % |
| `get_ptr_rcu_reader` (self) | 1.6 % | — |
| other (the category the thunk sat in) | 24.4 % | 18.3 % |
| generated code | 25.6 % | 28.3 % |
| translation + lookup | 29.3 % | 31.3 % |
| mid-race fps (25 dumps/s, not saturated) | 19.0 (21.5 in a re-run) | 20.3 |

Standing start, 60 dumps/s, no perf map, both binaries the same afternoon
(`moto-p18-r60` / `moto-p19-r60`): **37.9 → 40.4 fps** (+6.6 %; the
probe delivered 713 dumps in 15 s on the faster run against 791, so it is
near its ceiling again and the number is a lower bound). The remaining thunk calls are
the `tcg_gen_*` wrappers (26+15+12 samples), `tcg_emit_op` (67 — the one
read it still makes), `cpu_tb_exec` (44), `init_ts_info` (30, the
`ctx->tcg` path still pays one for `tcg_malloc`), and an unattributed
122 under `thread_start`.

**Mid-race numbers are not an A/B.** The patch-18 binary was run a second
time with the same settings (`moto-p18-prof`): 21.5 fps where its first
run had 19.0 and patch 19's 20.3 — the 15 s window lands on a different
track section each time (the screendumps show it; the x87 helpers are
5.8 % of the vCPU in two of the runs and 1.0 % in the third, and the
`info jit` deltas over the window differ 4×: 155 k / 419 k / 654 k TB
invalidations). What is robust across all three: the thunk's share
(8.8 % and 9.7 % before, 2.5 % after) and `get_ptr_rcu_reader` (1.6–1.8 %
before, gone). The fps A/B is the standing start (`RACE_DELAY` 0, both
binaries back to back, `PERFMAP=0 FPS_RATE=60`), and at 40 fps the probe
is at its ceiling — the next fps oracle for this game needs a faster
probe (a frame counter in the guest, or the display driver's flip count
in the QEMU log).

## Patch 20: the jump-cache probe inline (2026-09-05, evening)

Item 1 of the list below, the user's pick after patch 19. Every `ret`,
`call *`, `jmp *` and every jump that leaves its page ended its TB with
`helper_lookup_tb_ptr()`: a sync of the globals, `cpu_get_tb_cpu_state`
(in this fork with the x87 / SSE fast-mode bits, runtime state),
`curr_cflags`, the breakpoint check, the jump-cache compare, ~70 host
instructions plus the call — 13.9 % of the vCPU's self time on 7-Zip
(`helper_lookup_tb_ptr` in `7zip-p20-off`), 5.8 % in Moto Racer's race.

**Design** (`patches/qemu/20-inline-lookup.patch`). A generic
`translator_lookup_and_goto_ptr(pc, cs_base, flags)` in
`accel/tcg/translator.c`, fed by the target: i386's `gen_eob` (the
`DISAS_JUMP` exit, after its own hflags / RF resets are stored) computes
the three as TCG ops exactly like `cpu_get_tb_cpu_state()` — hflags, the
eflags bits, `x87_fast_mode << 29`, the SSE inexact bit, CS base + eip —
from `env`, so far jumps (CS and hflags changed by a helper) are as right
as near ones. The generic part hashes the pc (`tb_jmp_cache_hash_func` as
ops), loads the entry, and folds into one 64-bit mismatch word: the
entry's pc ^ pc, the TB's `cs_base` ^ cs_base, `flags` ^ flags, `cflags`
^ `cpu->tcg_cflags`, `singlestep_enabled`, the breakpoint list head, and
the entry's nullness (a null entry is replaced by the current TB as a safe
base for the loads — `movcond`). One `movcond` then picks the TB's
`tc.ptr` or `tcg_code_gen_epilogue`, and `tcg_gen_goto_ptr` (new, the
op exposed) jumps: the main loop's `tb_lookup` is the miss path, which
fills the jump cache — exactly what the helper did when it found nothing;
the helper's own `tb_htable_lookup` on a miss is replaced by the loop's
(the race: `cpu_exec_loop` 1.3 → 1.5 %). **Branch-free on purpose**: a
`brcond` ends a TCG basic block and every live TB temp is spilled to the
stack there and reloaded after, so five compares as branches would have
cost ~13 memory ops; the word costs xor/or. 44 TCG ops, 312 bytes of
aarch64 for a `ret` TB (`build/test/p20-dump.log`), one indirect branch.

Kept on the helper: `-accel tcg,inline-lookup=off` (the oracle),
`CF_NO_GOTO_PTR`, exec / nochain logging at translation time (the helper
logs every TB it chains to), `one-insn-per-tb`, 32-bit hosts, the x86-64
target (its CS64 case: cs_base 0 and a 64-bit pc; not built here). Like a
`goto_tb` chain, a chain of inline hits keeps its cflags until the next
return to the main loop; `-d nochain` or `one-insn-per-tb` toggled at
runtime apply from the next main-loop lookup (upstream's direct chains
behave the same). Breakpoints and gdb single-step are checked at run time,
so those are exact.

**Result**, on vs off, same session, back to back:

| | off (the helper) | on |
|---|---|---|
| 7-Zip compress rating, dict 22 / 23 (MIPS) | 1181 / 1071 | **1325 / 1201 (+12 %)** |
| 7-Zip decompress rating, dict 22 / 23 | 1651 / 1534 | **1765 / 1638 (+7 %)** |
| 7-Zip vCPU: `helper_lookup_tb_ptr` / generated code | 13.9 % / 79.4 % | 0 / 91.5 % |
| the race: the helper / `cpu_exec_loop` | 5.8 % / 1.3 % | 0 / 1.5 % |
| the race, standing start, 60 dumps/s | 39.2 | 39.3 (the probe's ceiling, no signal) |

The game needs a faster fps oracle now (the item added under patch 19).
Follow-ups if they show up in a profile: the flags computation is 12 of
the 44 ops (the SSE term alone 8 — `(exc_flags >> 4) & sse_fast_mode`
would be 4), and the two 64-bit constants (the current TB, the epilogue)
are materialized per TB.

## Patch 21: pinned guest registers (2026-09-05, night — in progress, off by default)

Item 2 of the list below, the user's pick after patch 20; the design is
`docs/18-pinned-guest-registers.md`, the patch `21-pinned-regs.patch`.
`eip` and the eight GPRs live in x20–x28 for the life of a chain of TBs:
loaded by the prologue, stored by the epilogue, stored before a helper
that may read globals (once per TB unless modified again), reloaded after
one that may write them, stored by the `qemu_ld/st` slow path before its
helper (through one shared thunk emitted after the epilogue). The
allocator keeps them in their registers (outputs straight into the
register, `deposit eax, eax, t` in place, a scratch + `mov` only when a
constraint forbids), the liveness pass keeps them live at block ends and
at may-fault ops instead of syncing them, and — the piece the first
numbers asked for — `op T; mov G, T` is coalesced into `op G` when
nothing in between reads or writes G, reads T, may fault or ends the
block (`la_coalesce_pinned_mov`: two thirds of the `mov gpr, tmp` gone,
1799 loads landing directly in the register on a FreeDOS boot). The
optimizer prefers a pinned global as the canonical copy of a value.
Because the callee-saved registers are now the globals', the aarch64
`qemu_ld/st` slow path saves and restores the live caller-saved registers
itself (`TCG_TARGET_LDST_SAVES_LIVE`, `TCGLabelQemuLdst.live`) so the
fast path no longer spills temps around every memory access. The
system-mode prologue moves to `tcg_exec_realizefn`, after the target
created its globals. `-accel tcg,pinned-regs=on` enables it (the
`tcg_pin_globals` flag; **the default is off until the two open items
below are closed**), `QEMU_TCG_OPTS=pinned-regs=on` runs the DOS
batteries under it, `QEMU_TCG_PIN_MAX=n` caps the number of pinned
globals (an experiment knob).

**Bug found on the way, fixed:** `mem_coherent` of a pinned global is
per-path state, and the allocator carried it across labels — a branch
that skipped a store reached the label with the flag saying the `env`
slot was current, and the next `int 21h` saw stale registers: the SSE
battery's `sse-fast=off` run hung at result line 125,484 every time
(DOS looping in the kernel, no exception in `-d int`). Reset at every
block boundary.

**Results, 7-Zip 26.02 `b -mmt1` (M1 Air, same session, dict 22 / 23):**

| | `pinned-regs=off` (upstream's allocation) | on, first build (no coalescing) | on, with coalescing + in-place aliases | on, 7 pinned (`PIN_MAX=7`) |
|---|---|---|---|---|
| compress rating (MIPS) | 1375 / 1245 | 1363 / 1228 | **1421 / 1283 (+3 %)** | 1404 / 1259 |
| decompress rating | 1757 / 1628 | 1796 / 1659 | **2039 / 1853 (+16 % / +14 %)** | 1959 / 1778 |
| 7-Zip's own CPU-frequency loop (MHz) | 3128 | 1574 (the `mov` per ALU op halved it) | 3132 | 3137 |
| vCPU: generated code / helpers flags-cc | 94.6 % / 1.0 % | 91.8 % / 4.1 % | 91.1 % / 4.0 % | 90.7 % / 4.3 % |

The second pass on the hot LZMA page (`7zip-p21-d` vs `7zip-p21-off-d`,
`tools/tcg-hot.py`) corrects the profile's premise: the "guest regs"
class fell from 39.5 % to 9.6 % of the samples and "work" rose from
6.5 % to 32.9 % while the TLB lookup stayed at 42 % — **the 41 % of
samples on register loads was mostly the sampler's skid off the softmmu
chain**, not time the loads cost (they hit L1). The gain is the removed
instructions (the hottest TB, the match-finder compare, 40 → 34 host
instructions before coalescing) and it shows where the loop is not
chain-bound: decompression.

**Open (why the default is off):**

1. A boot with `QEMU_TCG_PIN_MAX=8` (edi left in memory) crashed XP once
   at boot (the safe-mode menu in `7zip-p21-pin8/after.png`); 9 and 7
   pinned booted and ran every time, the DOS batteries pass at 9. A
   reproduction run (`pin8-boot1/2`) was interrupted. Until explained,
   a latent allocator bug is possible in the mixed case (a memory global
   among pinned ones — the state the x86-64 backend would be in).
2. `helper_cc_compute_c` shows 4 % of the vCPU instead of 1 %, at the
   same call sites, 695 of its 824 samples on its first instruction: a
   stall at the call boundary, not in the helper. Not the spills of temps
   around the call (7 pinned leaves two callee-saved registers and the
   share does not move). Untested theory: the `dmb` that follows the
   call (the guest store after it) draining the spill stores;
   `QEMU_TCG_NO_MB=1` on the same run would tell (the run was
   interrupted).
3. Not measured yet: Moto Racer's race (the 60-dumps/s probe is
   saturated at 39 fps anyway — the vCPU split from `RACE_SAMPLE=` is
   the signal), Super PI, FIFA.

**Verified with pinning on (9):** `rep` and SMC batteries (final code),
x87 and SSE batteries (the x87 on the first build, the SSE on the
label-fix build; both rerun on the final code at the end of the session,
see the commit), the DOS host of `scripts/test.sh` boots, XP boots and
runs the benchmark six times. `scripts/test.sh all` runs with the
default (off).

**Follow-ups once on by default:** a cheaper pinned store/reload around
helper calls (9 + 9 instructions inline per call without `NO_RWG`/`NO_WG`
— a thunk pair, or a per-helper "touches GPRs" flag so the x87/SSE
helpers skip it); `cc_dst`/`cc_src` as the next pinning candidates if a
budget ever allows; the x86-64 backend's five callee-saved registers for
the rig.

## The tyre smoke: a second self-patching rasterizer (2026-09-08)

The user's report: in Moto Racer's **software renderer** the game "works pretty
well most of the time now, but when you brake some smoke is emitted from the
tyres and that almost hangs the game". It is the patch-18 pathology again, in a
routine patch 18 cannot help with — and finding it needed a way to measure *one
effect* rather than a lap, because a 20 s sample and an fps probe both report the
mean and the smoke lasts a second.

**The tools that came out of it** (this track owns them):

- `tools/moto-watch.py` — one row per second of a running guest: TB
  invalidations and bytes of code generated (`info jit` at 4 Hz over QMP), a
  screendump a second, and the seconds with the most invalidations named and
  kept. A fourth argument (`4:3`) cycles the bike's own controls over the same
  connection — QEMU serves one QMP client at a time, so the thing that measures
  has to be the thing that drives. `WATCH_TRACE_LOG=` turns QEMU's
  `translate_block` trace on for one whole throttle phase and one whole brake
  phase (`log trace:translate_block` over QMP, the log sliced by the byte
  offsets it noted) and prints the guest pages retranslated in each;
  `WATCH_MEMSAVE=` saves the code pages twice inside the same phase for
  `tools/smc-diff.py`; `WATCH_SAMPLE_PID=` takes a macOS `sample` of one phase.
- `tools/xp-moto-race.sh` gained `RACE_BRAKE=1` (throttle and brake measured
  apart), `RACE_WATCH=<s>` / `RACE_CYCLE=A:B` / `RACE_TRACE=1` (the watcher
  above), `RACE_STAGE=demo` (watch the attract demo, no menus), and passes
  `RACE_SAMPLE` / `RACE_MEMSAVE` through to the watcher so both land inside a
  brake. `tools/tcg-profile.sh` gained `DDFLAGS=` — the adapter's bisection
  register, and `DDFLAGS=32` (`DDF_NO_D3D`) is how a title is put back on its own
  software renderer now that the M7 driver answers its HAL probe. `tools/tcg-fps.py`
  prints its per-second counts under the total.

**The measurement** (M1 Air, `winxp-m7.qcow2`, `-cpu pentium3`, DDFLAGS=32, the
practice race on Speed Bay, throttle and brake cycled 4 s / 3 s and 8 s / 6 s):

| | throttle | brake | at the brake's first second |
|---|---|---|---|
| TB invalidations/s | 31,000–35,400 | 38,100–42,300 | 50,000–51,200 |
| host code generated | 30–36 MiB/s | | **57–69 MiB/s** |

Every brake onset is a peak, in every run (seconds 7, 14, 28, 43 of one run;
14, 28 of the next). The vCPU in a sampled brake: **translation + lookup 32.7 %**
(`liveness_pass_1` 5.6, `tb_invalidate_phys_page_range__locked` 4.3,
`tcg_reg_alloc_op` 2.8, `sys_icache_invalidate` 2.6, `tcg_optimize` 2.2),
generated code 25.4 %, softmmu 3.3 % (the perf map's own 15.2 % on top).

**What braking wakes up.** `translate_block` traced over one phase of each kind
names it — the guest pages retranslated, per second:

| page | throttle | brake |
|---|---|---|
| `0x436000` (the texture span loop, doc's patch-18 section) | 28,400/s | 16,200/s |
| `0x435000` | 3,000/s | **11,600/s** |

and `smc-diff.py` on two captures taken inside one brake says what page
`0x435000` is: at `0x4357f1`–`0x435851`, `shl edx, 5` / `and eax, 0x1f` /
`shl ebp, 0x10` / `mov ax, [ecx*2 + disp32]` / `mov edx, 0xf800f81f` /
`shl edx, 0xb` / `shr ebp, 0xa` / `and eax, 0x3e0` / `and ebp, 0x7c0` /
`add esi, imm32` / `adc ebx, imm32` / `adc cl, imm8` / `cmp edi, imm32` — the
RGB565 channel masks and shift counts of a **translucent span loop**, patched
into the instruction stream. **14 immediate fields per use** (19 imm32, 13 imm8,
5 disp32 across both routines in the capture), and the unpatched capture still
holds the template's `0x12345678` / `0x12` placeholders, so the game rewrites all
of them every time with values that really change: this is the 6 % that
patch 18's compare-before-invalidate cannot skip, and the smoke is made of it.

**The same race on the M7 HAL** (`DDFLAGS=0`, the game's Direct3D 5 renderer,
which our driver now answers — the `D3D: NOT DETECTED` lead of the 2026-09-05
list is closed): 77–120 frames/s, 100,000 draws in 5 s, and the TB invalidate
count does not move at all over a 20 s sample. The software renderer is the
workload; the smoke is its worst minute.

**Not reproduced: the magnitude.** Headless the harness brakes at moderate speed
and the smoke is a thin trail behind the rear wheel (`build/tcg-profile/moto-sw-hard/watch/w15.png`),
worth 1.3–1.9× of translation work, not a hang. What the user sees is presumably
a plume that covers the screen (a hard brake from top speed, or the motocross
tracks' dirt), i.e. the same loop with far more spans. The mechanism is the same
either way; only the number in a fix's A/B needs the bigger repro.

## Patch 24: soft immediates — the block outlives its patches (2026-09-08)

The user's pick after the tyre-smoke finding above. Patch 18 took the 94 % of
code-page stores that rewrite the value already there; this takes the rest, by
changing what a patched operand *is*.

**The idea.** A block at a physical address that four guest writes have thrown
away is translated again with its immediates and displacements emitted as
**host loads of the guest's own code bytes** instead of constants — the field
lives where the guest is writing it, so the guest's store is the update and
there is nothing to keep in step. The block then only has to survive the
write, and for that it carries the list of byte ranges it reads that way: a
store landing entirely inside them changes nothing about the code translated
from the rest of the bytes, so it invalidates nothing.

Two properties make this safe to reason about:

- **Emitting the load is correct on its own.** It reads memory at execution
  time, which is what the architecture says the instruction's operand is. The
  field list is only an optimisation of the *write* side.
- **A field is registered exactly where the load is emitted.** An emitter that
  keeps the constant never registers, so a stale use is not expressible. That
  is why the immediate whitelist is by emitter (ADD, OR, ADC, SBB, AND, SUB,
  XOR, MOV — which is CMP and TEST too, the group-1 table spells them that
  way) and not by opcode: those take the immediate through `gen_load` and
  nowhere else, while the shifts, the jumps, `INT`, `RET` and the SSE lane
  selectors read `decode->immediate` directly in their emitter and must keep a
  constant. Displacements need no whitelist — an address is a runtime value
  anyway — except the two MPX forms that read `a.disp` without going through
  `gen_lea_modrm_1`, which is why registration lives in that function.

**Where the pieces are** (`patches/qemu/24-soft-immediates.patch`):
`accel/tcg/tb-softimm.c` + `include/exec/tb-softimm.h` (the counters, the
per-block field lists in a bump arena reset by `tb_flush`, the coverage test),
`translate-all.c` (`soft_imm_begin` at `restart_translate`, `soft_imm_attach`
after the code is generated), `tb-maint.c` (the counter on the write path, and
`soft_imm_absorbs__locked` at the top of `tb_invalidate_phys_range_fast`),
`target/i386/tcg/` (`gen_soft_field`, and its two call sites), `tcg-all.c`
(`-accel tcg,soft-imm=off`, default on).

**The self-correcting part.** A block that goes soft and is thrown away
*anyway* — patched somewhere the generated code baked in, an opcode or an
operand of an instruction that keeps its constant — is worse off than before:
it pays for the loads and still retranslates. Four such strikes and the
address gives up and goes back to constants. The first build without that rule
showed it: the DOS battery's `smc-same-value=off` control had **104,857**
translations of one block in FreeDOS that could never be absorbed. With it,
that run translates 23 soft blocks and absorbs *more* writes than before.

**The oracle** is `tools/smc-guest-test.py`, extended: 13 DOS cases now,
including the four this patch is about — a memory operand's disp32 rewritten
per call (a texture base), a sign-extended imm8, one instruction whose modrm
*and* whose immediate are patched (the immediate may be read at run time, the
modrm may not), and one store that covers the tail of an immediate *and* the
two opcode bytes after it (which no block can survive, however soft its
immediate). It runs all four combinations of `soft-imm` and `smc-same-value`,
and asserts the path was reached — QEMU's own `soft_imm_block` and
`soft_imm_absorb` trace events, counted — so a battery that stops exercising
the feature fails instead of passing.

**Result, Moto Racer's race** (`tools/xp-moto-race.sh` with the cycled
throttle/brake harness, `DDFLAGS=32`, one binary, `soft-imm` off then on):

| in the race | off | on |
|---|---|---|
| TB invalidations/s, throttle / brake | 31,045 / 36,548 | 0 / 1 |
| host code generated | 30–36 MiB/s | 0.03 MiB/s |
| frames/s (the driver's own flip counter) | 41 mean, 32.4 worst | **58 mean, 44.8 worst** |

The fps probe cannot see this — it saturates at ~52 dumps/s and reports 51.9
against 51.3 — so the number above is the display driver's own
`d3dpt-vga: N page flips in 5.0 s`, which is the guest's real frame rate and
was the item this track's patch-19 section asked for. **The game is now at the
60 Hz flip cap for most of the race**, where before it ran 32–53, so the true
speedup is larger than 41 % and the cap hides it.

A second A/B with the cap taken off (`DDFLAGS=32800`, `DDF_NO_VSYNC`;
`moto-novsync-off` / `-on`) reads 44 → 56 fps mean in the race with the same
invalidation collapse (35,175 / 40,726 a second → 0 / 1). **Treat that pair as
indicative, not as the number**: the `on` run shared the machine with a
`scripts/test.sh host`, which can only have cost it, and the 15 s windows land
on different track sections each time (the patch-19 section's warning). The
clean pair is the capped one in the table.

## Blood: the shift counts patch 24 leaves alone (2026-09-10)

The user's report: Blood (the 1997 DOS Build-engine game, in a Windows 98 DOS
box on `claude98`, 640×480 VESA 2.0) is "sluggish in the starting room,
depending on where you look". Measured on a raw copy of `claude98` with
`tools/win98-game-test.sh` (`GUEST_CMD=$'cd \\BLOOD\nBLOOD.EXE -quick -map
e1m1'` — `-quick` skips the intro, `-map` loads the level, so the crypt is up
~25 s after the DOS box takes the screen with no menu to drive), `EXTRA="-perfmap
-name debug-threads=on"` and `perf record -e cycles:u` on the `CPU 0/TCG` thread:

| facing | frames/s | TB invalidations/s |
|---|---|---|
| the corridor (walls, floor, ceiling, cobwebs) | **9.4** | ~46,000 |
| a bare dark wall, close | **154** | ~41,000 |

The frame count is Blood's own page flipping: it cycles the display start
through eight 480-line pages, so every VBE index-9 write (HMP `trace-event
vga_vbe_write on`, logged with `-D`) is a frame. `tools/tcg-fps.py` cannot be
used for this: with the camera still, Build draws the same frame again and the
probe counts none. The vCPU is ~70 % in QEMU (`tcg_gen_code`, the liveness
passes, `tcg_optimize`, the decoder), 6–11 % in generated code, and the 1 GiB
code buffer is flushed every ~20 s.

**One block is 95 % of it**: `translate_block` over 3 s gave 117,254
translations, 111,890 of them at guest `0x8ae56623` — Build's two-pixel column
loop (`shr ecx, 0x19` … `add edi, 0x280` … `jae 0x8ae56623`). Two `memsave`s of
its page 200 ms apart (`smc-diff.py`) show 36 patched instructions: 17 disp32
(the texture pointers) and 5 imm32 (the steps), which patch 24 can read from the
code bytes, and **14 imm8 shift and rotate counts** (`shr ecx, 0x18 ↔ 0x19`,
`rol eax, 0xe8 ↔ 0xe7`, `shl esi`, `shl ebp`), which it keeps as constants —
`gen_shift_count_1`'s `X86_OP_IMM` case bakes the count in, and a count of zero
even changes the shape of the code. Patch 18 cannot skip them either: the value
alternates. So the block is thrown away while soft, four times, and the address
is marked `giveup` (`soft_imm_note_invalidate`) — after which every per-column
patch of the fields patch 24 *could* absorb retranslates the loop too.
`soft_imm_block` fired zero times in the window. A frame's cost is how many
patches it takes: ~4,900 retranslations per frame facing the corridor, ~265
facing the wall.

**Built the same day, as part of patch 24** (the item 6 leftover below):
`gen_shift_count_1`'s `X86_OP_IMM` case, in a block translated soft, loads the
count's byte from the guest's code with `gen_soft_field` and masks it at run
time, and hands the emitter `can_be_zero` — the path the `CL` form already
takes, so a count patched to zero leaves the flags alone exactly as the
hardware does. Two exclusions, both stated in the code: RCL/RCR on 8 and 16-bit
operands, whose count is reduced modulo 9 or 17 at translation, and SHLD/SHRD,
whose count the decoder reads without recording its address. `gen_IMUL3` joins
`insn_soft_imm_ok` (its immediate reaches it through `gen_load`). Same switch,
`-accel tcg,soft-imm=off`. `tools/smc-guest-test.py` grew four cases — a `shr`
and a `rol` count patched per call over every byte value (so the processor's
mask and the zero counts are both exercised, the carry folded into the result),
IMUL's imm32, and a 16-bit `rcr` that must keep its constant and still be right
— and passes all 17 under the four switch combinations, the soft-imm runs
reaching the path (28–30 soft blocks, 10,000–13,000 writes absorbed);
`scripts/test.sh all` 54 passed, 0 failed.

**And it did not help Blood — which found a second bug, in patch 24 as it
already was.** The corridor went 9.4 → 10.7 fps with invalidations unchanged.
A temporary trace event on the refusals (`soft_imm_miss`: address, size, the
block that refused, whether it was soft, why — built locally, never
committed) showed the column loop at page offset `0x623` *was* soft now — 20
fields, ~238,000 patch writes absorbed in 3 s — and every one of the 55,705
refused writes belonged to a second loop, at `0x170`: `mov [edi], ebx` (two
bytes), then `rol eax, 7` at `0x172`, which is where the code jumps in from
`0x15e`, with `sub esi, imm32`, `sbb edx, imm32` and a texture disp32 patched
per span. Two blocks, `…170` and `…172`, both over the same fields and both
thrown away by every patch — and the invalidation counters were indexed by
`pc >> 2`, so the two shared a slot, each reset it to its own pc with one hit,
neither reached `SOFT_IMM_HITS`, and neither ever went soft: ~18,500
retranslations a second each. The counters now hash multiplicatively
(`soft_imm_hash`). The battery grew a case R for exactly that shape (a
4-byte-aligned routine entered at its top and two bytes in, over one patched
imm32), and — because every case already computed the right answer while this
was broken — the program now prints where four cases' fields are and the
soft-imm runs require each to have been absorbed at least N/2 times. Against
the old hash that check fails (case R: right sum, absorbed 0 times) while N, O
and P pass it, which is the proof that the new shift-count path is really
taken and that the battery can see the collision.

With the hash fixed, the same 3 s trace in Blood's starting room:

| 3 s in the starting room | shift counts soft, `pc >> 2` hash | multiplicative hash |
|---|---|---|
| translations | 115,827 | **1,130** |
| TB invalidations (`info jit` across the window) | ~135,000 | **17** |
| patch writes absorbed | 238,336 | **2,199,574** |
| host code generated | ~200 MB | 0.7 MB |

Both loops now outlive their patches — each of the `0x170` loop's four
patched fields was absorbed 376,124 times in the window, a patch that used to
cost two retranslations. The ~800 writes still refused are someone's data
landing on the second page of a block that spans a page boundary (`why=1`, a
handful each), which patch 24 refuses by design.

**The result**, measured the way the report was (the regenerated patch 24
applied from pristine by `scripts/build.sh`, the temporary trace gone,
`scripts/test.sh all` 54 passed, 0 failed; a fresh raw copy of `claude98`,
Blood's own VBE page flips over 10 s):

| Blood's starting room | before | both fixes |
|---|---|---|
| facing the corridor | 9.4 fps | **131 fps** |
| facing a bare wall | 154 fps | **556 fps** |
| TB invalidations in 10 s | ~460,000 / ~410,000 | 18 / 0 |
| translations in a 3 s trace | 117,254 | 34 (Windows' own code at `0xc00…`/`0xc14…`, none of Blood's) |

Blood has no frame cap, so these are raw rates: the corridor is ~14× faster
and "depending on where you look" is now 131 against 556 rather than 9
against 154.

## Win98 3D: 3DMark 99 (2026-09-11) — the handoff

**Why.** The user found Win98 3D "a bit underwhelming" and then asked for
the first-person test at 60 fps, TCG only (**KVM is not an option: the Mac
has none**). Everything below is on `main`. The benchmark: 3DMark 99 Max on
the `claude98` machine (Win98 SE, DirectX 9.0c, our display driver,
800×600×16, triple buffer, "Pentium III optimizations", `-cpu pentium3`),
driven headless by **`tools/w98-3dmark.sh`** (it clicks through 3DMark over
a USB tablet and screendumps the score; since 2026-09-12 it screendumps
every 5 s and **`tests.txt`** places every `ddi: N frames/s` and page-flip
line by the test that was on screen — read a test's rate there, never off
a bare rate line: the correction below).

**Correction, 2026-09-12 — read before the table.** The "First person"
column is *not* the first-person test. It is the executor's `ddi:` line
for the 40–56 s window after the click, and screendumps every 5 s against
the timeline show that window to be ~5 s of the first-person test's tail
followed by ~9 s of **"Synthetic CPU 3D Speed"**, which presents about two
frames a second; the line prints on the first readback 5 s after the
previous one, so its "frames/s" is a frame count over a window that is
two-thirds empty. The first-person test itself is the 25–45 s stretch,
and **3DMark's own on-screen counter reads 60.0 in it in every run since
`3dmA` (2026-09-11, 19:52)** — the 60 Hz flip cap, like the race test;
with the vertical blank off it does 90–99 fps on the patch 44 build and
95–105 on patch 45. Every "first person" number below therefore tracked
the CPU test (which the CPU 3DMarks score measures honestly), the
profiles cut at 40–56 s were mostly of that test, and the levers picked
from them were picked for it. **The user closed the optimization work on
2026-09-12** ("we don't need any more optimisations"): both game tests
are at the cap on the Ryzen. What is still open is the Mac (items 1 and 2
below): the aarch64 build of patches 39 and 45 has never been compiled,
and the Air is the machine the track is named for.

| Step | 3DMarks | CPU 3DMarks | "First person" (see the correction above) |
|---|---|---|---|
| before (the user's own number too) | 3334 | 10969 | 4.5 fps |
| **patch 35** — a per-page 64-chunk code map: data writes to a page with code at both ends stop walking its TB list | 5894 | 11648 | 13.6 |
| **patch 36** — a 16-byte SSE operand stored once, not as two halves read back by a 16-byte load | 5912 | 13549 | 15.4 |
| **patch 37** — x87 at PC=24 skips the inexact bookkeeping once PE is sticky (TB flag bit 2) | 5917 | 14690 | 16.2 |
| **patch 38** — the inline lookup takes the TB's own x87/SSE mode bits as constants | 5932 | 15389 | 17.1 |
| **patch 39** — `vec_allsign_i32`: the SSE lane check branches on a register | 5950 | 15940 | 17.5 |
| **executor** (`5d07018`) — DX7 indexed draws hand DXVK only their vertex range | 6014 | 15712 | 18.4 |
| **patch 41** — the translator's 13.6 KB context not zero-filled | 6012 | 15483 | 18.4 |
| *(evening, same box, clean re-run of the above)* | 6013 | 15589 | 18.2 |
| **patch 42** — the jump cache survives a TLB flush (generation-stamped, revalidated by physical page) and holds 65,536 entries | — | — | 19.0 |
| **patch 43** — a non-jump end of block chains when no interrupt is pending; a control-word change rebuilds the lookup's mode bits | — | — | 19.1 |
| **patch 44** — a CR3 write retires the TLB; a miss reuses the retired entry once its page-table entries check unchanged | 6003 | 16295 | 19.0 |
| the same with `tlb-retire=off` (the A/B) | 6012 | 16080 | 18.9 |
| **all three, `DDFLAGS=32768`** (vertical blank off: the CPU-bound number) | — | — | **26.2** |
| **patch 45** (2026-09-12) — the x87 shadows at PC=24 are binary32: an op with PE sticky is one `mulss`-class instruction plus a range check, no rounding pass | 6005 | 16899 | 19.5 |
| the same, `DDFLAGS=32768` | — | — | **27.8** |

CPU 3DMarks moves ±2–3 % between identical runs; the first-person fps is
the stable number (±0.2 between identical runs). The race tests sit at the
**60 Hz flip cap** from patch 35 on (`DDFLAGS=32768` turns the vertical
blank off: +3 % before, more now). The two rows with no score are runs a
rebuild overlapped after their first-person window; the last two are clean.

**The evening session (2026-09-11, patches 42–44): what the counters said.**
A fresh whole-run profile of the first-person window (`tools/tcg-perf-cut.py`)
put the vCPU thread at 98 % of wall time and the executor + DXVK under 1 %
of it — the 3D path is not the bottleneck at all, so an asynchronous
executor or a lazier readback would buy nothing, and every lever is TCG.
The generated code was 76 %, and by instruction form
(`tools/tcg-form-weights.py` over a memsave of the hot pages): integer
memory ops 32 % (a `mov` with a memory operand alone 27 %, 1.8 samples per
instruction against 1.2 for a register form), x87 memory forms 20 % and
register forms 4 % (2.9 and 2.6 per instruction), SSE memory 13 % and
register 9 % (3.1 / 2.8), branches 16 %. The host side, by `info jit`
counters taken twice inside the window (`w98-3dmark.sh JIT_SNAPS="43 53"`):
**2,400 CR3 writes a second, every one with the same value**, from one VMM
routine (`Begin/End_Critical_Section`, then `mov cr3, [current]`) whose
callers map or unmap *one page* and flush the whole TLB — Windows 98's page
fault handler (3DMark commits and frees buffers every frame; the ring-3
faults were 3DMark's first touch of fresh buffers and KERNEL32's heap);
after each: every jump-cache entry gone (11.7 M hash-table lookups per 10 s,
mostly conflicts in 4,096 entries), every TLB entry gone (4.2 M page walks
per 10 s, ~185 per flush), and the main loop entered 1.85 M times a second
from generated code — the VMM's ring-0 entry (`mov ds`, `mov es`, `sti`,
the shadow's end: five per VxD call), `popf` in its timer read, and MSVC's
`_ftol` (two `fldcw` per call, 220,000 calls a second in the engine) whose
blocks missed the inline probe because patch 38's mode constants were the
leaving block's. Patches 42 (jump cache kept and grown: 18.2 → 19.0), 43
(the round trips chained: 5.7 M entries per 10 s from 12.3 M, 19.0 → 19.1)
and 44 (95 % of the walks replaced by a check of the page-table entries:
within noise) followed. **The walks were cheap on this host** — the 13 %
their symbols suggested was mostly the slow path's own entry, which a
reuse pays too. Also measured on the way: the driver's vertical-blank wait
polls FRAMES 34,000 times a second in this test (each poll a
`QueryPerformanceCounter`, a ring transition on 9x), which is wall-clock
time the benchmark spends waiting for the 60 Hz edge, not a lever, but it
means part of the ring-0 traffic above is inside the wait; and the DP2
window's doorbell is ~1,000 MMIO writes a second, nothing.

**Where the time goes now** (the first-person window, patch 39 profile): 74 %
guest code, spread flat — MAX-FX's Pentium III DLL (`e2_PentiumIII_cpu_mfc.dll`,
loaded at `0x1580000`) 42 %: integer memory ops 13 % (the softmmu TLB check,
~60 host bytes each), SSE 15 %, x87 5 %, calls/returns 7 %; 3DMARK.EXE's x87
lighting code 13 %; MAX-FX's C++ core `e2mfc.DLL` 9 % (virtual calls and
`ret`); host ~26 %: dispatch ~5 %, softmmu ~4 %, TLB wipes of *used* tables
~1 % (110 GB of 128 KiB `memset`s a run, ~2000/s), DXVK texture uploads.

**The method, which is the reusable part:**
- **Profile the whole run and cut it by time** (`w98-3dmark.sh <n> whole`
  with `EXTRA=-perfmap`; `click.txt` and the `ddi:` lines place the tests).
  One 10 s window lands on one test only and misled twice — and a
  window taken from the rate lines' own timing misled for a whole day
  (the correction at the head of this section): take a test's seconds
  from `tests.txt`, which names the test on screen every 5 s.
- **perfmap entries are per guest instruction**, not per TB. Group the
  samples by instruction *form* (memory vs register operand, SSE / x87 /
  integer) and look at samples per instruction: the uneven form is the
  bug (a memory-operand `addps` cost 10 samples, the register form 2.7 →
  patch 36).
- **`EXTRA="-d in_asm,out_asm -dfilter <pc>+<len> -D <file>"`** shows the
  real host code of the hot loop (markers are page offsets under CF_PCREL).
- **A module at an unknown base** (3DMark's DLLs all ask for 0x10000000):
  fit the hot guest PCs to every candidate DLL at every 64 KiB base against
  its instruction boundaries.
- **perf cannot unwind out of glibc's AVX `memcpy`/`memset` loops**:
  `tools/memtrace.c` (a preload counting ≥ 4 KiB calls per return address)
  named the executor's 32 GB of vertex copies and the translator's memset.
- **A/B before crediting**: the drivers' byte-loop `memcpy` looked like 30 %
  of a profile window and changed nothing when fixed (the walks' cost
  landing on its stores). `-perfmap` itself barely moves the score.
- **Count before profiling** (2026-09-11 evening): `info jit` twice inside
  the window (`w98-3dmark.sh JIT_SNAPS="43 53"`) gave the TLB flush rate,
  the refills, the jump-cache hit split and the main-loop entries as
  numbers a profile only hints at; two temporary histograms (the pcs
  entered from the main loop, the CR3 writers and their callers through
  the guest stack) named the VMM routines, and `memsave` of the hot pages
  + capstone (`tools/tcg-form-weights.py`) named the instructions. A
  user-mode page is there only in its own process: dump inside the test.
  Every such counter was removed before the patches were cut.
- **`QEMU_TCG_OPTS=<switch>=off` on `win98-game-test.sh`** is the A/B for
  an accelerator switch; a second `-accel` on the command line is ignored.

**Lessons that cost a round each:**
- **A new TB flag has two places**: `cpu_get_tb_cpu_state` *and* patch 20's
  `gen_lookup_and_goto_ptr`, which builds the flags as TCG ops. Patch 37
  without the second one lost 25 % (every indirect jump into a sticky TB
  left through the epilogue; guard exits 0 — `info registers` now counts
  `x87-fast guard exits`).
- **Upstream already skips clean MMU indexes** in a TLB flush (`c.dirty`):
  "don't wipe the unused tables" (patch 40) was a no-op and was dropped.
  `n_used_entries` is the resize heuristic's and is not exact.
- `build.sh`'s prepare wipes unqueued edits in `qemu/`: keep a patch file
  of work in progress before any `build.sh`.
- **A patch's diff is cut against the *prepared* tree**, not the tree you
  worked in: patch 40's leftover lines were still in `cpu.h` after it was
  dropped, became context in patch 44's hunk, and the forward-apply
  refused it (2026-09-11 evening).
- **A flush event is `partial flushes / dirty indexes`**, not `/ 8`:
  `info jit` counts mmu indexes cleaned, and Win98 dirties three.
- **Patch 38's constants are wrong for the block that changed the control
  word** — a `fldcw` block must rebuild the mode bits from env, or every
  `_ftol` takes the main loop twice (patch 43's second half).
- **Page walks were not 13 %** — the symbols that added up to it are the
  slow path's entry (`mmu_lookup`, `probe_access_internal`), paid by a
  refill of any kind; replacing 95 % of the walks moved 0.1 fps.

**User rules for this work (2026-09-11):** only optimizations that could
help *any* guest — nothing title-specific (no 3DMark DLL replacements, no
per-game hooks; a guest DLL in game folders only for shared runtimes or
middleware); exact (bit-identical) work before the opt-in **relaxed
floating-point mode**, which the user agreed to in principle (a per-machine
`fp-relaxed` switch, off by default: SSE without its per-op checks, x87
without PC=24 rounding and window checks but with a correct out-of-range
conversion, no FIP/FDP stores) and deferred.

## Patch 45: the x87 shadows at PC=24 are binary32 (2026-09-12)

Item 3 of the list below. Patch 06 keeps the x87 stack as host doubles
across a block and, at PC=24, rounded every result to 24 bits through
`cvtsd2ss` + `cvtss2sd` + an xor on the dependent chain after the op and
its window check. Now (`x87-shadow.c.inc`, "PC=24" at the head of the file)
the mode-2 shadows are binary32 in eight i32 globals of their own
(`cpu_x87_ss[]`, `env->x87_ss[]`; a small `X87SV` value type carries either
representation through the emitters), and with PE already sticky (patch
37's TB flag, the state a game's loop runs in) an op is `addss` /
`subss` / `mulss` / `divss` / `sqrtss` plus one range check. Exactness:
correctly rounded to 24 bits *is* the x87's PC=24 result while the result
has a binary32 exponent; a result that would not — overflow, underflow,
and **the lowest binade, excluded on purpose**: a value just below 2^-126
rounds up to 2^-126 in binary32 (fewer bits there) but not at 24
significant bits with the x87's 15-bit exponent, and only that binade can
tell the two apart (operands may sit in it, results may not) — takes the
slow path. While PE is still to be decided the operands are widened and
the old binary64 path runs, its residuals feeding PE and its rounding to
24 bits being the result. `fld m32` is the operand's own bits after the
zero-or-normal check, `fst m32` the shadow's bits with no check and no
flag, m64 loads round through one conversion, `fist` / `frndint` /
`fst m64` widen once and keep their binary64 code, `fild` narrows its
exact binary64 integer, a reload from `fpregs[]` accepts exponents
−126..127, and the unwinder (`tcg-cpu.c`) converts from binary32 for a
block whose TB flags say mode 2.

**Measured**: the 40–56 s window 26.2 → 27.8 fps (vertical blank off),
19.0 → 19.5 (on) — which is the CPU 3D Speed test's window, not the
first-person test's (the correction at the head of the "Win98 3D"
section, found the same day); the first-person test itself 90–99 → 95–105
fps uncapped, 60.0 capped before and after; CPU 3DMarks 16295 → 16899,
3DMarks 6005 at the cap. The race windows +4 %.

**The battery had a hole**: `tools/x87-guest-test.py` runs `fninit`
before every case, so no block of it was ever translated with PE sticky —
patch 37's whole variant, and now this one's arithmetic, ran only under
3DMark. Every control word is now swept twice, the second time with 1/3
computed before each case (bit 15 of the table entry, `do_op`), and the
two bench loops set PE first for the same reason (a loop is one block
chained to itself and runs for ever in the variant it was translated
for), and a third loop, `X87BEN2S`, is the single-precision shape of
Direct3D code: 0.33 s for 20 M iterations of seven ops, PC=53's own
number, where the m64 loop at PC=24 takes 0.44 (0.38 before the sticky
variant was the one measured; it widens every store and narrows every
load, and the BIOS tick is 55 ms). 709,893 result lines identical on/off.
Unwinding with binary32
shadows outstanding has no DOS test (a real-mode program cannot fault
mid-block under QEMU); the Windows guests page-fault inside x87 blocks all
the time and the 3DMark run above is the evidence so far.

## Next steps, in order

**Closed by user decision, 2026-09-12** (the correction at the head of
the "Win98 3D" section): the game tests run at the 60 Hz cap on the
Ryzen, so items 3–6 below are not pursued; items 1 and 2 (the Mac) are
what remains, and they are verification, not optimization.

**From the Win98 3D session (2026-09-11), in order** — the section above:

0. ~~**Measure the first-person test with `DDFLAGS=32768`**~~ — done:
   **26.2 fps** against 19.0 with the vertical blank on. The wait for the
   60 Hz edge is a quarter of every frame at this speed, so the fps with
   it on will move in steps as frames cross refresh periods; judge the
   remaining work by the vertical-blank-off number (the 60 the user asked
   for means the CPU-bound frame under 16.7 ms, i.e. 2.3× from here).
1. ~~**On the Mac, first: `scripts/build.sh`, then `tools/sse-guest-test.py`.**~~
   — **done on the Air, 2026-09-12**, patches 35–45 built clean. Patch 39's
   aarch64 encoding compiles and is exact: the SSE battery's 546,425
   result lines are identical on/off (packed 12.0×, scalar 3.6×, MMX 3.6×,
   clamp+cmp 7.3× — the last two on the aarch64 fallbacks the docs called
   unvalidated). The x87 battery with patch 45 is identical over its full
   709,893 lines (the PE-set sweep included); SMC 18/18 in every
   `smc-same-value` × `soft-imm` combination, rep, PIT, ATAPI, MIDI and
   pad all pass, and `scripts/test.sh host` is 33/33. Its XP guest check
   is Linux-only (`mkfs.fat`, `sfdisk`) and did not run. **One gap
   against the Ryzen**: the single-precision x87 loop at PC=24
   (`X87BEN2S`, the shape patch 45 targets) takes 0.49 s here against
   PC=53's 0.38 s — the Ryzen has the two equal at 0.33 s — and the m64
   loop at PC=24 0.71 s (Ryzen 0.44 s). Binary32 on aarch64 does not yet
   reach PC=53's speed; not profiled.
2. **The two game tests on the Air** (`tools/w98-3dmark.sh <name>`, then
   read `build/w98game/<name>/tests.txt` and the scores off `score.png`),
   to know what TCG on aarch64 makes of the same patches: whether the
   race and the first-person test reach the 60 Hz cap there, and with
   `DDFLAGS=32768` what they do uncapped (95–105 fps on the Ryzen).
   **Capped half done, 2026-09-12**, on the user's `win98-2` (English 98
   SE, DirectX 9.0c, our driver, 800×600×32, `TDM_DIR=\PROGRA~1\3DMARK~1`),
   patches through 45: **both game tests at the 60 Hz cap** — race 59.8,
   first person 59.8 fps, the latter placed at 36–47 s by `tests.txt`'s
   classifier and checked by eye (3DMark's own counter reads 60.17) —
   **6001 3DMarks, 16085 CPU 3DMarks** against the Ryzen's 6005 / 16899.
   The same image before patch 45 scored 5968 / 14613, so 45 is worth
   +10 % CPU 3DMarks here too. **Uncapped (`DDFLAGS=32768`), same image
   and build: 9235 3DMarks, 16494 CPU 3DMarks; race 95.6 fps (windows
   89.7 and 101.5); first person ≈ 85 fps** (the three windows wholly
   inside it, 31–46 s after the click, read 87.9 / 82.7 / 84.2; 3DMark's
   own counter in the 43 s shot says 92.38) — some 10–15 % under the
   Ryzen's 95–105, and far clear of the cap either way. **A flaw in
   `tools/w98-3dmark-tests.py`, open:** that run's `tests.txt` gave the
   first-person test no rate, because the shot at ~32 s is missing and the
   report only joins shots up to 7 s apart, so the test split into a
   27–27 s run and a 37–48 s one and the report took the first; the
   numbers above are from the raw `ddi:` lines and the screendump. Joining
   across one missing shot, or taking the longest `f` run, would fix it.
3. ~~**x87 at PC=24 as binary32**~~ — **patch 45, 2026-09-12** (the
   section below): first person 26.2 → 27.8 fps with the vertical blank
   off, 19.0 → 19.5 with it on, CPU 3DMarks 16295 → 16899 — numbers of the
   CPU 3D Speed window, see the correction at the head of the "Win98 3D"
   section; the first-person test itself went 90–99 → 95–105 fps uncapped
   and was at the 60 Hz cap before and after.
4. **The TLB chain's two env loads as immediates**: the mask and table of
   an mmu index are constants once the table's size is fixed (patch 16
   floors it, patch 44 clears it by filled entries so a bigger fixed size
   costs nothing per flush) and there is one vCPU; two loads and an
   instruction fewer on 65 % of the generated code.
4b. **Two-page blocks linkable**: 127,000 unlinked `goto_tb` exits a
   second are direct jumps to a block spanning two pages, which the main
   loop refuses to chain; an inline check of the second page's mapping at
   the block's entry would let them link. And the x87 / SSE slow blocks
   exit to the next instruction (`rsqrtps` in the engine's loop, ~100,000
   a second): they could chain too.
4c. ~~**Smaller TLB wipes of used tables**~~ — patch 44 clears only the
   filled entries; the x87 per-block reload (a double per register plus a
   validity mask across blocks) stays open behind item 3.
5. **x86-64 pinned registers** (doc 18's follow-up: the backend lists none
   yet — rbx, rbp, r12, r13, r15 are free): the largest general lever left,
   but patch 21's two open items come first.
6. **The relaxed floating-point switch**, when the user asks for it.

Done on the way: patch 13 (`-perfmap` on Darwin), patch 14 (the
redundant macOS W^X toggles: Super PI 1M 1:36.2 → 1:25.3), and from the
Moto Racer profile patches 15 (TB invalidation: the vAPIC ROM page
storm, per-page code ranges, no jump-cache flush per TB) and 16 (the
4096-entry TLB floor) — the game's vCPU went from 14 % to 57 % generated
code; then the profiler's epoch fix and the blit finding (the correction
in the Moto Racer section), which puts a REP MOVS fast path first. The user
picks the next item (the track was opened profile-first); the
recommended order, each with M8's methodology (an on/off switch as the
oracle, both guest batteries identical, `scripts/test.sh all` green,
7-Zip / Super PI / D3DGAME9 profiled before and after with the tools
above):

0. **The games, measured where the player plays them** (a day): FIFA 2000
   headless with the player's disc order (a `CDS_SLOTS=` or the M7
   scripts' layout), Esc past the video, the match via
   `tools/xp-fifa-match.sh`'s clicks, `tools/tcg-fps.py` (blind to frames
   presented through the 3D device — use the executor's frame counter or
   `PLAYER_DUMP_OUT` cadence there); Moto Racer's race on `winxp` vs
   `winxp-m7`; Super PI before/after patches 15/16. And hand M7 the
   `D3D: NOT DETECTED` lead — Moto Racer's Direct3D 5 HAL probe against
   our driver would take the software rasterizer out of the picture.
0b. ~~**REP MOVS / STOS fast path** (patch 17)~~ — done 2026-09-05, the
   section above (7-Zip / Super PI not re-measured: neither is
   `rep`-bound; the DOS batteries and the XP guest stage are the
   regression guard).
1. ~~**Inline the TB lookup for indirect branches**~~ — patch 20
   (2026-09-05): 7-Zip +12 % compress / +7 % decompress, the helper gone
   from every profile; the section above.
2. **Pinned guest registers across chained TBs** — patch 21, doc 18,
   the section above: built, 7-Zip decompress +15 % / compress +3 %,
   **off by default** until its two open items are closed: (a) the
   `PIN_MAX=8` boot crash reproduced or explained (`QEMU_TCG_PIN_MAX=8
   BOOT_WAIT=90 WARM=5 SECS=5 tools/tcg-profile.sh ~/vms/winxp.qcow2
   pin8-boot`, then `-d int` / a bisect over the allocator changes),
   (b) the flags-helper skid (`QEMU_TCG_NO_MB=1` on the 7-Zip run first).
   Then the race / Super PI / FIFA numbers, `tcg_pin_globals = true`,
   and the follow-ups listed in the section.
3. **Shorter TLB chain**: the per-mmu-index mask/table pair loaded once
   per TB (or pinned) instead of per access — one dependent load less
   per guest memory access. The probe's `pinned` kernels say this is
   worth 0 % on dependent loads and 10–20 % on independent ones; fold it
   into 2 if it comes for free, don't make it a patch of its own.
   - **The VM design's risk number** (can go first, it is a day): a
     counter of distinct guest pages touched per second and the
     `tlb_fill` rate on 7-Zip / FIFA 2000 / D3DGAME9 (a `-d` line or a
     QMP query, temporary), to predict the nested-TLB penalty the probe
     measured (~21 ns per 4 KiB miss beyond 12 MiB of random working
     set). With that number the "TCG inside an HVF VM" port (the probe
     section above: ~45 KLOC freestanding + cputlb rewritten as the
     fault-driven mirror + a mailbox protocol, 4–8 weeks) becomes a
     decision instead of a bet; design doc 18 either way.
4. **Carry at TB entry**: inline the ADC/SBB/shift cases of
   `gen_prepare_eflags_c`, or carry the static cc_op into the TB lookup
   key (the high half of `cs_base` is free in 32-bit mode) so a TB knows
   the flags state it is entered with. ~3.5 %.
5. **Barriers off on one vCPU** (~4 %): only after an audit of every
   reader of guest RAM outside the BQL; as a machine property, not an
   env var.
6. ~~**Self-modifying rasterizers, the changed-value half**~~ — **patch 24**
   (2026-09-08, its section above): soft immediates, the first of the two
   shapes below, chosen by the user. The race 41 → 58 fps, invalidations
   36,500/s → 1/s. What is left of the idea, if a workload ever asks: the
   fields are per block and only on its first page, `CF_PARALLEL` is left
   alone, and the whitelist is eight emitters — a guest that patched the
   operand of a shift or a jump would still retranslate. **A workload asks
   (2026-09-10): Blood**, whose column loop patches imm8 shift and rotate
   counts per column (the section above: 9.4 fps facing the corridor, 154
   facing a wall, ~40,000 retranslations a second). **Built 2026-09-10**:
   soft shift / rotate counts and `gen_IMUL3` in patch 24, four new cases in
   `tools/smc-guest-test.py` (the section above). What is still left: jump
   targets, the SHLD/SHRD count, RCL/RCR's 8/16-bit counts, the fields past a
   block's first page. For the record, the two shapes were:
   - **Soft immediates** (days) — built, and simpler than costed here: the
     fields are read from the guest's own code bytes rather than from a pool
     the store side has to refresh, so there is no pool and nothing to keep in
     step.
   - **A cheap-translation mode** (half a day): a TB invalidated more than N
     times is retranslated with `tcg_optimize` and the liveness passes skipped.
     Bounded by their share — `liveness_pass_1` 5.6 %, `tcg_optimize` 2.2 %,
     `init_ts_info` 0.9 % — against worse code in the block that runs the spans.
   Also left:
   ~~`_tlv_get_addr` 8.8 % in the race~~ — patch 19 (it was mostly the
   store slow path's nested RCU locks, not the translator): 2.5 % left.
