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


def cell(rows):
    """Render one monitor's result for one benchmark from its (time, status) rows.
    Median of successful times; `>T` if it only ever timed out / was disqualified;
    `err` if it crashed; blank handled by the caller (monitor not run at all)."""
    oks = [t for (t, s) in rows if s == "ok"]
    if oks:
        return f"{statistics.median(oks):10.4f}"
    slow = [t for (t, s) in rows if s in ("timeout", "disqualified")]
    if slow:
        return (">" + str(int(max(slow)))).rjust(10)
    return "err".rjust(10)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "out.csv"
    rows = {}  # (benchmark, monitor) -> [(time, status)]
    order = []  # benchmark order of first appearance
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            b, m = r["benchmark"], r["monitor"]
            if b not in order:
                order.append(b)
            rows.setdefault((b, m), []).append(
                (float(r["time"]), r.get("status", "ok")))

    present = [m for m in MONITORS if any((b, m) in rows for b in order)]
    present += sorted({m for (_, m) in rows} - set(MONITORS))  # any extras

    wb = max([len("benchmark")] + [len(b) for b in order])
    header = "benchmark".ljust(wb) + "  " + "  ".join(c.rjust(10) for c in present)
    print(header)
    print("-" * len(header))
    for b in order:
        cells = [cell(rows[(b, m)]) if (b, m) in rows else " " * 10 for m in present]
        print(b.ljust(wb) + "  " + "  ".join(cells))

    print("\n(median seconds over repetitions; >N = timed out / disqualified at "
          "N s; blank = formula outside that monitor's fragment)")


if __name__ == "__main__":
    main()
