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
    # aggregations, with and without a group-by variable. AVG/MED produce a
    # float output (compared numerically).
    "c <- CNT x; y r(x,y)", "c <- CNT y; x r(x,y)", "c <- CNT x r(y,x)",
    "m <- SUM x; y r(y,x)", "m <- MIN x; y r(y,x)", "m <- MAX x; y r(y,x)",
    "m <- MIN x r(y,x)", "m <- MAX x r(y,x)", "s <- SUM x r(y,x)",
    "avg <- AVG x; y r(y,x)", "md <- MED x; y r(y,x)", "md <- MED x r(y,x)",
    "c <- CNT x; y (r(x,y) AND p(x))",
    # strings, incl. constants (mterm.h tcst + formula.h sv-literal fixed)
    'a(u) AND u = "aa"', 'b(x,u) AND u = "bb"',
    "EXISTS u. b(x,u)", "b(x,u) AND p(x)",
    # LET / LETPAST
    "LET P(x) = q(x) IN P(x) AND p(x)",
    "LET T(x,y) = r(x,y) IN ONCE[0,3] T(x,y)",
    "LET P(x) = q(x) IN P(x) OR (EXISTS y. r(x,y))",
    "LET P(x) = q(x) IN LET Z(x) = P(x) AND p(x) IN Z(x)",
    "LET A(x) = p(x) IN LET A(x) = q(x) IN A(x) AND A(x)",
    "LETPAST L(x) = p(x) OR (PREV L(x) AND q(x)) IN L(x)",
    "LETPAST L(x) = q(x) OR (PREV L(x)) IN L(x) AND p(x)",
    # FRZ: the frozen predicate at the current time-point only (LET-equivalent)
    "FRZ F(x) = p(x) IN F(x)",
    "FRZ F(x) = p(x) IN F(x) AND q(x)",
    "FRZ F(x) = p(x) IN F(x) AND ONCE[0,5] q(x)",
    # FRZ temporal mode: frozen predicate under past operators
    "FRZ F(x) = p(x) IN ONCE[0,5] F(x)",
    "FRZ F(x) = p(x) IN q(x) AND ONCE[0,10] F(x)",
    "FRZ F(x) = p(x) IN F(x) SINCE[0,5] q(x)",
    "FRZ F(x) = p(x) IN q(x) SINCE[0,5] F(x)",
    "FRZ T(x,y) = r(x,y) IN ONCE[0,4] T(x,y)",
    # FRZ temporal mode: frozen predicate under future operators
    "FRZ F(x) = p(x) IN EVENTUALLY[0,5] F(x)",
    "FRZ F(x) = p(x) IN q(x) AND EVENTUALLY[0,3] F(x)",
    "FRZ F(x) = p(x) IN F(x) UNTIL[0,4] q(x)",
    # FRZ with PREV/NEXT (window-disabled paths)
    "FRZ F(x) = p(x) IN PREV[0,5] F(x)",
    "FRZ F(x) = p(x) IN NEXT[0,5] F(x)",
    # frozen definition with temporal operators (alpha lags / leads)
    "FRZ F(x) = (ONCE[0,5] p(x)) IN ONCE[0,3] F(x)",
    "FRZ F(x) = (EVENTUALLY[0,3] p(x)) IN ONCE[0,3] F(x)",
    "FRZ F(x) = (EVENTUALLY[0,3] p(x)) IN EVENTUALLY[0,3] F(x)",
    # unused frozen predicate (elided body)
    "FRZ F(x) = p(x) IN q(y)",
    # shadowing a signature predicate
    "FRZ p(x) = q(x) IN ONCE[0,5] p(x)",
    # nesting: FRZ in FRZ, FRZ with LET (both orders)
    "FRZ F(x) = p(x) IN FRZ G(x) = q(x) IN ONCE[0,5] (F(x) AND G(x))",
    "FRZ F(x) = p(x) IN ONCE[0,3] (FRZ G(x) = F(x) IN ONCE[0,3] G(x))",
    "LET U(x) = q(x) IN FRZ F(x) = U(x) IN ONCE[0,5] F(x)",
    "FRZ F(x) = p(x) IN LET U(x) = F(x) AND q(x) IN ONCE[0,5] U(x)",
    # aggregation over a frozen predicate
    "FRZ F(x) = p(x) IN c <- CNT x ONCE[0,5] F(x)",
    # --- deviation D1: div / mod / conversions inside constraints ---
    "r(x,y) AND z = x + y", "r(x,y) AND z = x - y",
    "r(x,y) AND z = x * y",
    "r(x,y) AND z = x / y",          # tdiv (explicitmon bug: tplus)
    "r(x,y) AND z = x MOD y",        # tmod (both staticmon and monpoly spell it MOD)
    "r(x,y) AND y = x / 2",
    "r(x,y) AND x = f2i(i2f(y))",    # f2i/i2f (explicitmon drops them)
    "p(x) AND y = x * 2 - 1",
    "r(x,y) AND x < y / 2",
]
for f in CURATED:
    emit(f)

# small random monitorable formulas. Productions are chosen to keep the result
# monitorable by construction (so the differential harness actually tests them)
# while spanning the reachable fragment: conjunction variants (join/antijoin/
# filter/assign), all interval shapes, aggregations (incl. over a temporal body),
# LET/LETPAST/nested-LET, arithmetic (+ - * / MOD, f2i/i2f), string/float
# constants, and negated-LHS since/until. rr-only operators (IMPLIES/EQUIV/FORALL/
# ALWAYS/HISTORICALLY/PAST_ALWAYS) are deliberately NOT generated -- staticmon
# rejects them without rr, so they would only be skipped.
rng = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 11)
PREDS = ["p(x)", "q(x)", "r(x,y)", "s(x,y)", "r(y,z)"]

# (guard, constraints monitorable given the guard's free variables)
CONSTRAINED = [
    ("r(x,y)", ["x = 5", "x < 3", "x = y", "x <= y", "NOT x = y",
                "z = x + y", "z = x - y", "z = x * y", "z = x / y",
                "z = x MOD y", "x = f2i(i2f(y))", "i2f(x) = 2.0", "y = x + 1"]),
    ("p(x)", ["x = 5", "x < 3", "NOT x = 5", "i2f(x) = 2.0", "y = x", "y = x * 2"]),
    ("a(u)", ['u = "aa"', 'NOT u = "bb"']),
    ("b(x,u)", ['u = "bb"', "x = 5"]),
]
ANTIJOINS = ["(p(x) AND NOT q(x))", "(r(x,y) AND NOT q(y))",
             "(r(x,y) AND NOT s(x,y))", "(b(x,u) AND NOT p(x))"]
# guaranteed-monitorable since/until, including negated-LHS (fv(lhs) subset fv(rhs))
SINCEUNTIL = [
    "(p(x) SINCE q(x))", "(p(x) SINCE[0,5] q(x))", "(r(x,y) SINCE[1,4] s(x,y))",
    "((NOT p(x)) SINCE q(x))", "((NOT p(x)) SINCE[0,4] q(x))",
    "(p(x) UNTIL[0,5] q(x))", "(r(x,y) UNTIL[1,4] s(x,y))",
    "((NOT p(x)) UNTIL[0,5] q(x))",
]
LETS = [
    "LET U(x) = q(x) IN U(x) AND p(x)",
    "LET U(x) = q(x) IN LET Z(x) = U(x) AND p(x) IN Z(x)",   # nested
    "LET T(x,y) = r(x,y) IN ONCE[0,3] T(x,y)",
    "LETPAST L(x) = p(x) OR (PREV L(x) AND q(x)) IN L(x)",
]


def frz(depth):
    """A random FRZ formula: freeze a random (possibly temporal) definition and
    use it in a random body position -- current-time, past, future, PREV/NEXT,
    nested in another binder -- so all three translation modes are hit."""
    name = rng.choice(["F", "G"])
    alpha = rng.choice(["p(x)", "q(x)", "(p(x) AND q(x))",
                        "(ONCE[0,4] p(x))", "(EVENTUALLY[0,3] p(x))",
                        "(p(x) AND NOT q(x))"])
    body = rng.choice([
        f"{name}(x)",
        f"{name}(x) AND q(x)",
        f"ONCE{_interval(False)} {name}(x)",
        f"q(x) AND ONCE[0,6] {name}(x)",
        f"{name}(x) SINCE[0,5] q(x)",
        f"q(x) SINCE[0,5] {name}(x)",
        f"EVENTUALLY[0,4] {name}(x)",
        f"{name}(x) UNTIL[0,4] q(x)",
        f"PREV[0,5] {name}(x)",
        f"NEXT[0,4] {name}(x)",
        f"ONCE[0,5] ({name}(x) AND q(x))",
        f"c <- CNT x ONCE[0,5] {name}(x)",
        f"FRZ G2(x) = q(x) IN ONCE[0,4] ({name}(x) AND G2(x))",
        f"LET U2(x) = {name}(x) AND q(x) IN ONCE[0,4] U2(x)",
    ])
    return f"(FRZ {name}(x) = {alpha} IN {body})"
AGGS = [
    "c <- CNT x; y r(x,y)", "c <- CNT x r(y,x)", "m <- SUM x; y r(y,x)",
    "m <- MIN x r(y,x)", "m <- MAX x; y r(y,x)", "avg <- AVG x; y r(y,x)",
    "md <- MED x r(y,x)", "c <- CNT x ONCE[0,3] p(x)",       # agg over temporal
]


def _interval(future):
    # future operators must be bounded; past ones may be unbounded / one-sided
    if future:
        return rng.choice(["[0,3]", "[0,5]", "[1,4]", "[3,3]"])
    return rng.choice(["", "[0,*)", "[0,5]", "[1,4]", "[3,3]", "[3,*)", "[0,10]"])


def small(depth):
    if depth <= 0 or rng.random() < 0.45:
        r = rng.random()
        if r < 0.6:
            return rng.choice(PREDS)
        if r < 0.8:
            g, cs = rng.choice(CONSTRAINED)
            return f"({g} AND {rng.choice(cs)})"
        return rng.choice(ANTIJOINS)
    r = rng.random()
    if r < 0.14:
        return f"({small(depth-1)} AND {rng.choice(PREDS)})"
    if r < 0.26:
        g, cs = rng.choice(CONSTRAINED)
        return f"({g} AND {rng.choice(cs)})"
    if r < 0.36:
        return rng.choice(ANTIJOINS)
    if r < 0.52:
        op = rng.choice(["ONCE", "PREV", "EVENTUALLY", "NEXT"])
        return f"{op}{_interval(op in ('EVENTUALLY', 'NEXT'))} {small(depth-1)}"
    if r < 0.64:
        return rng.choice(SINCEUNTIL)
    if r < 0.76:
        return f"(EXISTS {'x' if rng.random() < 0.6 else 'x, y'}. {small(depth-1)})"
    if r < 0.84:
        return rng.choice(LETS)
    if r < 0.92:
        return frz(depth - 1)
    return rng.choice(AGGS)


N = int(sys.argv[1]) if len(sys.argv) > 1 else 40
for _ in range(N):
    emit(small(rng.randint(1, 3)))
