#!/usr/bin/env python3
"""Structural-coverage model for MonPoly/MFOTL formulas.

Parses a formula into an AST and extracts a set of *structural features* -- the
operators, interval shapes, conjunction variants, negated-LHS temporals,
aggregation/let shapes, argument/constant types, and operator-nesting adjacencies
that appear. A fixed CHECKLIST enumerates the target features; test adequacy is
then "what fraction of the checklist did the generated corpus hit, and which
features are still uncovered". (Front-end and runtime coverage are later steps;
structural coverage maps closely to front-end AST-node handling.)

CLI: features.py <formulas-file> [sig-line-prefixed? no] -> prints a coverage
report over the non-comment lines.
"""
import re
import sys

# --------------------------------------------------------------------------- #
# Tokenizer                                                                   #
# --------------------------------------------------------------------------- #
KEYWORDS = {
    "AND", "OR", "NOT", "IMPLIES", "EQUIV", "EXISTS", "FORALL",
    "ONCE", "EVENTUALLY", "HISTORICALLY", "ALWAYS", "PAST_ALWAYS",
    "PREV", "PREVIOUS", "NEXT", "SINCE", "UNTIL", "TRIGGER", "RELEASE",
    "LET", "LETPAST", "IN", "TRUE", "FALSE",
    "CNT", "SUM", "MIN", "MAX", "AVG", "MED", "mod", "f2i", "i2f",
}
UNARY_TEMPORAL = {"ONCE", "EVENTUALLY", "HISTORICALLY", "ALWAYS", "PAST_ALWAYS",
                  "PREV", "PREVIOUS", "NEXT"}
BIN_TEMPORAL = {"SINCE", "UNTIL", "TRIGGER", "RELEASE"}
AGG_OPS = {"CNT", "SUM", "MIN", "MAX", "AVG", "MED"}

_TOK = re.compile(r"""
      (?P<ws>\s+)
    | (?P<str>"[^"]*")
    | (?P<flt>-?\d+\.\d+)
    | (?P<num>-?\d+)
    | (?P<larrow><-)
    | (?P<le><=)
    | (?P<ge>>=)
    | (?P<id>[A-Za-z_][A-Za-z0-9_]*)
    | (?P<punc>[()\[\],.;=<>*+\-/@])
""", re.VERBOSE)


def tokenize(s):
    toks, i = [], 0
    while i < len(s):
        m = _TOK.match(s, i)
        if not m:
            raise ValueError("cannot tokenize at %r" % s[i:i + 20])
        i = m.end()
        if m.lastgroup == "ws":
            continue
        toks.append((m.lastgroup, m.group()))
    toks.append(("eof", None))
    return toks


# --------------------------------------------------------------------------- #
# AST                                                                         #
# --------------------------------------------------------------------------- #
class N:
    def __init__(self, kind, **kw):
        self.kind = kind
        self.__dict__.update(kw)

    def __repr__(self):
        return "N(%s)" % self.kind


class Parser:
    def __init__(self, toks):
        self.t = toks
        self.i = 0

    def peek(self, k=0):
        return self.t[self.i + k]

    def kind(self):
        return self.t[self.i][0]

    def val(self):
        return self.t[self.i][1]

    def next(self):
        tok = self.t[self.i]
        self.i += 1
        return tok

    def eat(self, v):
        if self.val() != v:
            raise ValueError("expected %r, got %r" % (v, self.val()))
        return self.next()

    # ---- formulas ---------------------------------------------------------
    def parse(self):
        f = self.expr(0)
        if self.kind() != "eof":
            raise ValueError("trailing input at %r" % (self.val(),))
        return f

    # binding powers for the binary connectives (low binds looser)
    INFIX = {"EQUIV": 1, "IMPLIES": 2, "OR": 3, "AND": 4,
             "SINCE": 6, "UNTIL": 6, "TRIGGER": 6, "RELEASE": 6}

    def expr(self, min_bp):
        left = self.prefix()
        while True:
            v = self.val()
            bp = self.INFIX.get(v)
            if bp is None or bp < min_bp:
                break
            self.next()
            if v in BIN_TEMPORAL:
                intv = self.opt_interval()
                right = self.expr(bp + 1)
                left = N("bintemp", op=v, intv=intv, l=left, r=right)
            else:
                right = self.expr(bp + 1)
                left = N("bool", op=v, l=left, r=right)
        return left

    def prefix(self):
        v = self.val()
        if v in ("LET", "LETPAST"):
            return self.parse_let()
        # aggregation:  res <- OP var [; groups] body
        if self.kind() == "id" and self.peek(1)[1] == "<-":
            return self.parse_agg()
        if v == "NOT":
            self.next()
            return N("not", f=self.prefix())
        if v in UNARY_TEMPORAL:
            self.next()
            intv = self.opt_interval()
            return N("untemp", op=v, intv=intv, f=self.prefix())
        if v in ("EXISTS", "FORALL"):
            self.next()
            vs = [self.next()[1]]
            while self.val() == ",":
                self.next()
                vs.append(self.next()[1])
            self.eat(".")
            return N("quant", op=v, vars=vs, f=self.expr(0))
        return self.atom()

    def atom(self):
        if self.val() == "(":
            self.next()
            f = self.expr(0)
            self.eat(")")
            return f
        if self.val() in ("TRUE", "FALSE"):
            self.next()
            return N("bconst")
        # predicate or comparison. Parse a term; if it's a bare predicate node use
        # it, else expect a comparison operator.
        left = self.term()
        if left.kind == "pred":
            return left
        op = self.val()
        if op in ("=", "<", "<=", ">", ">="):
            self.next()
            right = self.term()
            return N("cmp", op=op, l=left, r=right)
        raise ValueError("expected predicate or comparison near %r" % (op,))

    def parse_let(self):
        kind = self.next()[1]                 # LET | LETPAST
        name = self.next()[1]
        self.eat("(")
        params = []
        if self.val() != ")":
            params.append(self.next()[1])
            while self.val() == ",":
                self.next()
                params.append(self.next()[1])
        self.eat(")")
        self.eat("=")
        defn = self.expr(0)
        self.eat("IN")
        body = self.expr(0)
        return N("let", letkind=kind, name=name, params=params, defn=defn, body=body)

    def parse_agg(self):
        resvar = self.next()[1]
        self.eat("<-")
        op = self.next()[1]                   # CNT/SUM/...
        aggvar = self.next()[1]
        groups = []
        if self.val() == ";":
            self.next()
            groups.append(self.next()[1])
            while self.val() == ",":
                self.next()
                groups.append(self.next()[1])
        body = self.expr(0)
        return N("agg", op=op, resvar=resvar, aggvar=aggvar, groups=groups, body=body)

    # ---- intervals & terms ------------------------------------------------
    def opt_interval(self):
        if self.val() != "[":
            return None
        self.next()
        lo = self.next()[1]
        self.eat(",")
        hi = self.next()[1]                    # number or '*'
        close = self.next()[1]                 # ')' or ']'
        return (int(lo), None if hi == "*" else int(hi))

    def term(self):
        # additive over multiplicative; also predicate application & functions
        return self.add_term()

    def add_term(self):
        left = self.mul_term()
        while self.val() in ("+", "-"):
            op = self.next()[1]
            left = N("arith", op=op, l=left, r=self.mul_term())
        return left

    def mul_term(self):
        left = self.unary_term()
        while self.val() in ("*", "/", "mod"):
            op = self.next()[1]
            left = N("arith", op=op, l=left, r=self.unary_term())
        return left

    def unary_term(self):
        k, v = self.peek()
        if k == "num":
            self.next()
            return N("tconst", ty="int")
        if k == "flt":
            self.next()
            return N("tconst", ty="float")
        if k == "str":
            self.next()
            return N("tconst", ty="string")
        if v == "(":
            self.next()
            t = self.add_term()
            self.eat(")")
            return t
        if v in ("f2i", "i2f"):
            self.next()
            self.eat("(")
            a = self.add_term()
            self.eat(")")
            return N("func", name=v, arg=a)
        if k == "id":
            name = self.next()[1]
            if self.val() == "(":                # predicate application
                self.next()
                args = []
                if self.val() != ")":
                    args.append(self.add_term())
                    while self.val() == ",":
                        self.next()
                        args.append(self.add_term())
                self.eat(")")
                return N("pred", name=name, args=args)
            return N("tvar", name=name)
        raise ValueError("unexpected term token %r" % (v,))


# --------------------------------------------------------------------------- #
# Free variables (for conjunction-variant classification)                     #
# --------------------------------------------------------------------------- #
def term_vars(t):
    if t.kind == "tvar":
        return {t.name}
    if t.kind == "arith":
        return term_vars(t.l) | term_vars(t.r)
    if t.kind == "func":
        return term_vars(t.arg)
    if t.kind == "pred":
        s = set()
        for a in t.args:
            s |= term_vars(a)
        return s
    return set()


def fv(n):
    k = n.kind
    if k == "pred":
        return term_vars(n)
    if k == "cmp":
        return term_vars(n.l) | term_vars(n.r)
    if k == "not":
        return fv(n.f)
    if k == "bool":
        return fv(n.l) | fv(n.r)
    if k == "bintemp":
        return fv(n.l) | fv(n.r)
    if k == "untemp":
        return fv(n.f)
    if k == "quant":
        return fv(n.f) - set(n.vars)
    if k == "agg":
        return {n.resvar} | set(n.groups)
    if k == "let":
        return fv(n.body)
    return set()


# --------------------------------------------------------------------------- #
# Feature extraction                                                          #
# --------------------------------------------------------------------------- #
def interval_shape(intv):
    if intv is None:
        return "intv:none"
    lo, hi = intv
    if hi is None:
        return "intv:unbounded" if lo == 0 else "intv:lower_inf"
    if lo == hi:
        return "intv:point"
    if lo == 0:
        return "intv:zero_upper"
    return "intv:two_sided"


def op_tag(n):
    """A short operator tag for a node, for nesting adjacencies."""
    k = n.kind
    if k == "bool":
        return {"AND": "and", "OR": "or", "IMPLIES": "implies", "EQUIV": "equiv"}[n.op]
    if k == "untemp":
        return n.op.lower()
    if k == "bintemp":
        return n.op.lower()
    return {"not": "not", "quant": "quant", "agg": "agg", "let": "let",
            "pred": "pred", "cmp": "cmp", "bconst": "bconst"}.get(k, k)


def extract(n, feats, adj, parent=None):
    """Collect features of node n into `feats`; record (parent,child) op adjacencies."""
    tag = op_tag(n)
    if parent is not None and tag not in ("pred", "cmp", "bconst") \
            and parent not in ("pred", "cmp", "bconst"):
        adj.add("adj:%s>%s" % (parent, tag))

    k = n.kind
    if k == "pred":
        feats.add("op:pred")
        for a in n.args:
            _term_feats(a, feats)
        return
    if k == "cmp":
        feats.add({"=": "op:eq", "<": "op:less", "<=": "op:leq",
                   ">": "op:less", ">=": "op:leq"}[n.op])
        _term_feats(n.l, feats)
        _term_feats(n.r, feats)
        return
    if k == "bconst":
        feats.add("op:const_bool")
        return
    if k == "not":
        feats.add("op:not")
        # NOT (x = y)  == inequality
        if n.f.kind == "cmp" and n.f.op == "=":
            feats.add("op:neq")
        extract(n.f, feats, adj, tag)
        return
    if k == "bool":
        feats.add({"AND": "op:and", "OR": "op:or",
                   "IMPLIES": "op:implies", "EQUIV": "op:equiv"}[n.op])
        if n.op == "AND":
            _conj_variant(n, feats)
        extract(n.l, feats, adj, tag)
        extract(n.r, feats, adj, tag)
        return
    if k == "quant":
        feats.add("op:exists" if n.op == "EXISTS" else "op:forall")
        if len(n.vars) > 1:
            feats.add("quant:multi")
        extract(n.f, feats, adj, tag)
        return
    if k == "untemp":
        feats.add("op:" + n.op.lower().replace("previous", "prev"))
        feats.add(interval_shape(n.intv))
        extract(n.f, feats, adj, tag)
        return
    if k == "bintemp":
        feats.add("op:" + n.op.lower())
        feats.add(interval_shape(n.intv))
        if n.l.kind == "not":
            feats.add("negLHS:" + n.op.lower())   # (NOT phi) SINCE/UNTIL psi
        extract(n.l, feats, adj, tag)
        extract(n.r, feats, adj, tag)
        return
    if k == "agg":
        feats.add("op:agg:" + n.op)
        feats.add("agg:grouped" if n.groups else "agg:ungrouped")
        if _has_temporal(n.body):
            feats.add("agg:over_temporal")
        extract(n.body, feats, adj, tag)
        return
    if k == "let":
        feats.add("op:letpast" if n.letkind == "LETPAST" else "op:let")
        if n.letkind == "LETPAST" and _refers(n.defn, n.name):
            feats.add("letpast:recursive")
        if _has_let(n.body):
            feats.add("let:nested")
        extract(n.defn, feats, adj, tag)
        extract(n.body, feats, adj, tag)
        return


def _term_feats(t, feats):
    if t.kind == "tconst":
        feats.add("const:" + t.ty)
    elif t.kind == "arith":
        feats.add({"+": "term:plus", "-": "term:minus", "*": "term:mult",
                   "/": "term:div", "mod": "term:mod"}[t.op])
        _term_feats(t.l, feats)
        _term_feats(t.r, feats)
    elif t.kind == "func":
        feats.add("term:" + t.name)
        _term_feats(t.arg, feats)
    elif t.kind == "pred":
        for a in t.args:
            _term_feats(a, feats)


def _conj_variant(n, feats):
    """Classify an AND node: join / antijoin / filter / assign."""
    r = n.r
    if r.kind == "not":
        feats.add("conj:antijoin")
        return
    if r.kind == "cmp":
        lvars = fv(n.l)
        if r.op == "=":
            lv = r.l.kind == "tvar"
            rv = r.r.kind == "tvar"
            if lv and term_vars(r.r) <= lvars and r.l.name not in lvars:
                feats.add("conj:assign"); return
            if rv and term_vars(r.l) <= lvars and r.r.name not in lvars:
                feats.add("conj:assign"); return
        feats.add("conj:filter")
        return
    feats.add("conj:join")


def _has_temporal(n):
    if n.kind in ("untemp", "bintemp"):
        return True
    for c in ("f", "l", "r", "body", "defn"):
        if hasattr(n, c) and isinstance(getattr(n, c), N) and _has_temporal(getattr(n, c)):
            return True
    return False


def _has_let(n):
    if n.kind == "let":
        return True
    for c in ("f", "l", "r", "body", "defn"):
        if hasattr(n, c) and isinstance(getattr(n, c), N) and _has_let(getattr(n, c)):
            return True
    return False


def _refers(n, name):
    if n.kind == "pred" and n.name == name:
        return True
    for c in ("f", "l", "r", "body", "defn"):
        if hasattr(n, c) and isinstance(getattr(n, c), N) and _refers(getattr(n, c), name):
            return True
    return False


def features(formula):
    ast = Parser(tokenize(formula)).parse()
    feats, adj = set(), set()
    extract(ast, feats, adj)
    return feats, adj


# --------------------------------------------------------------------------- #
# Checklist + coverage report                                                 #
# --------------------------------------------------------------------------- #
CHECKLIST = {
    # operators
    "op:pred", "op:eq", "op:neq", "op:less", "op:leq",
    "op:and", "op:or", "op:not", "op:implies", "op:equiv",
    "op:exists", "op:forall",
    "op:prev", "op:next", "op:once", "op:eventually",
    "op:historically", "op:always", "op:past_always",
    "op:since", "op:until",
    "op:agg:CNT", "op:agg:SUM", "op:agg:MIN", "op:agg:MAX", "op:agg:AVG", "op:agg:MED",
    "op:let", "op:letpast",
    # terms / consts
    "term:plus", "term:minus", "term:mult", "term:div", "term:mod",
    "term:f2i", "term:i2f", "const:int", "const:string", "const:float",
    # interval shapes
    "intv:none", "intv:unbounded", "intv:zero_upper", "intv:lower_inf",
    "intv:two_sided", "intv:point",
    # conjunction variants
    "conj:join", "conj:antijoin", "conj:filter", "conj:assign",
    # negated-LHS temporal
    "negLHS:since", "negLHS:until",
    # aggregation / let / quant shapes
    "agg:grouped", "agg:ungrouped", "agg:over_temporal",
    "let:nested", "letpast:recursive", "quant:multi",
}


def coverage(formulas):
    hit, adj = set(), set()
    unparsed = []
    for f in formulas:
        try:
            fe, ad = features(f)
        except Exception as e:                # noqa: BLE001
            unparsed.append((f, str(e)))
            continue
        hit |= fe
        adj |= ad
    covered = hit & CHECKLIST
    return covered, sorted(CHECKLIST - covered), sorted(adj), unparsed


def main():
    path = sys.argv[1]
    formulas = [ln.strip() for ln in open(path)
                if ln.strip() and not ln.strip().startswith("#")]
    covered, uncovered, adj, unparsed = coverage(formulas)
    n = len(CHECKLIST)
    print("structural coverage: %d/%d checklist features (%.0f%%)"
          % (len(covered), n, 100.0 * len(covered) / n))
    print("distinct nesting adjacencies observed: %d" % len(adj))
    if uncovered:
        print("uncovered (%d): %s" % (len(uncovered), ", ".join(uncovered)))
    if unparsed:
        print("unparsed (%d):" % len(unparsed))
        for f, e in unparsed[:10]:
            print("  %-50s  %s" % (f[:50], e))


if __name__ == "__main__":
    main()
