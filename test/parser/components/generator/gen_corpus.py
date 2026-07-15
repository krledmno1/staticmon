#!/usr/bin/env python3
"""Corpus generator for differential-testing staticmon's C++ formula parser
against the MonPoly oracle (parser/components/oracle).

Emits length-prefixed frames (decimal byte count line + bytes) on stdout —
the shared stdin protocol of parser_dump and oracle.exe. Deliberately
includes formulas that should FAIL to parse: both parsers must agree on the
ok/parse_error category (and on the AST when ok).

Sections:
  1. every operator/production in isolation
  2. all ordered pairs of binary connectives (precedence/associativity)
  3. unary x binary interactions, quantifier scoping
  4. interval shapes, time units
  5. term-level arithmetic precedence and parenthesization
  6. lexer quirks (identifier charset, negative literals, comments, strings)
  7. regex (MFODL) fragment
  8. aggregations
  9. seeded random formulas (grammar-directed)
"""

import random
import sys

out = sys.stdout.buffer


def emit(formula: str):
    b = formula.encode()
    out.write(str(len(b)).encode() + b"\n" + b)


ATOMS = ["P(x)", "Q(y)", "R(z)", "S(x,y)", "x = 5", "TRUE", "FALSE"]
BINOPS = ["AND", "OR", "IMPLIES", "EQUIV", "SINCE", "UNTIL", "TRIGGER",
          "RELEASE", "SINCE [0,5]", "UNTIL (2,10)"]
UNOPS = ["NOT", "EXISTS x.", "FORALL y.", "PREV", "NEXT", "EVENTUALLY",
         "ONCE", "ALWAYS", "PAST_ALWAYS", "PREV [1,2]", "ONCE [0,*)",
         "EVENTUALLY (0,7]"]

# --- 1. single productions -------------------------------------------------
singles = [
    "P()", "P(x)", "P(x,y,z)", "P(5)", "P(\"str\")", "P(1.5)", "P(x+1)",
    "P(_)", "P(_,_)", "P(_x)", "P(_x,_x)", "P(x,_,y)",
    "TRUE", "FALSE",
    "x = 5", "x < 5", "x <= 5", "x > 5", "x >= 5",
    "\"a\" SUBSTRING \"ab\"", "x SUBSTRING y",
    "x MATCHES r\"a.*b\"", "x MATCHES y (a, _, 5)", "x MATCHES y ()",
    "NOT P(x)",
    "P(x) AND Q(y)", "P(x) OR Q(y)", "P(x) IMPLIES Q(y)", "P(x) EQUIV Q(y)",
    "EXISTS x. P(x)", "EXISTS x, y. S(x,y)", "FORALL x. P(x)",
    "FORALL x, y, z. P(x)",
    "PREV P(x)", "NEXT P(x)", "EVENTUALLY P(x)", "SOMETIMES P(x)",
    "ONCE P(x)", "ALWAYS P(x)", "PAST_ALWAYS P(x)", "HISTORICALLY P(x)",
    "PREVIOUS P(x)",
    "P(x) SINCE Q(y)", "P(x) UNTIL Q(y)", "P(x) TRIGGER Q(y)",
    "P(x) RELEASE Q(y)",
    "LET P(x) = Q(x) IN P(5)", "LETPAST P(x) = Q(x) IN P(5)",
    "FRZ P(x) = Q(x) IN P(5)",
    "LET P(_) = Q(x) IN P(5)",        # add_ex in LET head -> error
    "c <- CNT x P(x)", "s <- SUM x; y S(x,y)", "m <- MED x; a, b P(x)",
    "a <- AVG x P(x,y)", "mx <- MAX x P(x)", "mn <- MIN x P(x)",
    "c <- CNT z P(x)",                 # agg var not free -> error
    "c <- CNT x; w P(x)",              # group-by var not free -> error
    "|> P(x)", "<| P(x)", "FORWARD P(x)", "BACKWARD P(x)",
    "MATCHF P(x)", "MATCHP P(x)",
    "EXISTS . P(x)",                   # empty varlist -> error
    "P(x) | Q(y)",                     # BAR unused by grammar -> error
    "a = b = c",                       # nonassoc -> error
    "x",                               # bare term -> error
    "",                                # empty input -> error
]
for f in singles:
    emit(f)

# --- 2. binary x binary precedence pairs ------------------------------------
for op1 in BINOPS:
    for op2 in BINOPS:
        emit(f"P(x) {op1} Q(y) {op2} R(z)")

# --- 3. unary x binary interactions -----------------------------------------
for u in UNOPS:
    for b in ["AND", "OR", "IMPLIES", "SINCE", "UNTIL [0,3]"]:
        emit(f"{u} P(x) {b} Q(y)")
        emit(f"P(x) {b} {u} Q(y)")
for u1 in UNOPS:
    for u2 in ["NOT", "PREV", "EXISTS x.", "ONCE [1,2]"]:
        emit(f"{u1} {u2} P(x)")
# quantifier scoping vs every binop
for b in BINOPS:
    emit(f"EXISTS x. P(x) {b} Q(x)")
    emit(f"FORALL x. P(x) {b} Q(x)")

# --- 4. intervals ------------------------------------------------------------
interval_cases = [
    "[0,0]", "[0,1)", "(0,1]", "(0,1)", "[0,*)", "[0,*]", "(1,*)",
    "[5,3]", "[2d,3d]", "[1h,2h]", "[30m,1h]", "[59s,1m]", "[2d,*)",
    "[-1,5]", "[-5,-1]", "(0,0)",
    "[999999999999999999999999,*)",           # beyond int64: Z is fine
    "[106751991167300d,*)",                    # d-multiply overflows 63-bit
    "[4611686018427387903,*)",                 # OCaml max_int
    "[5x,10]",                                 # bad unit letter -> error
    "[5,10", "[5 10]", "[,10]", "[5,]",        # malformed -> error
]
for i in interval_cases:
    emit(f"ONCE {i} P(x)")
emit("P(x) SINCE [1,2] Q(y)")
emit("P(x) SINCE (1,2] Q(y)")
emit("P(x) UNTIL [0,0] Q(y)")

# --- 5. term arithmetic -------------------------------------------------------
terms = [
    "x + y = z", "x - y = z", "x * y = z", "x / y = z", "x MOD y = z",
    "x + y * z = w", "x * y + z = w", "(x + y) * z = w",
    "x + y + z = w", "x - y - z = w", "x / y / z = w",
    "- x = y", "- x + y = z", "x + - y = z", "- (x + y) = z",
    "f2i(x) = y", "i2f(x) = y", "s2i(x) = y", "i2s(x) = y",
    "f2s(x) = y", "s2f(x) = y", "r2s(x) = y", "s2r(x) = y",
    "DAY_OF_MONTH(x) = y", "MONTH(x) = y", "YEAR(x) = y",
    "FORMAT_DATE(x) = y",
    "f2i(x + y) * 2 = z", "f2i(i2f(x)) = y",
    "(x) = 5", "((x)) = 5", "(x) + 1 = y", "((x + y)) * 2 = z",
    "(P(x))", "((P(x)))", "(P(x) AND Q(y))",
    "(x = 5)", "((x = 5))", "(x = 5) AND Q(y)",
    "5 = 5", "1.5 = x", "1. = x", "\"foo\" = x", "r\"pat\" = x",
    "0 = -0", "007 = 7", "-007 = -7",
    "99999999999999999999999999999 = x",
    "-99999999999999999999999999999 = x",
    "1.5 + 2.5 = x", "1.79769313486231571e308 = x",
    "3.14159265358979312 = x",
]
for t in terms:
    emit(t)

# --- 6. lexer quirks ----------------------------------------------------------
lexer_cases = [
    "x-5 = y",          # x-5 is ONE identifier
    "x - 5 = y",        # subtraction
    "x -5 = y",         # x INT(-5): error (no comparison)
    "x - -5 = y",       # Minus(x, -5)
    "5-3 = x",          # identifier "5-3"
    "a/b = c", "a:b = c", "a'b = c",
    "ANDx(y)", "TRUEfoo(y)", "NOTP(x)",       # keyword-prefix identifiers
    "and(x)", "true(x)", "since(x)",          # lowercase = identifiers
    "P(x) AND(*c*)Q(y)", "P(x)(*c*)AND Q(y)", "(*c*) P(x)",
    "P(x) # comment\n AND Q(y)",
    "P(x) (* multi\nline *) AND Q(y)",
    "P(x) (* unterminated",                    # -> error
    "P(\"a b\")", 'P("a\\"b")', 'P("a\\\\b")', 'P("")',
    'P("tab\\there")', 'P("nl\\nhere")',
    "123abc(x)",        # identifier starting with digits
    "_x = 5",           # identifier _x as term var
    "_ = 5",            # LD not a term -> error
    "x = _",            # -> error
    "P(x,)",            # trailing comma -> error
    "P(,x)",            # -> error
    "1.5.5 = x",        # RAT then .5 -> error
    ".5 = x",           # DOT INT -> error
    "▷ P(x)",      # unicode FREX
    "◁ P(x)",      # unicode PREX
    "P(x) AND", "AND P(x)", "()", "(", ")",   # -> errors
]
for f in lexer_cases:
    emit(f)

# --- 7. regexes ----------------------------------------------------------------
regex_cases = [
    "|> P(x) Q(y)", "|> P(x) Q(y) R(z)",
    "|> P(x) + Q(y)", "|> P(x) Q(y) + R(z)", "|> P(x) + Q(y) R(z)",
    "|> P(x)?", "|> P(x)? Q(y)", "|> .", "|> . .", "|> . P(x) .",
    "|> P(x)*", "|> P(x)?*", "|> (P(x) Q(y))*", "|> .*",
    "|> (P(x))", "|> (P(x) + Q(y)) R(z)", "|> ((P(x)))",
    "|> [0,5] P(x) Q(y)", "<| [0,5] P(x) Q(y)", "<| P(x)? . Q(y)",
    "|> P(x) AND Q(y)",            # regex ends at AND
    "|> x = 5 Q(y)", "|> (x) = 5", "|> NOT P(x) Q(y)",
    "|> EXISTS x. P(x)",           # quantifier inside regex atom
    "|> (|> P(x))", "|> (<| P(x)) Q(y)",
    "|> P(x) SINCE Q(y)",          # (|> P(x)) SINCE Q(y)
    "P(x) AND |> Q(y)", "NOT |> P(x)",
    "|> TRUE? Q(y)", "|> TRUE FALSE",
    "|>",                          # -> error
    "|> +",                        # -> error
]
for f in regex_cases:
    emit(f)

# --- 8. aggregation shapes -------------------------------------------------------
agg_cases = [
    "c <- CNT x P(x) AND Q(y)",      # body extent: swallows AND?
    "c <- CNT x P(x) SINCE Q(y)",
    "c <- CNT x EXISTS y. S(x,y)",
    "c <- CNT x; y, z P(x,y,z)",
    "c <- CNT x; y P(x,y) AND Q(z)",
    "c <- CNT x; y, z, P(x)",        # trailing comma in group-by -> error
    "c <- CNT x; a = 5",             # a eaten by group-by -> error
    "d <- SUM x; y c <- CNT x; y S(x,y)",  # nested aggregations
    "c <- CNT x (x = 5)",
    "x <- CNT x P(x)",               # res == agg var
    "NOT c <- CNT x P(x)",
    "c <- CNT x P(x) OR Q(c)",
]
for f in agg_cases:
    emit(f)

# --- 9. seeded random formulas ----------------------------------------------------
rng = random.Random(20260713)

VARS = ["x", "y", "z", "w"]
PREDS = [("P", 1), ("Q", 1), ("R", 2), ("S", 3), ("Z0", 0)]
CSTS = ["0", "1", "5", "42", "-3", '"s"', '"t u"', "1.5", "2.", "-0.5"]
FUNCS = ["f2i", "i2f", "i2s", "s2i", "f2s", "s2f", "r2s", "s2r",
         "DAY_OF_MONTH", "MONTH", "YEAR", "FORMAT_DATE"]


def rand_term(depth):
    r = rng.random()
    if depth <= 0 or r < 0.45:
        return rng.choice(VARS + CSTS)
    if r < 0.55:
        return f"{rng.choice(FUNCS)}({rand_term(depth - 1)})"
    if r < 0.62:
        return f"- {rand_term(depth - 1)}"
    if r < 0.72:
        return f"({rand_term(depth - 1)})"
    op = rng.choice(["+", "-", "*", "/", "MOD"])
    return f"{rand_term(depth - 1)} {op} {rand_term(depth - 1)}"


def rand_interval():
    r = rng.random()
    if r < 0.4:
        return ""
    lo = rng.choice(["0", "1", "2", "5", "1h", "2m", "30s", "1d"])
    hi = rng.choice(["3", "7", "10", "2h", "5m", "*", "2d"])
    lb = rng.choice("[(")
    rb = rng.choice("])")
    return f" {lb}{lo},{hi}{rb}"


def rand_pred():
    name, arity = rng.choice(PREDS)
    args = ", ".join(
        rng.choice(VARS + CSTS + ["_"]) if rng.random() < 0.8
        else rand_term(1)
        for _ in range(arity))
    return f"{name}({args})"


def rand_regex(depth):
    r = rng.random()
    if depth <= 0 or r < 0.35:
        if rng.random() < 0.25:
            return "."
        base = rand_pred() if rng.random() < 0.8 else "TRUE"
        return base + ("?" if rng.random() < 0.5 else "")
    if r < 0.55:
        return f"{rand_regex(depth - 1)} {rand_regex(depth - 1)}"
    if r < 0.75:
        return f"{rand_regex(depth - 1)} + {rand_regex(depth - 1)}"
    if r < 0.9:
        return f"({rand_regex(depth - 1)})*"
    return f"({rand_regex(depth - 1)})"


def rand_formula(depth):
    r = rng.random()
    if depth <= 0 or r < 0.30:
        c = rng.random()
        if c < 0.5:
            return rand_pred()
        if c < 0.6:
            return rng.choice(["TRUE", "FALSE"])
        op = rng.choice(["=", "<", "<=", ">", ">=", "SUBSTRING"])
        return f"{rand_term(1)} {op} {rand_term(1)}"
    if r < 0.36:
        return f"NOT {rand_formula(depth - 1)}"
    if r < 0.48:
        op = rng.choice(["AND", "OR", "IMPLIES", "EQUIV"])
        return f"{rand_formula(depth - 1)} {op} {rand_formula(depth - 1)}"
    if r < 0.58:
        op = rng.choice(["SINCE", "UNTIL", "TRIGGER", "RELEASE"])
        return f"{rand_formula(depth - 1)} {op}{rand_interval()} {rand_formula(depth - 1)}"
    if r < 0.68:
        op = rng.choice(["PREV", "NEXT", "EVENTUALLY", "ONCE", "ALWAYS",
                         "PAST_ALWAYS"])
        return f"{op}{rand_interval()} {rand_formula(depth - 1)}"
    if r < 0.76:
        q = rng.choice(["EXISTS", "FORALL"])
        vs = ", ".join(rng.sample(VARS, rng.randint(1, 2)))
        return f"{q} {vs}. {rand_formula(depth - 1)}"
    if r < 0.82:
        return f"({rand_formula(depth - 1)})"
    if r < 0.88:
        name, _ = rng.choice(PREDS[:2])
        return (f"LET {name}(x) = {rand_formula(depth - 1)} "
                f"IN {rand_formula(depth - 1)}")
    if r < 0.94:
        res, agg = "res", rng.choice(VARS)
        op = rng.choice(["CNT", "SUM", "MIN", "MAX", "AVG", "MED"])
        return f"{res} <- {op} {agg} {rand_formula(depth - 1)}"
    d = rng.choice(["|>", "<|"])
    return f"{d}{rand_interval()} {rand_regex(depth - 1)}"


N_RANDOM = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 20260713
rng = random.Random(SEED)
for _ in range(N_RANDOM):
    emit(rand_formula(rng.randint(1, 6)))

# --- 9b. signatures (frames marked with a leading "#SIG" comment line) ------
SIGS = [
    "P(int)", "P(int,string)", "P(int, string, float, regexp)",
    "P()", "P() Q(int)", "P(x:int)", "P(x:int, y:string)",
    "P(x:int:junk)", "P(:int)", "P(int:)",           # last -> error
    "P(int) P(int)",                                  # duplicate -> error
    "tp(int)", "ts(int)", "tpts(int,int)",            # base redefinition -> error
    "P(int) # comment\nQ(string)",
    "P(bool)",                                        # unknown type -> error
    "P(int,)", "P(,int)",                             # -> errors
    "\"quoted\"(int)", "r\"rq\"(int)",                # quoted pred names
    "P(\"x:int\")", "P(\"\")",                        # quoted args; empty -> error
    "p.q!r[0]/s:t(int)",                              # log-lexer ident charset
    "P(int", "P int", "(int)", "P((int))",            # -> errors
    "", "# only a comment\n",                         # empty schema (base only)
    "5(int)", "x-y(int)", "_(int)",
]
for s in SIGS:
    emit("#SIG\n" + s)

# --- 10. token soup: random token sequences, mostly invalid ------------------
# Both parsers must agree on the ok/parse_error category; occasionally the
# soup is valid and then ASTs must match. Exercises acceptance boundaries the
# grammar-directed generator cannot reach (trailing input, odd juxtaposition).
VOCAB = [
    "P(x)", "Q(y,z)", "x", "y", "5", "-3", "1.5", '"s"', "r\"p\"", "_", "TRUE",
    "FALSE", "(", ")", "[", "]", ",", ";", ".", "?", "*", "+", "-", "/", "MOD",
    "=", "<", "<=", ">", ">=", "<-", "AND", "OR", "IMPLIES", "EQUIV", "NOT",
    "EXISTS", "FORALL", "PREV", "NEXT", "ONCE", "ALWAYS", "EVENTUALLY",
    "PAST_ALWAYS", "SINCE", "UNTIL", "TRIGGER", "RELEASE", "LET", "LETPAST",
    "FRZ", "IN", "|>", "<|", "|", "CNT", "SUM", "MIN", "MAX", "AVG", "MED",
    "SUBSTRING", "MATCHES", "f2i", "s2r", "2d", "3h", "x-5", "5-3",
]
N_SOUP = N_RANDOM // 2
for _ in range(N_SOUP):
    k = rng.randint(1, 10)
    emit(" ".join(rng.choice(VOCAB) for _ in range(k)))
