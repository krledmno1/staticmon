#!/usr/bin/env python3
"""Profiling mode for the benchmark harness (docs/optimization-plan.md, I2).

Runs each (benchmark, monitor) pair once, untimed, with a sampling profiler
attached, and reports where the time actually goes -- so optimizations are
prioritized on evidence rather than on scaling ratios alone.

Operates on a directory produced by `test-runner ... bench -o <dir>`, which
leaves everything needed behind: <dir>/<benchmark>/{sig, fo, log} and, for
staticmon, the compiled staticmon.bin. Deliberately a separate tool rather than
a flag inside the Haskell runner: attaching a profiler needs the child's pid,
which the runner's process abstraction hides, and nothing here may perturb the
timing path.

Writes <dir>/<benchmark>/profile-<monitor>.txt (the raw profiler report) and
<dir>/profile_summary.csv (samples, wall time, peak RSS and the self-time share
of each cost category).

Usage:
  profile.py <bench_out_dir> [-m staticmon,monpoly,verimon] [-d SECONDS]
                             [-b BENCH_SUBSTRING] [--top N]
"""
import argparse
import csv
import os
import re
import subprocess
import sys
import time

MONPOLY = os.environ.get(
    "MONPOLY",
    os.path.expanduser("~/.opam/4.14.2/bin/monpoly"),
)

# Monitor command lines. These mirror Monitors.hs (monpolyBaseOpts and the
# staticmon/monpoly/verimon records); keep them in sync if the harness changes
# how it invokes a monitor.
def monitor_cmd(monitor, d):
    sig, fo, log = (os.path.join(d, x) for x in ("sig", "fo", "log"))
    if monitor == "staticmon":
        return [os.path.join(d, "staticmon.bin"), "--log", log]
    base = [MONPOLY, "-formula", fo, "-sig", sig, "-no_rw", "-nofilteremptytp",
            "-log", log]
    if monitor == "verimon":
        return [MONPOLY, "-verified"] + base[1:]
    return base


def strip_types(sym):
    """Drop template and call arguments, so a symbol classifies on its own name
    rather than on the types it mentions: staticmon's operators carry the whole
    formula in their template arguments, so e.g. `bin_rel_op<...>::eval(absl::
    flat_hash_map<...>&)` must count as evaluation, not as hashing."""
    out, depth = [], 0
    for ch in sym:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(ch)
    s = "".join(out)
    out, depth = [], 0
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(ch)
    return "".join(out)


# Cost categories, first match wins. Matched against the stripped symbol.
CATEGORIES = [
    ("frz",     ["mfrz"]),                       # replay machinery itself
    ("copy",    ["uninitialized_allocator_copy", "__destroy_vector", "memmove",
                 "memcpy", "copy_impl", "__copy", "basic_string"]),
    ("join",    ["make_join_map", "table_join_dispatch", "join"]),
    ("hash",    ["raw_hash_set", "flat_hash", "absl", "Hash"]),
    ("eval",    ["bin_rel_op", "msince", "monce", "muntil", "mand", "mor",
                 "mpredicate", "mexists", "since_impl", "once_impl",
                 "monitor::step", "do_monitor", "aggregation"]),
    ("parse",   ["trace_parser", "lexy"]),
    ("alloc",   ["operator new", "free", "malloc", "memset", "je_",
                 "pthread_getspecific", "DYLD-STUB"]),
]


def categorize(sym):
    s = strip_types(sym)
    for name, pats in CATEGORIES:
        if any(p in s for p in pats):
            return name
    return "other"


# `sample`'s self-time section: "<symbol>  (in <image>)        <count>"
LEAF_RE = re.compile(r"^\s{4,}(.*?)\s+\(in ([^)]*)\)\s+(\d+)\s*$")
# A call-graph line: tree drawing characters, then "<count> <symbol>  (in ...)".
GRAPH_RE = re.compile(r"^([ +!:|]*)(\d+) (.*?)\s+\(in ([^)]*)\)")


def parse_leaves(path):
    """Self ("top of stack") samples per symbol from a `sample` report."""
    leaves, on = [], False
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("Sort by top of stack"):
                on = True
                continue
            if on:
                if line.startswith("Binary Images") or line.startswith("Total number"):
                    break
                m = LEAF_RE.match(line.rstrip("\n"))
                if m:
                    leaves.append((m.group(1), m.group(2), int(m.group(3))))
    return leaves


def parse_callgraph(path):
    """(indent, count, symbol) per call-graph node, in document order."""
    nodes, on = [], False
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("Call graph:"):
                on = True
                continue
            if on:
                if line.startswith("Total number") or line.startswith("Sort by top"):
                    break
                m = GRAPH_RE.match(line.rstrip("\n"))
                if m:
                    nodes.append((len(m.group(1)), int(m.group(2)), m.group(3)))
    return nodes


def inclusive_share(nodes, cat):
    """Inclusive samples spent in (and below) frames of category `cat`, as a
    fraction of the profiled thread's samples.

    Self time alone under-reports a category that delegates: a database copy's
    own loop is small, but the operator new / free it triggers are charged to
    `alloc`. Summing only the TOP-MOST matching node of each branch counts each
    sample once."""
    if not nodes:
        return 0.0
    root = max(c for _, c, _ in nodes)
    total, skip_above = 0, None
    for indent, count, sym in nodes:
        if skip_above is not None:
            if indent > skip_above:
                continue  # inside an already-counted subtree
            skip_above = None
        if categorize(sym) == cat:
            total += count
            skip_above = indent
    return 100.0 * total / root if root else 0.0


def profile_one(monitor, d, duration):
    cmd = monitor_cmd(monitor, d)
    if not os.path.exists(cmd[0]):
        return None
    out = os.path.join(d, f"profile-{monitor}.txt")
    devnull = subprocess.DEVNULL
    # Warm up: on macOS the FIRST exec of a new executable inode pays ~0.2s of
    # code-signature validation, which would otherwise land in the profile.
    subprocess.run(cmd, stdout=devnull, stderr=devnull)
    t0 = time.perf_counter()
    p = subprocess.Popen(cmd, stdout=devnull, stderr=devnull)
    subprocess.run(["sample", str(p.pid), str(duration), "-file", out],
                   stdout=devnull, stderr=devnull)
    _, _, ru = os.wait4(p.pid, 0)
    wall = time.perf_counter() - t0
    leaves = parse_leaves(out) if os.path.exists(out) else []
    nodes = parse_callgraph(out) if os.path.exists(out) else []
    total = sum(c for _, _, c in leaves)
    shares = {}
    for sym, _img, c in leaves:
        shares[categorize(sym)] = shares.get(categorize(sym), 0) + c
    # Inclusive copy share answers opt A directly: how much time is spent in
    # (and under) database-copy frames, alloc included.
    incl_copy = inclusive_share(nodes, "copy")
    return {
        "wall": wall,
        "maxrss_mb": ru.ru_maxrss / (1024 * 1024),
        "samples": total,
        # `sample` stops when the process dies; a longer run is profiled only
        # up to `duration` (a steady-state prefix).
        "partial": wall > duration + 0.5,
        "shares": shares,
        "incl_copy": incl_copy,
        "leaves": leaves,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("-m", "--monitors", default="staticmon,monpoly,verimon")
    ap.add_argument("-d", "--duration", type=int, default=10,
                    help="max seconds to sample (default 10)")
    ap.add_argument("-b", "--bench", default="",
                    help="only benchmarks whose name contains this")
    ap.add_argument("--top", type=int, default=8,
                    help="top self-time frames to print per profile")
    a = ap.parse_args()

    monitors = a.monitors.split(",")
    benches = sorted(x for x in os.listdir(a.dir)
                     if os.path.isdir(os.path.join(a.dir, x)) and a.bench in x)
    cats = [c for c, _ in CATEGORIES] + ["other"]
    rows = []
    for b in benches:
        d = os.path.join(a.dir, b)
        if not os.path.exists(os.path.join(d, "fo")):
            continue
        for mon in monitors:
            r = profile_one(mon, d, a.duration)
            if r is None:
                continue
            row = {"benchmark": b, "monitor": mon,
                   "wall": round(r["wall"], 3),
                   "maxrss_mb": round(r["maxrss_mb"], 1),
                   "samples": r["samples"],
                   "partial": r["partial"]}
            for c in cats:
                pct = (100.0 * r["shares"].get(c, 0) / r["samples"]
                       if r["samples"] else 0.0)
                row[c] = round(pct, 1)
            row["incl_copy"] = round(r["incl_copy"], 1)
            rows.append(row)
            print(f"\n=== {b} / {mon} "
                  f"({r['samples']} samples, {r['wall']:.2f}s, "
                  f"{r['maxrss_mb']:.0f}MB{', partial' if r['partial'] else ''}) ===")
            print("  " + "  ".join(f"{c}={row[c]}%" for c in cats if row[c] > 0)
                  + f"  |  inclusive copy={row['incl_copy']}%")
            for sym, _img, c in sorted(r["leaves"], key=lambda x: -x[2])[:a.top]:
                pct = 100.0 * c / r["samples"] if r["samples"] else 0
                print(f"    {pct:5.1f}%  {categorize(sym):6}  {strip_types(sym)[:90]}")

    if rows:
        outcsv = os.path.join(a.dir, "profile_summary.csv")
        with open(outcsv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
        print(f"\nwrote {outcsv}")


if __name__ == "__main__":
    main()
