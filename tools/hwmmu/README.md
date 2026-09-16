# hwmmu — gauging the hardware-MMU design before building it (track/m9-hwmmu)

The design (docs/tracks/m9-tcg-aarch64.md, "The HVF EL1 probe"): run TCG's
Arm output inside a Hypervisor.framework VM whose stage-1 tables mirror the
x86 guest's page tables, so a guest load is one host load instead of the
softmmu chain. The probe priced every primitive; this directory prices the
design on the real workloads, in two halves that multiply:

- **`census.c`**, a TCG plugin: what a workload does to memory —
  instructions, loads, stores, distinct 4 KiB pages per second and per
  window of 65,536 accesses (the nested TLB holds 3072: a window beyond
  that misses), and the pages touched / written for the first time (a
  mirror fill / a dirty upgrade each). One line per second on the plugin
  log. Build and use:

  ```sh
  cc -O2 -shared -fPIC -undefined dynamic_lookup -I qemu/include/qemu \
     $(pkg-config --cflags --libs glib-2.0) tools/hwmmu/census.c -o build/hwmmu/libcensus.dylib
  QEMU_EXTRA="-plugin $PWD/build/hwmmu/libcensus.dylib -d plugin -D $PWD/build/hwmmu/census-cpu.log" \
     OUT=build/hwmmu/runs RUN_WAIT=8000 tools/specbench/run.sh ~/vms/winxp.qcow2 default
  ```

  The plugin slows the guest about five times; every count is exact
  regardless, and the per-second lines are read against the phases of
  the run (`run.sh`'s log says when each program was started).

- **The probe's workload-shaped kernels** (`tools/hvf-el1/bench.S`,
  `mix4`, `mix12`, `copy`): the chase and sum kernels were the two
  extremes; these put K ALU ops behind each independent load, and a store
  behind each load, so the chain's extra instructions are paid where an
  out-of-order core would hide them. `tools/hvf-el1/build.sh` then
  `build/hvf-el1/hvf-el1 build/hvf-el1/payload.bin`, alone on the machine.

The reading is in docs/tracks/m9-tcg-aarch64.md ("Gauging the gain").
