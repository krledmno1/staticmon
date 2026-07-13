#!/usr/bin/env python3
"""Generate MFOTL formulas (+ a fixed signature) for differentially testing
staticmon_compile's typing and monitorability against MonPoly.

Emits, on stdout, the signature (first line, prefixed `SIG `) then one formula
per line, built to be mostly monitorable by construction (the harness also
feeds non-monitorable and some ill-typed ones). Used by
test/pipeline_diff/run_check_diff.sh, which compares
`staticmon_compile -sigout`/`-check` against `monpoly -sigout`/`-check`.
"""

import random
import sys

SIG = "p(int) q(int) r(int,int) s(int,int) a(string) b(int,string)"

# monitorable atomic predicates (all-variable / mixed args)
PREDS = [
    "p(x)", "q(x)", "q(y)", "r(x,y)", "r(y,z)", "s(x,y)", "s(y,z)",
    "a(u)", "b(x,u)", "p(5)", "r(x,5)",
]


def emit(f):
    sys.stdout.write(f + "\n")


# Curated coverage: each operator in a monitorable context.
CURATED = [
    "p(x)", "r(x,y)", "p(5)", "r(x,5)",
    "ONCE[0,10] p(x)", "ONCE p(x)", "EVENTUALLY[0,5] p(x)",
    "PREV[1,2] p(x)", "NEXT p(x)", "ONCE[2,*) r(x,y)",
    "p(x) AND q(x)", "p(x) AND r(x,y)", "r(x,y) AND s(y,z)",
    "p(x) OR q(x)", "(p(x) AND q(x)) OR (r(x,y) AND s(x,y))",
    "p(x) AND NOT q(x)", "r(x,y) AND NOT s(x,y)",
    "p(x) AND x = 5", "r(x,y) AND x = y", "p(x) AND x < 5",
    "p(x) AND NOT x = 5", "r(x,y) AND x <= y",
    "EXISTS x. p(x)", "EXISTS x. r(x,y)", "EXISTS x, y. s(x,y)",
    "EXISTS y. (r(x,y) AND q(y))",
    "EXISTS y. (p(x) AND y = x)",
    "p(x) SINCE q(x)", "p(x) SINCE[0,10] q(x)",
    "q(x) SINCE (NOT p(x))", "r(x,y) UNTIL[0,5] s(x,y)",
    "p(x) UNTIL q(x)", "(NOT p(x)) SINCE q(x)",
    "c <- CNT x p(x)", "c <- CNT x; y r(x,y)",
    "m <- SUM x; y r(x,y)", "m <- MIN x r(y,x)", "m <- MAX x p(x)",
    "m <- AVG x r(y,x)",
    "c <- CNT x ONCE[0,5] p(x)",
    "c <- CNT x (r(x,y) SINCE s(x,y))",
    "p(x) IMPLIES q(x)", "p(x) EQUIV q(x)",
    "FORALL x. (p(x) IMPLIES q(x))",
    "ALWAYS[0,5] p(x)", "PAST_ALWAYS[0,5] p(x)",
    "ONCE[0,10] (EXISTS y. r(x,y)) AND q(x)",
    "p(x) AND x = 5 AND x < 10",
    "EXISTS y, z. (s(x,y) AND s(y,z))",
    'a(u) AND u = "hello"', 'b(x,u) AND u = "v"',
    "r(x,y) AND y = x + 1", "r(x,y) AND y = x * 2",
    "r(x,y) AND y = x - 1", "r(x,y) AND z = x + y AND s(z,x)",
    "NOT (x = y) AND r(x,y)",
]
for f in CURATED:
    emit(f)


# Grammar-directed random monitorable formulas.
rng = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 7)


def rand_pred():
    return rng.choice(PREDS)


def rand_interval():
    if rng.random() < 0.4:
        return ""
    lo = rng.choice(["0", "1", "2", "5"])
    hi = rng.choice(["3", "7", "10", "*"])
    lb, rb = rng.choice(["[]", "()", "[)", "(]"])
    if hi == "*":
        rb = rng.choice(")]")
    return f"{lb}{lo},{hi}{rb}".replace("[", "[").replace("]", "]") \
        .replace("[0", lb + "0")  # keep simple


def simple_interval():
    if rng.random() < 0.4:
        return ""
    lo = rng.choice(["0", "1", "2"])
    hi = rng.choice(["5", "10", "*"])
    lb = rng.choice("[(")
    rb = ")" if hi == "*" else rng.choice("])")
    return f"{lb}{lo},{hi}{rb}"


def rand_monitorable(depth):
    """Produce a (formula, free_var_set) pair that is monitorable."""
    if depth <= 0 or rng.random() < 0.4:
        return rand_pred()
    r = rng.random()
    if r < 0.25:
        return f"({rand_monitorable(depth-1)} AND {rand_pred()})"
    if r < 0.35:
        # AND with a constraint over a guard's variable
        return f"({rand_pred()} AND x = {rng.choice(['5','y','x + 1'])})"
    if r < 0.45:
        return f"({rand_pred()} AND NOT {rand_pred()})"
    if r < 0.55:
        # OR requires equal free vars; use same pred shape
        p = rng.choice(["p(x)", "q(x)", "r(x,y)"])
        p2 = rng.choice(["p(x)", "q(x)", "r(x,y)"])
        return f"({p} OR {p2})"
    if r < 0.68:
        op = rng.choice(["ONCE", "EVENTUALLY", "PREV", "NEXT"])
        return f"{op}{simple_interval()} {rand_monitorable(depth-1)}"
    if r < 0.80:
        op = rng.choice(["SINCE", "UNTIL"])
        return f"({rand_pred()} {op}{simple_interval()} {rand_monitorable(depth-1)})"
    if r < 0.90:
        v = rng.choice(["x", "y"])
        return f"(EXISTS {v}. {rand_monitorable(depth-1)})"
    op = rng.choice(["CNT", "SUM", "MIN", "MAX", "AVG"])
    return f"res <- {op} x r(x,y)"


N = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
for _ in range(N):
    emit(rand_monitorable(rng.randint(1, 4)))

# Signature marker printed last so the harness can grab it deterministically.
sys.stderr.write(SIG + "\n")
