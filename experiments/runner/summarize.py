#!/usr/bin/env python3
"""Summarize a benchmark out.csv into a per-benchmark monitor comparison table.

Reads the `benchmark,monitor,repetition,time` CSV the harness writes and prints,
for each benchmark, the median run time of each monitor. A blank cell means the
monitor's fragment does not cover that formula (it was not run) -- so the table
also shows, at a glance, the fragment membership per formula.

Usage: summarize.py [out.csv]   (default: out.csv)
"""
import csv
import statistics
import sys

MONITORS = ["staticmon", "monpoly", "timelymon", "whymon", "dejavu"]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "out.csv"
    times = {}  # (benchmark, monitor) -> [times]
    order = []  # benchmark order of first appearance
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            b, m = row["benchmark"], row["monitor"]
            if b not in order:
                order.append(b)
            times.setdefault((b, m), []).append(float(row["time"]))

    present = [m for m in MONITORS if any((b, m) in times for b in order)]
    present += sorted({m for (_, m) in times} - set(MONITORS))  # any extras

    wb = max([len("benchmark")] + [len(b) for b in order])
    header = "benchmark".ljust(wb) + "  " + "  ".join(c.rjust(10) for c in present)
    print(header)
    print("-" * len(header))
    for b in order:
        cells = []
        for m in present:
            vs = times.get((b, m))
            cells.append(f"{statistics.median(vs):10.4f}" if vs else " " * 10)
        print(b.ljust(wb) + "  " + "  ".join(cells))

    print("\n(median seconds over repetitions; blank = formula outside that "
          "monitor's fragment)")


if __name__ == "__main__":
    main()
