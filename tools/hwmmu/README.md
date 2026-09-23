# hwmmu: pricing the hardware-MMU design on real workloads (M9)

`tools/hvf-el1/` priced each primitive of running TCG's output in a
Hypervisor.framework VM with the guest's page tables mirrored. This
directory prices the design on real workloads, as two halves that
multiply. The reading (a projected 1.05–1.20x) and the decision to park
the design are in `docs/tracks/m9-tcg-aarch64.md`, "Gauging the gain";
the tables are in doc 22 §8.2 and the raw data in `docs/22-data/hwmmu/`.

**`census.c`, a TCG plugin**, counts what a workload does to memory:
instructions, loads, stores, distinct 4 KiB pages per second and per
window of 65,536 accesses (the nested TLB holds 3072 entries; a window
beyond that misses), and pages touched and written for the first time
(a mirror fill and a dirty upgrade each). It writes one `census` line per
second to the plugin log, plus a `reuse` line of page reuse distances in
power-of-two buckets.

```sh
cc -O2 -shared -fPIC -undefined dynamic_lookup -I qemu/include/qemu \
   $(pkg-config --cflags --libs glib-2.0) tools/hwmmu/census.c -o build/hwmmu/libcensus.dylib
QEMU_EXTRA="-plugin $PWD/build/hwmmu/libcensus.dylib -d plugin -D $PWD/build/hwmmu/census-cpu.log" \
   OUT=build/hwmmu/runs RUN_WAIT=8000 tools/specbench/run.sh ~/vms/winxp.qcow2 default
```

The plugin slows the guest about five times; the counts stay exact.
Split the per-second lines by the run's phases (`run.sh`'s log says when
each program started) with `phases.py <census log> <shell_after_s>
name:start:end …`.

**The probe's workload-shaped kernels** (`mix4`, `mix12`, `copy` in
`tools/hvf-el1/bench.S`) put ALU work behind each independent load, and
a store behind each load, so the softmmu chain's extra instructions are
paid where an out-of-order core would hide them. Run the probe alone on
the machine (`tools/hvf-el1/README.md`).

**`project.py <probe results> name:Ginsn:Mld:Mst:wall_s …`** multiplies
the two, charging each load and store the probe's softmmu-minus-direct
difference at the 64 KiB, 4 MiB and 8 MiB working sets. `project.py
<probe> --reuse <census log> <shell_after_s>
name:Ginsn:Mld:Mst:wall_s:start:end …` instead charges each access the
row its own reuse distance puts it on. It counts nothing else: not TLB
refills, CR3 flushes or the helpers' own accesses.
