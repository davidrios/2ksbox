# The literature spikes of 2026-09-16 (doc 23, last section)

`spike-2026-09-16.diff` is the whole spike against the patched QEMU tree
(the queue applied by `scripts/prepare-qemu.sh`); `apply-ab.py`,
`apply-b2.py` and `apply-d.py` are the edit scripts that produce it, in
that order. Switches: `-accel tcg,jump-table=on|off` (A),
`irq-check-backedge=on|off` (B), `ras=on|off` (D); `SSES_NOCHECK=1` in the
environment is the inexact ceiling experiment (C). One directory per run
with `tools/specbench/run.sh`'s result lines and the programs' own output;
`pinned-7/` has the serial log and the screendump of the rebooted guest.
