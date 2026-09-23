# hvf-el1 — the Hypervisor.framework EL1 probe (M9)

Measures whether TCG's Arm output could run inside a Hypervisor.framework
VM whose stage-1 tables mirror the x86 guest's page tables, so that a
guest load is one host load. The design, the numbers and the verdict
(feasible, then abandoned for the time being by user decision) are in
`docs/tracks/m9-tcg-aarch64.md`, "The HVF EL1 probe" and "Gauging the
gain"; this file says what the probe does and how to run it.

It is a bare-metal aarch64 guest (Rust, `aarch64-unknown-none`, 37 KiB,
`payload/`) plus a host program (`host/`) that creates the VM, shares a
64 MiB arena with it at the same address in both worlds, and services
its exits. The guest brings up the MMU (4 KiB granule, 48-bit VA),
installs exception vectors, builds an x86-style two-level page table in
"x86 RAM" and mirrors it lazily into a 4 GiB VA window from the
data-abort handler (one ASID-tagged stage-1 root per x86 CR3). It then
measures:

- a VM exit (HVC; an MMIO load through a stage-2 miss) against an in-VM
  exception and an in-VM function call;
- the first touch of a window page, the hardware TLB miss after it, an
  x86 #PF delivered to a resume point, the dirty-bit upgrade of a clean
  page, and a CR3 switch with the tables kept against flush-and-refault;
- the load kernels of `bench.S` through the window against the exact
  sequence `tcg/aarch64` emits for a softmmu load (with a TLB that always
  hits), 64 KiB to 32 MiB working sets, dependent and independent, plus
  the workload-shaped `mix4` / `mix12` / `copy` kernels `tools/hwmmu/`
  uses — and the same kernels natively in the host for the baseline;
- running code the host wrote, self-patching without a W^X toggle, and
  the latency of a host-thread kick to the guest's IRQ handler;
- the `rep movsd` blit loop of a 2D game (`exp_movs` / `native_movs`):
  TCG's loop transcribed from a `-d out_asm` log, with pinned registers,
  with direct window accesses, and a per-page-run copy — with `env` both
  in the identity map and in the window, because a store to block-mapped
  memory followed by one through a 4 KiB page costs ~2 ns extra per pair
  in the VM (the `diag3` lines).

```sh
tools/hvf-el1/build.sh                     # payload + host, signed with hv.entitlements
build/hvf-el1/hvf-el1 build/hvf-el1/payload.bin
build/hvf-el1/hvf-el1 x --native-only      # only the host baseline
```

macOS on Apple Silicon only (`kern.hv_support`), ~2 s, alone on the
machine. Output is `key: value` lines; reference runs from the M1 Air are
`results-m1air-2026-09-05.txt` and `results-movs-m1air-2026-09-05.txt`.
It is a measurement, not a regression guard, so it is not in
`scripts/test.sh`. On a guest fault the host prints the vCPU state and
the guest's `ESR_EL1` / `FAR_EL1` / `ELR_EL1`; `llvm-objdump -d` on
`build/hvf-el1/payload/aarch64-unknown-none/release/payload` maps the
addresses.
