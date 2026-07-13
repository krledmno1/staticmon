#!/usr/bin/env python3
"""Generate a small random MonPoly trace for behavioral testing.

Usage: gen_trace.py "<sig>" <n_timepoints> <seed>
Emits one `;`-terminated timepoint per line (the format staticmon's file
driver requires; monpoly accepts it too). Non-decreasing timestamps, a small
value domain so joins/temporal operators actually fire. Capped at 100
timepoints (VeriMon is slow).
"""
import random
import re
import sys

sig = sys.argv[1]
ntp = min(int(sys.argv[2]), 100)
rng = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 0)

# parse "name(t1,t2) name2(...) ..."
preds = []
for m in re.finditer(r"(\w+)\(([^)]*)\)", sig):
    name = m.group(1)
    types = [t.strip().split(":")[-1] for t in m.group(2).split(",") if t.strip()]
    preds.append((name, types))

INTS = [1, 2, 3, 4]
STRS = ["aa", "bb", "cc"]


def val(t):
    if t == "int":
        return str(rng.choice(INTS))
    if t == "float":
        return str(rng.choice(INTS)) + ".0"
    return '"' + rng.choice(STRS) + '"'


ts = 0
for _ in range(ntp):
    ts += rng.choice([0, 1, 1, 2, 3])
    events = []
    for name, types in preds:
        # each predicate fires 0..3 tuples this timepoint
        for _ in range(rng.choice([0, 0, 1, 1, 2, 3])):
            args = ",".join(val(t) for t in types)
            events.append(f"{name}({args})")
    rng.shuffle(events)
    sys.stdout.write(f"@{ts} " + " ".join(events) + ";\n")
