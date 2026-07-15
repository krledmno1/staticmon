#!/usr/bin/env python3
"""Randomized differential correctness test: staticmon vs VeriMon (live oracle).

Generates random formulas (the generator component (gen_bench.py)) + traces
(generator/gen_trace.py), keeps the ones BOTH staticmon and VeriMon accept as monitorable,
compiles the staticmon monitor (cached via scripts/staticmon-impl), runs both on
each trace, and flags any verdict mismatch (compare_verdicts.py). Reports the
structural coverage (features.py) the generated corpus achieved.

VeriMon is a user-provided monpoly build run with -verified; the repo has no
monpoly dependency. Exits 77 (ctest "skipped") if --verimon is missing.

Usage: run.py --verimon PATH [--iterations N] [--seed S] [--tp T]
              [--traces-per-formula K] [--timeout SEC] [--out DIR]
"""
import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile

# test/monitor/methods/live/  ->  repo root is four levels up; shared components
# live under test/monitor/components/.
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
COMPONENTS = os.path.join(HERE, "..", "..", "components")
GEN_BENCH = os.path.join(COMPONENTS, "generator", "gen_bench.py")
GEN_TRACE = os.path.join(COMPONENTS, "generator", "gen_trace.py")
COMPARE = os.path.join(COMPONENTS, "comparator.py")
IMPL = os.path.join(REPO, "scripts", "staticmon-impl")
HEADERS = os.path.join(REPO, "builddir", "bin", "staticmon-headers")

sys.path.insert(0, COMPONENTS)
import coverage as features  # noqa: E402  (structural-coverage component)


def run(cmd, timeout=None, **kw):
    return subprocess.run(cmd, timeout=timeout, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, **kw)


def strip_maxts(s):
    """Drop VeriMon's end-of-log MaxTS line(s). We keep the default flush (which
    matches staticmon's offline end-of-log semantics), and only remove the extra
    large-timestamp marker line VeriMon can append, as the old behavioral harness
    did (`sed '$ {/MaxTS/ d}'`). compare_verdicts already ignores it (the ts is
    non-numeric), but strip it explicitly for robustness."""
    return "\n".join(l for l in s.splitlines() if "MaxTS" not in l) + "\n"


def staticmon_accepts(sig, ff):
    r = run([HEADERS, "-sig", sig, "-formula", ff, "-check"])
    return "is monitorable" in r.stdout


def verimon_accepts(verimon, sig, ff):
    r = run([verimon, "-verified", "-check", "-sig", sig, "-formula", ff])
    return "sequence of free variables" in r.stdout and "NOT monitorable" not in r.stdout


def compile_staticmon(sig, ff, outbin):
    r = run([IMPL, "compile", "-sig", sig, "-formula", ff, "-keep", outbin, "-quiet"])
    return r.returncode == 0, r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verimon", help="path to a monpoly build (run with -verified)")
    ap.add_argument("--iterations", type=int, default=40)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--tp", type=int, default=40)
    ap.add_argument("--traces-per-formula", type=int, default=2)
    ap.add_argument("--timeout", type=float, default=15)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()

    verimon = a.verimon or os.environ.get("STATICMON_VERIMON")
    if not verimon or not shutil.which(verimon) and not os.path.isfile(verimon):
        sys.stderr.write("random_diff: no VeriMon binary (--verimon / $STATICMON_VERIMON); skipping\n")
        sys.exit(77)
    if not os.path.exists(HEADERS):
        sys.stderr.write("random_diff: staticmon-headers not built; skipping\n")
        sys.exit(77)

    out = a.out or tempfile.mkdtemp(prefix="random_diff_")
    work = os.path.join(out, "work")
    faildir = os.path.join(out, "failures")
    os.makedirs(work, exist_ok=True)

    # Generate a pool of formulas + the shared signature (SIG on stderr).
    gb = subprocess.run([sys.executable, GEN_BENCH, str(a.iterations), str(a.seed)],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    sig_line = next((l for l in gb.stderr.splitlines() if l.startswith("SIG ")), None)
    sig_str = sig_line[4:].strip()
    pool = [l.strip() for l in gb.stdout.splitlines() if l.strip()]
    rng = random.Random(a.seed)
    rng.shuffle(pool)
    pool = pool[: a.iterations]

    sig = os.path.join(work, "sig")
    open(sig, "w").write(sig_str + "\n")
    ff = os.path.join(work, "f.mfotl")
    binf = os.path.join(work, "staticmon.bin")

    counts = dict(tested=0, rr_skipped=0, skipped=0, timeout=0, compile_err=0, mismatch=0)
    tested_formulas = []

    for formula in pool:
        open(ff, "w").write(formula)
        sm_ok = staticmon_accepts(sig, ff)
        vm_ok = verimon_accepts(verimon, sig, ff)
        if not (sm_ok and vm_ok):
            if vm_ok and not sm_ok:
                counts["rr_skipped"] += 1     # staticmon more conservative (rr gap)
            else:
                counts["skipped"] += 1
            continue

        ok, log = compile_staticmon(sig, ff, binf)
        if not ok:
            counts["compile_err"] += 1
            save_failure(faildir, "compile", formula, sig_str, None, log, None)
            continue

        counts["tested"] += 1
        tested_formulas.append(formula)
        for k in range(a.traces_per_formula):
            trace = os.path.join(work, "trace")
            with open(trace, "w") as th:
                subprocess.run([sys.executable, GEN_TRACE, sig_str, str(a.tp),
                                str(a.seed * 1000 + k)], stdout=th, check=True)
            try:
                sm = run([binf, "--log", trace], timeout=a.timeout)
                vm = run([verimon, "-verified", "-sig", sig, "-formula", ff,
                          "-log", trace], timeout=a.timeout)
            except subprocess.TimeoutExpired:
                counts["timeout"] += 1
                continue
            smf = os.path.join(work, "sm.out"); open(smf, "w").write(sm.stdout)
            vmf = os.path.join(work, "vm.out"); open(vmf, "w").write(strip_maxts(vm.stdout))
            cmp = subprocess.run([sys.executable, COMPARE, smf, vmf],
                                 stdout=subprocess.PIPE, text=True)
            if cmp.returncode != 0:
                counts["mismatch"] += 1
                save_failure(faildir, "mismatch", formula, sig_str, trace,
                             sm.stdout, vm.stdout, cmp.stdout)

    report(counts, tested_formulas, out)
    sys.exit(1 if (counts["mismatch"] or counts["compile_err"]) else 0)


def save_failure(faildir, kind, formula, sig, trace, sm_out, vm_out, diff=""):
    import uuid
    d = os.path.join(faildir, "%s_%s" % (kind, uuid.uuid4().hex[:8]))
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "formula"), "w").write(formula + "\n")
    open(os.path.join(d, "sig"), "w").write(sig + "\n")
    if trace:
        shutil.copy(trace, os.path.join(d, "trace"))
    if sm_out is not None:
        open(os.path.join(d, "staticmon.out"), "w").write(sm_out)
    if vm_out is not None:
        open(os.path.join(d, "verimon.out"), "w").write(vm_out)
    if diff:
        open(os.path.join(d, "diff"), "w").write(diff)
    sys.stderr.write("  [%s] %s -> %s\n" % (kind, formula[:60], d))


def report(counts, tested_formulas, out):
    print()
    print("random_diff: " + "  ".join("%s=%d" % (k, v) for k, v in counts.items()))
    covered, uncovered, adj, _, _ = features.coverage(tested_formulas)
    n = len(features.CHECKLIST)
    print("structural coverage over %d tested formulas: %d/%d features (%.0f%%), "
          "%d nesting adjacencies"
          % (len(tested_formulas), len(covered), n, 100.0 * len(covered) / max(n, 1), len(adj)))
    if uncovered:
        print("uncovered: " + ", ".join(uncovered))
    if counts["mismatch"] or counts["compile_err"]:
        print("FAILURES saved under %s/failures" % out)


if __name__ == "__main__":
    main()
