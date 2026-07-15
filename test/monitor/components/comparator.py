#!/usr/bin/env python3
"""Compare two MonPoly-format verdict streams for semantic equality.

Verdict lines look like:  @<ts> (time point <tp>): (t) (u) ...
Row order within a time point is irrelevant (both tools sort, but we
normalise anyway); float fields are compared numerically to absorb
formatting differences (staticmon uses %.5e, monpoly its own format).

Usage: comparator.py file_a file_b   -> exit 0 if equal, else prints diff.
"""
import re
import sys


def norm_num(tok):
    try:
        f = float(tok)
        # canonical numeric form; ints stay ints
        if f == int(f) and "e" not in tok.lower() and "." not in tok:
            return str(int(f))
        return f"{f:.6g}"
    except ValueError:
        return tok


def norm_tuple(tup):
    inner = tup[1:-1]  # strip parens
    parts = [norm_num(p) for p in inner.split(",")]
    return "(" + ",".join(parts) + ")"


def parse(path):
    out = {}
    for line in open(path):
        line = line.strip()
        m = re.match(r"@(\d+) \(time point (\d+)\):(.*)", line)
        if not m:
            continue
        tp = int(m.group(2))
        rest = m.group(3).strip()
        if rest == "true":
            out[tp] = ("true",)
        else:
            tuples = re.findall(r"\([^)]*\)", rest)
            out[tp] = tuple(sorted(norm_tuple(t) for t in tuples))
    return out


a = parse(sys.argv[1])
b = parse(sys.argv[2])
if a == b:
    sys.exit(0)

# report up to a few differing time points
keys = sorted(set(a) | set(b))
shown = 0
for k in keys:
    if a.get(k) != b.get(k):
        print(f"  tp {k}: a={a.get(k)} b={b.get(k)}")
        shown += 1
        if shown >= 8:
            break
sys.exit(1)
