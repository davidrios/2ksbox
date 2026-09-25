#!/usr/bin/env python3
"""Read a guest run of Wine's Direct3D tests (track M16) and compare it.

    tools/winetest-summary.py <dir>                      a table per test file
    tools/winetest-summary.py <dir> --save <baseline>    write the run as a baseline
    tools/winetest-summary.py <dir> --baseline <file>    exit 1 on anything worse

<dir> holds what WTRUN.EXE wrote (guest-tools/src/winetest/wtrun.c):
<exe>_<test>.txt per test file and WTRUN.LOG. A baseline
(reference/winetest/*.txt) is one line per failing check, keyed by source
line, which is stable for one pinned Wine tag:

    d3d9_test_visual visual.c:1234 2      a check that failed twice
    d3d9_test_device crash                the test file died
    d3d9_test_visual timeout              WTRUN killed it

"Worse" is a key the baseline lacks, or a key failing more often than
there. The rig's baseline is the oracle: nothing may fail on the driver
that passes on real hardware.
"""
import argparse
import collections
import os
import re
import sys

SUMMARY = re.compile(r"^[0-9a-f]{4}:(\w+):.*?(\d+) tests executed \((\d+) marked as todo, "
                     r"(\d+) as flaky, (\d+) failures?\), (\d+) skipped")
FAIL = re.compile(r"^(\w+\.c):(\d+): .*?(Test failed|Test succeeded inside todo block|"
                  r"Test marked flaky|Test succeeded inside flaky todo block)")
CRASH = re.compile(r"unhandled exception ([0-9a-f]{8})")
WTRUN = re.compile(r"^wtrun: (\w+) (\w+): (exit \d+|timeout|cannot start)")


def read_run(d):
    files = {}      # stem -> dict(executed, failures, skipped, state)
    keys = collections.Counter()
    for name in sorted(os.listdir(d)):
        if not name.lower().endswith(".txt") or "_test_" not in name.lower():
            continue
        stem = name[:-4].lower()
        info = {"executed": 0, "failures": 0, "skipped": 0, "state": "no summary"}
        seen = 0    # failure lines, the count when a crash left no summary line
        with open(os.path.join(d, name), errors="replace") as f:
            for line in f:
                line = line.rstrip("\r\n")
                m = SUMMARY.match(line)
                if m:
                    info.update(executed=int(m[2]), failures=int(m[5]), skipped=int(m[6]),
                                state="ran")
                    continue
                m = FAIL.match(line)
                if m:
                    keys[(stem, f"{m[1]}:{m[2]}")] += 1
                    seen += 1
                    continue
                m = CRASH.search(line)
                if m:
                    info["state"] = f"crash {m[1]}"
        if info["state"] != "ran":
            keys[(stem, "crash")] += 1
            info["failures"] = seen
        files[stem] = info
    log = os.path.join(d, "WTRUN.LOG")
    if os.path.exists(log):
        with open(log, errors="replace") as f:
            for line in f:
                m = WTRUN.match(line.strip())
                if m and m[3] == "timeout":
                    stem = f"{m[1]}_{m[2]}".lower()
                    files.setdefault(stem, {"executed": 0, "failures": 0, "skipped": 0})
                    files[stem]["state"] = "timeout"
                    keys.pop((stem, "crash"), None)
                    keys[(stem, "timeout")] += 1
    return files, keys


def read_baseline(path):
    keys = collections.Counter()
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].split()
            if not line:
                continue
            keys[(line[0], line[1])] = int(line[2]) if len(line) > 2 else 1
    return keys


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir")
    ap.add_argument("--save", metavar="BASELINE")
    ap.add_argument("--baseline", metavar="FILE")
    a = ap.parse_args()

    files, keys = read_run(a.dir)
    if not files:
        print(f"no test output in {a.dir}")
        return 1
    print(f"{'test file':28} {'executed':>9} {'failures':>9} {'skipped':>8}  state")
    for stem, i in sorted(files.items()):
        print(f"{stem:28} {i['executed']:>9} {i['failures']:>9} {i['skipped']:>8}  {i['state']}")

    if a.save:
        with open(a.save, "w") as f:
            f.write(f"# tools/winetest-summary.py {os.path.basename(os.path.normpath(a.dir))} "
                    "--save; one failing check per line\n")
            for (stem, key), n in sorted(keys.items()):
                f.write(f"{stem} {key} {n}\n")
        print(f"saved {len(keys)} failing keys to {a.save}")

    if a.baseline:
        base = read_baseline(a.baseline)
        worse = [(s, k, n, base.get((s, k), 0)) for (s, k), n in sorted(keys.items())
                 if n > base.get((s, k), 0)]
        better = sum(1 for sk in base if sk not in keys)
        for s, k, n, b in worse:
            print(f"WORSE {s} {k}: {n} here, {b} in the baseline")
        print(f"{len(worse)} keys worse than {os.path.basename(a.baseline)}, "
              f"{better} of its keys pass here")
        return 1 if worse else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
