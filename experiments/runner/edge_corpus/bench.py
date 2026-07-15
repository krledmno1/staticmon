#!/usr/bin/env python3
"""Corpus benchmark: staticmon vs monpoly on the edge_corpus formulas.

For each formula it compiles staticmon once (cached via scripts/staticmon-impl),
then picks 5 trace sizes adaptively -- it probes monpoly (the slower of the two)
and, by linear extrapolation, centres a geometric size ladder near a ~1.5s run so
that roughly the 3 smaller sizes finish under the timeout for both tools and the
2 larger ones stress them. Logs are generic random traces over the shared
signature. Output is the usual out.csv schema (benchmark,monitor,repetition,
time,status) so summarize.py works on it.

Usage: bench.py [-o OUTDIR] [-t TIMEOUT] [-r REPS]
"""
import argparse
import csv
import os
import random
import re
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
SIG = os.path.join(HERE, "sig")
FORMULAS = os.path.join(HERE, "formulas")
IMPL = os.path.join(REPO, "scripts", "staticmon-impl")

DOM = 10000    # value domain: moderate, so joins (P/Q/S) collide (real work) yet
               # temporal/aggregation state still grows with the trace
EPP = 50       # events per predicate per time-point
TARGET_S = 2.0   # anchor monpoly ~2s at the 4th size, so the 5th (2x) tends to
                 # cross the 3s cap -- ~4 sizes usable, the largest stresses them.
# 5 multiplicative steps spanning ~16x; the top 1-2 are allowed to time out.
LADDER = [0.125, 0.25, 0.5, 1.0, 2.0]
MIN_NTP, MAX_NTP = 50, 25000


def load_formulas(path=FORMULAS):
    out = []
    for ln in open(path):
        s = ln.strip()
        if s and not s.startswith("#"):
            out.append(s)
    return out


def parse_sig():
    preds = []
    for m in re.finditer(r"([A-Za-z_]\w*)\(([^)]*)\)", open(SIG).read()):
        arity = 0 if not m.group(2).strip() else len(m.group(2).split(","))
        preds.append((m.group(1), arity))
    return preds


PREDS = parse_sig()


def gen_log(path, ntp, seed):
    rnd = random.Random(seed)
    ri = rnd.randrange
    D = DOM
    with open(path, "w") as f:
        out = []
        for t in range(ntp):
            parts = ["@", str(t)]
            for name, ar in PREDS:
                if ar == 1:
                    for _ in range(EPP):
                        parts.append(" %s(%d)" % (name, ri(D)))
                elif ar == 2:
                    for _ in range(EPP):
                        parts.append(" %s(%d,%d)" % (name, ri(D), ri(D)))
                elif ar == 0:
                    for _ in range(EPP):
                        parts.append(" %s()" % name)
                else:
                    for _ in range(EPP):
                        parts.append(" %s(%s)" % (name, ",".join(str(ri(D)) for _ in range(ar))))
            parts.append(";\n")
            out.append("".join(parts))
            if len(out) >= 2000:
                f.write("".join(out))
                out = []
        if out:
            f.write("".join(out))
    return ntp * EPP * len(PREDS)  # event count


def run_timed(cmd, timeout):
    """(seconds, status) where status in ok/timeout/error; seconds=timeout on non-ok."""
    t0 = time.monotonic()
    try:
        p = subprocess.run(cmd, timeout=timeout, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return timeout, "timeout"
    dt = time.monotonic() - t0
    return (dt, "ok") if p.returncode == 0 else (timeout, "error")


def compile_staticmon(formula_file, workdir):
    outbin = os.path.join(workdir, "staticmon.bin")
    r = subprocess.run([IMPL, "compile", "-sig", SIG, "-formula", formula_file,
                        "-keep", outbin, "-quiet"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode != 0:
        return None
    return outbin


def monpoly_cmd(ff, log):
    return ["monpoly", "-formula", ff, "-sig", SIG, "-no_rw", "-nofilteremptytp", "-log", log]


def calibrate(ff, workdir, timeout, binary):
    """Find a target #time-points where the slower supported tool ~ TARGET_S.
    Calibrates on monpoly, but if monpoly rejects the formula (e.g. unverified
    monpoly has no LETPAST) it calibrates on staticmon and flags monpoly
    unsupported. Returns (target_ntp, monpoly_ok)."""
    log = os.path.join(workdir, "probe.log")
    monpoly_ok = True

    def probe(n):
        gen_log(log, n, 1)
        cmd = monpoly_cmd(ff, log) if monpoly_ok else [binary, "--log", log]
        return run_timed(cmd, timeout)

    ntp = 2000
    t, st = probe(ntp)
    if st == "error":              # tool rejects the formula -> switch to staticmon
        monpoly_ok = False
        t, st = probe(ntp)
    while st == "ok" and t < 0.15 and ntp < 40000:   # too cheap -> probe larger
        ntp *= 3
        t, st = probe(ntp)
    while st == "timeout" and ntp > 100:             # times out -> probe smaller
        ntp //= 4
        t, st = probe(ntp)
    slope = max(t, 0.05) / ntp
    target = max(MIN_NTP, min(MAX_NTP // 2, int(TARGET_S / slope)))
    return target, monpoly_ok


def slug(i, f):
    s = re.sub(r"[^A-Za-z0-9]+", "_", f).strip("_")[:28]
    return "f%02d_%s" % (i, s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--outdir", default="edge_out")
    ap.add_argument("-t", "--timeout", type=float, default=3.0)
    ap.add_argument("-r", "--reps", type=int, default=2)
    ap.add_argument("--formulas", default=FORMULAS, help="override the formula list")
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    workdir = os.path.join(a.outdir, "work")
    os.makedirs(workdir, exist_ok=True)

    formulas = load_formulas(a.formulas)
    rows = []
    for i, f in enumerate(formulas):
        name = slug(i, f)
        ff = os.path.join(workdir, "f.mfotl")
        open(ff, "w").write(f)
        binary = compile_staticmon(ff, workdir)
        if binary is None:
            print("COMPILE FAIL: %s" % f)
            continue
        target, monpoly_ok = calibrate(ff, workdir, a.timeout, binary)
        sizes = sorted({max(MIN_NTP, min(MAX_NTP, int(target * m))) for m in LADDER})
        note = "" if monpoly_ok else "  [monpoly UNSUPPORTED -- needs -verified]"
        print("%-32s target=%d tps, sizes(tps)=%s%s" % (name, target, sizes, note), flush=True)
        disq = set()
        for ntp in sizes:
            log = os.path.join(workdir, "run.log")
            events = gen_log(log, ntp, 7)
            bench = "%s__e%d" % (name, events)
            cmds = {"staticmon": [binary, "--log", log]}
            if monpoly_ok:
                cmds["monpoly"] = monpoly_cmd(ff, log)
            for mon, cmd in cmds.items():
                if mon in disq:
                    rows.append((bench, mon, 0, a.timeout, "disqualified"))
                    continue
                for rep in range(a.reps):
                    t, st = run_timed(cmd, a.timeout)
                    rows.append((bench, mon, rep, round(t, 5), st))
                    if st != "ok":
                        disq.add(mon)
                        break

    outcsv = os.path.join(a.outdir, "out.csv")
    with open(outcsv, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["benchmark", "monitor", "repetition", "time", "status"])
        w.writerows(rows)
    print("\nwrote %s (%d rows)" % (outcsv, len(rows)))


if __name__ == "__main__":
    main()
