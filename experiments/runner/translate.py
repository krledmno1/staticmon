#!/usr/bin/env python3
"""Translate the benchmark harness's MonPoly-format sig / formula / trace into
the input formats of the external monitors (timelymon, whymon, dejavu).

The harness (experiments/runner) generates every microbenchmark as a MonPoly
signature (`P(int,int)`), a MonPoly word-syntax formula (`ONCE[0,7] P(x0,x1)`,
`P(...) SINCE [0,5] Q(...)`, `(ONCE A(x)) AND (x = 5)`, ...) over `@ts P(a,b) ...;`
traces. This script converts those to each tool's format.

Usage:
    translate.py --to {timelymon|whymon|dejavu} --kind {sig|formula|trace} IN OUT

Formula translation:
  * whymon    -- MonPoly word syntax is accepted as-is (identity).
  * timelymon -- identical except the previous operator is spelled PREVIOUS.
  * dejavu    -- genuinely different syntax; only the past one-sided-metric
                 fragment is supported (the harness's fragment filter guarantees
                 nothing else reaches dejavu), so we parse and re-print.

Trace translation:
  * whymon    -- MonPoly log minus the trailing `;`.
  * timelymon -- `Name, tp=<n>, ts=<ts>, x0=.., x1=..` lines + `>WATERMARK n<`.
  * dejavu    -- CSV rows `Name,arg0,arg1,<ts>` (timestamp last; the caller must
                 give the output a `*.timed.csv` name for time to be read).

Signature translation:
  * whymon / timelymon -- `P(int,int)` is accepted as-is.
  * dejavu    -- signatures are optional (arities are inferred); emitted as
                 comments for reference.
"""
import argparse
import re
import sys

# --------------------------------------------------------------------------- #
# MonPoly-subset formula parser (only needed for dejavu).                      #
# --------------------------------------------------------------------------- #

KEYWORDS = {
    "AND", "OR", "NOT", "IMPLIES", "EQUIV", "EXISTS", "FORALL",
    "ONCE", "EVENTUALLY", "HISTORICALLY", "ALWAYS", "PREV", "NEXT",
    "SINCE", "UNTIL", "TRIGGER", "RELEASE",
}
UNARY_TEMPORAL = {"ONCE", "EVENTUALLY", "HISTORICALLY", "ALWAYS", "PREV", "NEXT"}
BINARY_TEMPORAL = {"SINCE", "UNTIL", "TRIGGER", "RELEASE"}

_TOKEN_RE = re.compile(r"""
      (?P<ws>\s+)
    | (?P<num>-?\d+)
    | (?P<id>[A-Za-z_][A-Za-z0-9_]*)
    | (?P<punc>[()\[\],.=<>*])
    | (?P<le><=)
""", re.VERBOSE)


def tokenize(s):
    toks, i = [], 0
    while i < len(s):
        m = _TOKEN_RE.match(s, i)
        if not m:
            raise ValueError(f"cannot tokenize at {s[i:i+20]!r}")
        i = m.end()
        if m.lastgroup == "ws":
            continue
        toks.append(m.group())
    toks.append(None)  # EOF sentinel
    return toks


class Node:
    def __init__(self, kind, **kw):
        self.kind = kind
        self.__dict__.update(kw)


class Parser:
    def __init__(self, toks):
        self.toks = toks
        self.pos = 0

    def peek(self):
        return self.toks[self.pos]

    def next(self):
        t = self.toks[self.pos]
        self.pos += 1
        return t

    def expect(self, t):
        got = self.next()
        if got != t:
            raise ValueError(f"expected {t!r}, got {got!r}")

    # formula := or_expr
    def parse(self):
        f = self.or_expr()
        if self.peek() is not None:
            raise ValueError(f"trailing input at {self.peek()!r}")
        return f

    def or_expr(self):
        f = self.and_expr()
        while self.peek() == "OR":
            self.next()
            f = Node("or", l=f, r=self.and_expr())
        return f

    def and_expr(self):
        f = self.temporal_expr()
        while self.peek() == "AND":
            self.next()
            f = Node("and", l=f, r=self.temporal_expr())
        return f

    # binary temporal (SINCE/UNTIL/...) between two unary expressions
    def temporal_expr(self):
        f = self.unary()
        if self.peek() in BINARY_TEMPORAL:
            op = self.next()
            intv = self.opt_interval()
            g = self.unary()
            return Node("bintemporal", op=op, intv=intv, l=f, r=g)
        return f

    def unary(self):
        t = self.peek()
        if t == "NOT":
            self.next()
            return Node("not", f=self.unary())
        if t in UNARY_TEMPORAL:
            self.next()
            intv = self.opt_interval()
            return Node("untemporal", op=t, intv=intv, f=self.unary())
        if t in ("EXISTS", "FORALL"):
            self.next()
            vs = [self.next()]
            while self.peek() == ",":
                self.next()
                vs.append(self.next())
            self.expect(".")
            return Node("quant", op=t, vars=vs, f=self.unary())
        return self.atom()

    def atom(self):
        t = self.peek()
        if t == "(":
            self.next()
            f = self.or_expr()
            self.expect(")")
            return f
        # predicate or comparison
        left = self.term_or_pred()
        if isinstance(left, Node):
            return left
        # `left` is a bare term -> must be a comparison (x = 5)
        op = self.peek()
        if op in ("=", "<", ">", "<="):
            self.next()
            right = self.next()
            return Node("cmp", op=op, l=left, r=right)
        raise ValueError(f"unexpected term {left!r}")

    def term_or_pred(self):
        name = self.next()
        if name in KEYWORDS or name is None:
            raise ValueError(f"expected atom, got {name!r}")
        if self.peek() == "(":  # predicate application
            self.next()
            args = []
            if self.peek() != ")":
                args.append(self.next())
                while self.peek() == ",":
                    self.next()
                    args.append(self.next())
            self.expect(")")
            return Node("pred", name=name, args=args)
        return name  # a bare term (variable or constant)

    def opt_interval(self):
        if self.peek() != "[":
            return None
        self.next()
        lo = self.next()
        self.expect(",")
        hi = self.next()  # int or '*'
        close = self.next()  # ')' or ']'
        if close not in (")", "]"):
            raise ValueError(f"bad interval close {close!r}")
        hi = None if hi == "*" else int(hi)
        return (int(lo), hi)


# --------------------------------------------------------------------------- #
# dejavu formula printer (past one-sided-metric fragment only).               #
# --------------------------------------------------------------------------- #

def _dejavu_bound(op, intv):
    """MonPoly interval -> dejavu metric suffix for a past operator."""
    if intv is None:
        return ""
    lo, hi = intv
    if lo == 0 and hi is None:
        return ""                    # untimed
    if lo == 0 and hi is not None:
        return f"[<={hi}]"           # diff <= hi
    if lo > 0 and hi is None:
        return f"[>{lo - 1}]"        # diff >= lo  <=>  diff > lo-1
    raise ValueError(f"two-sided interval [{lo},{hi}] not in dejavu fragment")


def _is_const(tok):
    return re.fullmatch(r"-?\d+", tok) is not None


def free_vars(node, bound=frozenset()):
    """Free (non-constant) variables of a parsed formula, respecting quantifiers.
    DejaVu only accepts closed properties, so the printer universally closes over
    these."""
    k = node.kind
    if k == "pred":
        return {a for a in node.args if not _is_const(a)} - bound
    if k == "cmp":
        return {t for t in (node.l, node.r) if not _is_const(t)} - bound
    if k == "not":
        return free_vars(node.f, bound)
    if k in ("and", "or", "bintemporal"):
        return free_vars(node.l, bound) | free_vars(node.r, bound)
    if k == "untemporal":
        return free_vars(node.f, bound)
    if k == "quant":
        return free_vars(node.f, bound | set(node.vars))
    return set()


def dejavu_rename(name):
    """Predicate names P/Q/A collide with dejavu's P/H/S operator keywords, so
    prefix every event predicate with `ev_` (applied identically to the trace)."""
    return "ev_" + name


def to_dejavu(node):
    k = node.kind
    if k == "pred":
        nm = dejavu_rename(node.name)
        return f"{nm}({','.join(node.args)})" if node.args else nm
    if k == "cmp":
        return f"{node.l} {node.op} {node.r}"
    if k == "not":
        return f"! ({to_dejavu(node.f)})"
    if k == "and":
        return f"({to_dejavu(node.l)}) & ({to_dejavu(node.r)})"
    if k == "or":
        return f"({to_dejavu(node.l)}) | ({to_dejavu(node.r)})"
    if k == "quant":
        if node.op != "EXISTS":
            raise ValueError("dejavu fragment: only EXISTS supported here")
        inner = to_dejavu(node.f)
        for v in reversed(node.vars):
            inner = f"exists {v} . ({inner})"
        return inner
    if k == "untemporal":
        op, intv, f = node.op, node.intv, to_dejavu(node.f)
        if op == "ONCE":
            return f"P{_dejavu_bound(op, intv)} ({f})"
        if op == "HISTORICALLY":
            return f"H{_dejavu_bound(op, intv)} ({f})"
        if op == "PREV":
            if intv not in (None, (0, None)):
                raise ValueError("dejavu has no metric previous")
            return f"@ ({f})"
        raise ValueError(f"dejavu fragment: unsupported unary {op}")
    if k == "bintemporal":
        if node.op != "SINCE":
            raise ValueError(f"dejavu fragment: unsupported binary {node.op}")
        return f"({to_dejavu(node.l)}) S{_dejavu_bound('SINCE', node.intv)} ({to_dejavu(node.r)})"
    raise ValueError(f"cannot print node {k}")


# --------------------------------------------------------------------------- #
# Formula translation entry points.                                           #
# --------------------------------------------------------------------------- #

def translate_formula(tool, text):
    text = text.strip()
    if tool == "whymon":
        return text
    if tool == "timelymon":
        return re.sub(r"\bPREV\b", "PREVIOUS", text)
    if tool == "dejavu":
        ast = Parser(tokenize(text)).parse()
        body = to_dejavu(ast)
        # DejaVu properties must be closed: universally close over free variables
        # (the bulk BDD work over the data is unchanged; only the top collapses to
        # a boolean). Capital Forall = quantify over the whole domain.
        for v in sorted(free_vars(ast)):
            body = f"Forall {v} . ({body})"
        return f"prop p0 : {body}"
    raise ValueError(tool)


# --------------------------------------------------------------------------- #
# Trace translation.                                                          #
# --------------------------------------------------------------------------- #

# One MonPoly db line: "@<ts>  P(a,b) Q(c) ... ;"
_EVENT_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)")


def parse_db_line(line):
    """Return (ts, [(name, [args...]), ...]) or None for a blank/`;`-only line."""
    line = line.strip()
    if not line or not line.startswith("@"):
        return None
    body = line.lstrip("@")
    # timestamp = leading integer
    m = re.match(r"\s*(-?\d+)", body)
    if not m:
        return None
    ts = int(m.group(1))
    rest = body[m.end():]
    events = []
    for em in _EVENT_RE.finditer(rest):
        name = em.group(1)
        argstr = em.group(2).strip()
        args = [a.strip() for a in argstr.split(",")] if argstr else []
        events.append((name, args))
    return ts, events


def iter_db_lines(fin):
    for line in fin:
        parsed = parse_db_line(line)
        if parsed is not None:
            yield parsed


def trace_to_timelymon(fin, fout):
    tp = 0
    for ts, events in iter_db_lines(fin):
        for name, args in events:
            kv = ", ".join(f"x{i}={a}" for i, a in enumerate(args))
            sep = ", " + kv if kv else ""
            fout.write(f"{name}, tp={tp}, ts={ts}{sep}\n")
        fout.write(f">WATERMARK {tp}<\n")
        tp += 1
    fout.write(f">WATERMARK {tp}<\n")


def trace_to_dejavu(fin, fout):
    for ts, events in iter_db_lines(fin):
        for name, args in events:
            fields = [dejavu_rename(name)] + args + [str(ts)]
            fout.write(",".join(fields) + "\n")


def trace_to_whymon(fin, fout):
    for line in fin:
        s = line.rstrip("\n")
        if s.strip().startswith("@"):
            fout.write(s.replace(";", "").rstrip() + "\n")


def translate_trace(tool, fin, fout):
    if tool == "timelymon":
        trace_to_timelymon(fin, fout)
    elif tool == "dejavu":
        trace_to_dejavu(fin, fout)
    elif tool == "whymon":
        trace_to_whymon(fin, fout)
    else:
        raise ValueError(tool)


# --------------------------------------------------------------------------- #
# Signature translation.                                                      #
# --------------------------------------------------------------------------- #

def translate_sig(tool, text):
    text = text.strip()
    if tool in ("whymon", "timelymon"):
        return text  # `P(int,int)` accepted as-is
    if tool == "dejavu":
        # arities are inferred by dejavu; keep the sig as a comment for reference
        return "\n".join("// " + ln for ln in text.splitlines() if ln.strip())
    raise ValueError(tool)


# --------------------------------------------------------------------------- #

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--to", required=True, choices=["timelymon", "whymon", "dejavu"])
    ap.add_argument("--kind", required=True, choices=["sig", "formula", "trace"])
    ap.add_argument("infile")
    ap.add_argument("outfile")
    a = ap.parse_args()

    if a.kind == "trace":
        with open(a.infile) as fin:
            content = fin.read()
        # guard against silently emitting an empty trace from a malformed log
        if content.strip() and not any(
                ln.lstrip().startswith("@") for ln in content.splitlines()):
            sys.exit(f"translate.py: log {a.infile} has events but no "
                     f"@-timestamped time-points")
        import io
        with open(a.outfile, "w") as fout:
            translate_trace(a.to, io.StringIO(content), fout)
        return
    with open(a.infile) as fin:
        text = fin.read()
    out = translate_formula(a.to, text) if a.kind == "formula" else translate_sig(a.to, text)
    with open(a.outfile, "w") as fout:
        fout.write(out + "\n")


if __name__ == "__main__":
    main()
