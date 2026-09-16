#!/usr/bin/env python3
"""project.py -- the hardware-MMU design's projected gain on a workload.

    tools/hwmmu/project.py <probe results> name:Ginsn:Mld:Mst:wall_s [...]

Per workload: the guest's instruction count and its loads and stores (from
census.c through phases.py) and the wall seconds the same work takes at
full speed (the run without the plugin), so the access rates are at full
speed. Each load is charged the difference between the probe's softmmu
and direct kernels (`mix4`: an independent load with four ALU ops behind
it, the throughput case translated code is closest to), each store the
`copy` pair's difference less the load's (`indep`), at the 64 KiB, 4 MiB
and 8 MiB working sets -- the optimistic, middle and pessimistic rows for
a workload whose 64K-access windows hold under 2000 distinct pages. The
projection is the fraction of each second those differences add up to,
and the speed-up that removing it would be. It counts nothing else: not
the TLB refills and CR3 flushes the mirror would also change, not the
helpers' own accesses.
"""
import re, sys
probe = open(sys.argv[1]).read()
def row(kind, setname):
    m = re.search(rf"load: +{re.escape(setname)} {kind} +direct ([\d.]+) ns +softmmu ([\d.]+) ns", probe)
    return float(m.group(1)), float(m.group(2))
sets = ["64 KiB", "4 MiB", "8 MiB"]
delta = {}
for s in sets:
    ld = row("mix4", s); ind = row("indep", s); cp = row("copy", s)
    delta[s] = (ld[1] - ld[0], (cp[1] - cp[0]) - (ind[1] - ind[0]))
print("per access, ns (softmmu - direct):", ", ".join(f"{s}: load {d[0]:.2f} store {d[1]:.2f}" for s, d in delta.items()))
print(f"{'workload':10} {'Ginsn/s':>8} {'Macc/s':>7} " + " ".join(f"{s:>14}" for s in sets))
for spec in sys.argv[2:]:
    name, gi, ld, st, wall = spec.split(':'); gi, ld, st, wall = map(float, (gi, ld, st, wall))
    ldr, str_ = ld * 1e6 / wall, st * 1e6 / wall
    cols = []
    for s in sets:
        frac = (ldr * delta[s][0] + str_ * delta[s][1]) * 1e-9
        cols.append(f"{100*frac:4.0f}% -> {1/(1-frac):4.2f}x")
    print(f"{name:10} {gi/wall:8.2f} {(ldr+str_)/1e6:7.0f} " + " ".join(f"{c:>14}" for c in cols))
