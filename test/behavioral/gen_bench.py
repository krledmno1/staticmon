#!/usr/bin/env python3
"""Corpus for the behavioral end-to-end oracle (staticmon vs VeriMon).

Prints the signature on stderr (as `SIG <sig>`) and one small, monitorable
formula per line on stdout. Formulas are kept small so per-formula monitor
compilation stays fast, and deliberately include the deviation-D1 constructs
(integer division `/`, `mod`, `f2i`/`i2f`) so the codegen bug-fixes are
validated behaviorally. Output variables are int/string only (no AVG/MED or
float-typed outputs) so verdict text needs no float-format normalization.
"""
import random
import sys

SIG = "p(int) q(int) r(int,int) s(int,int) a(string) b(int,string)"
sys.stderr.write("SIG " + SIG + "\n")


def emit(f):
    sys.stdout.write(f + "\n")


CURATED = [
    # basic
    "p(x)", "r(x,y)", "p(5)", "r(x,5)",
    "p(x) AND q(x)", "p(x) AND r(x,y)", "r(x,y) AND s(y,z)",
    "p(x) OR q(x)",
    "p(x) AND NOT q(x)", "r(x,y) AND NOT s(x,y)",
    "EXISTS x. r(x,y)", "EXISTS y. (r(x,y) AND q(y))",
    # constraints
    "p(x) AND x = 5", "r(x,y) AND x = y", "p(x) AND x < 5",
    "p(x) AND NOT x = 5", "r(x,y) AND x <= y",
    # temporal
    "ONCE[0,5] p(x)", "ONCE p(x)", "EVENTUALLY[0,3] p(x)",
    "PREV[1,2] p(x)", "NEXT[0,2] p(x)",
    "p(x) SINCE q(x)", "p(x) SINCE[0,5] q(x)",
    "q(x) SINCE (NOT p(x))", "r(x,y) UNTIL[0,3] s(x,y)",
    "ONCE[0,5] (p(x) AND q(x))",
    "(p(x) AND q(x)) SINCE[0,4] r(x,y)",
    # desugared
    "p(x) IMPLIES q(x)", "ALWAYS[0,3] p(x)", "PAST_ALWAYS[0,3] p(x)",
    "EVENTUALLY[0,2] (p(x) OR q(x))",
    # NOTE: aggregations (CNT/SUM/MIN/MAX/AVG/MED) are NOT behaviorally
    # tested: staticmon's MONITOR mishandles an empty relation at a timepoint
    # (aggregations.h:380 dereferences a nullopt -> asserts in Debug, wrong
    # results under NDEBUG). This is a pre-existing monitor bug independent of
    # the pipeline; the aggregation CODEGEN is validated structurally via the
    # header-diff harness. See STATUS.md.
    # string variables (no string CONSTANTS: staticmon's monitor mishandles
    # string constant literals - another pre-existing monitor bug; codegen for
    # them is validated structurally instead)
    "EXISTS u. b(x,u)", "b(x,u) AND p(x)",
    # --- deviation D1: div / mod / conversions inside constraints ---
    "r(x,y) AND z = x + y", "r(x,y) AND z = x - y",
    "r(x,y) AND z = x * y",
    "r(x,y) AND z = x / y",          # tdiv (explicitmon bug: tplus)
    "r(x,y) AND z = x mod y",        # tmod
    "r(x,y) AND y = x / 2",
    "r(x,y) AND x = f2i(i2f(y))",    # f2i/i2f (explicitmon drops them)
    "p(x) AND y = x * 2 - 1",
    "r(x,y) AND x < y / 2",
]
for f in CURATED:
    emit(f)

# small random monitorable formulas
rng = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 11)
PREDS = ["p(x)", "q(x)", "r(x,y)", "s(x,y)", "r(y,z)"]


def small(depth):
    if depth <= 0 or rng.random() < 0.5:
        return rng.choice(PREDS)
    r = rng.random()
    if r < 0.25:
        return f"({small(depth-1)} AND {rng.choice(PREDS)})"
    if r < 0.4:
        c = rng.choice(["x = 5", "x < 3", "x = y", "z = x + 1", "z = x / 2"])
        return f"({rng.choice(['r(x,y)','p(x)'])} AND {c})"
    if r < 0.5:
        return f"({rng.choice(['p(x)','r(x,y)'])} AND NOT {rng.choice(['q(x)','s(x,y)'])})"
    if r < 0.65:
        op = rng.choice(["ONCE", "EVENTUALLY", "PREV", "NEXT"])
        iv = rng.choice(["[0,3]", "[0,5]", "[1,2]"])
        return f"{op}{iv} {small(depth-1)}"
    if r < 0.8:
        op = rng.choice(["SINCE", "UNTIL"])
        iv = rng.choice(["", "[0,3]", "[0,5]"])
        return f"({rng.choice(PREDS)} {op}{iv} {small(depth-1)})"
    return f"(EXISTS x. {small(depth-1)})"


N = int(sys.argv[1]) if len(sys.argv) > 1 else 40
for _ in range(N):
    emit(small(rng.randint(1, 3)))
