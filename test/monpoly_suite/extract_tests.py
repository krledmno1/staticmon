#!/usr/bin/env python3
"""Extract (signature, formula, trace) triples from MonPoly's cram test suite
so they can be replayed against staticmon.

For each `<name>.t/` directory under the given tests dir we collect:
  - the signature file (`*.sig`, first one);
  - the trace (`*.log`, first one), converted to staticmon's format (one
    `@<ts> <events>;` per line -- monpoly accepts this too);
  - every formula: the `*.mfotl` file(s) plus any inline `echo '<f>' > x.mfotl`
    in `run.t`.

Writes the converted log and each formula into <staging>/ and prints a TSV
manifest (test_id, sig_path, log_path, mfotl_path) to stdout. Tests with no
signature or no trace are skipped (nothing to run behaviorally).

Usage: extract_tests.py <monpoly_tests_dir> <staging_dir>
"""
import os
import re
import sys

tests_dir = sys.argv[1]
staging = sys.argv[2]
os.makedirs(staging, exist_ok=True)


def parse_sig(sig_text: str):
    """predicate name -> list of arg types (int/string/float/...)."""
    types = {}
    for m in re.finditer(r"(\w+)\s*\(([^)]*)\)", sig_text):
        args = [a.strip().split(":")[-1].strip()
                for a in m.group(2).split(",") if a.strip()]
        types[m.group(1)] = args
    return types


def quote_string_args(events: str, sig_types) -> str:
    """MonPoly allows unquoted (bareword) string values in traces; staticmon
    requires them quoted. A predicate is `name (t1) (t2) ...`; quote args at
    string-typed positions in each of its tuples."""
    def fix_pred(m):
        name, tuples_str = m.group(1), m.group(2)
        if name not in sig_types:
            return m.group(0)
        tys = sig_types[name]

        def fix_tuple(tm):
            argstr = tm.group(1)
            if not argstr.strip():
                return "()"
            args = [a.strip() for a in argstr.split(",")]
            for i, a in enumerate(args):
                if i < len(tys) and tys[i] == "string" and a and \
                        not a.startswith('"'):
                    args[i] = '"' + a + '"'
            return "(" + ",".join(args) + ")"

        return name + " " + re.sub(r"\(([^()]*)\)", fix_tuple, tuples_str)

    return re.sub(r"(\w+)\s*((?:\([^()]*\)\s*)+)", fix_pred, events)


def convert_log(raw: str, sig_types) -> str:
    """MonPoly trace -> one `@ts events;` per line."""
    # Strip MonPoly '#' line comments before collapsing timepoints onto one
    # line (otherwise a comment would swallow the rest of the collapsed line).
    lines = []
    for line in raw.splitlines():
        h = line.find("#")
        lines.append(line if h < 0 else line[:h])
    raw = "\n".join(lines)
    out = []
    for piece in raw.split("@")[1:]:
        piece = piece.replace(";", " ").strip()
        if not piece:
            continue
        toks = piece.split()
        ts = toks[0]
        if not ts.lstrip("-").isdigit():
            continue  # a command (> ... <) or malformed chunk
        events = quote_string_args(" ".join(toks[1:]), sig_types)
        # A present nullary predicate may be written as a bare name (MonPoly
        # shorthand); staticmon needs `name()`. Add parens when a nullary
        # predicate name is not already followed by `(`.
        for p, tys in sig_types.items():
            if len(tys) == 0:
                events = re.sub(rf"\b{re.escape(p)}\b(?!\s*\()", p + "()", events)
        out.append(f"@{ts} {events};")
    return "\n".join(out) + "\n"


def formulas_of(test_path: str):
    forms = []
    for fn in sorted(os.listdir(test_path)):
        if fn.endswith(".mfotl"):
            with open(os.path.join(test_path, fn)) as f:
                s = f.read().strip()
            if s:
                forms.append(s)
    run_t = os.path.join(test_path, "run.t")
    if os.path.isfile(run_t):
        for m in re.finditer(r"echo\s+'([^']*)'\s*>\s*\S+\.mfotl", open(run_t).read()):
            s = m.group(1).strip()
            if s:
                forms.append(s)
    # dedup, preserve order
    seen, uniq = set(), []
    for s in forms:
        if s not in seen:
            seen.add(s)
            uniq.append(s)
    return uniq


def first_glob(test_path, ext):
    fs = sorted(f for f in os.listdir(test_path) if f.endswith(ext))
    return os.path.join(test_path, fs[0]) if fs else None


count = 0
for d in sorted(os.listdir(tests_dir)):
    tp = os.path.join(tests_dir, d)
    if not (d.endswith(".t") and os.path.isdir(tp)):
        continue
    sig = first_glob(tp, ".sig")
    log = first_glob(tp, ".log")
    if not sig or not log:
        continue
    name = d[:-2]
    sig_types = parse_sig(open(sig).read())
    conv_log = os.path.join(staging, f"{name}.log")
    with open(conv_log, "w") as f:
        f.write(convert_log(open(log).read(), sig_types))
    for i, formula in enumerate(formulas_of(tp)):
        mfotl = os.path.join(staging, f"{name}__{i}.mfotl")
        with open(mfotl, "w") as f:
            f.write(formula)
        print(f"{name}__{i}\t{os.path.abspath(sig)}\t{os.path.abspath(conv_log)}\t{os.path.abspath(mfotl)}")
        count += 1

sys.stderr.write(f"extracted {count} (formula, sig, trace) cases\n")
