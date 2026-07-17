#!/usr/bin/env bash
# Regenerate the FRZ fixtures from MonPoly's frz test suite (tests/frz*.t in
# monpoly-develop: 79 cram tests developed against the verified VeriMon FRZ
# implementation -- see docs/Freezing_MFOTL).
#
# OFFLINE maintainer tool -- needs monpoly (VeriMon) and the monpoly-develop
# checkout; nothing in the ctest flow runs it. For every frzNN.t whose formula
# both VeriMon and staticmon accept, writes
#   monitor/methods/fixture/cases-frz/<id>/{sig, formula, trace, expected}
# where `expected` is VeriMon's (-verified) verdict stream. Negative tests
# (formulas both tools reject) are skipped and reported.
#
# Usage: regen-frz.sh <staticmon-headers> [monpoly_tests_dir]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
SC=${1:?path to staticmon-headers binary}
TESTS=${2:-$HOME/Data/Projects/monpoly-develop/tests}
MP=${MONPOLY:-$(command -v monpoly 2>/dev/null || ls "$HOME"/.opam/*/bin/monpoly 2>/dev/null | head -1)}
CASES="$here/cases-frz"

{ [ -n "$MP" ] && [ -x "$MP" ]; } || { echo "monpoly not found (set \$MONPOLY)" >&2; exit 1; }
[ -d "$TESTS" ] || { echo "monpoly tests not found: $TESTS (arg 2)" >&2; exit 1; }
[ -x "$SC" ]    || { echo "staticmon-headers not found: $SC" >&2; exit 1; }

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/hdr"

rm -rf "$CASES"; mkdir -p "$CASES"
kept=0 vm_reject=0 sm_reject=0
for tdir in "$TESTS"/frz*.t; do
  id=$(basename "$tdir" .t)
  sig="$tdir/$id.sig"; mfotl="$tdir/$id.mfotl"; log="$tdir/$id.log"
  [ -f "$sig" ] && [ -f "$mfotl" ] && [ -f "$log" ] || { echo "skip $id: missing files" >&2; continue; }
  if ! $MP -verified -sig "$sig" -formula "$mfotl" -check 2>&1 | grep -q "is monitorable"; then
    vm_reject=$((vm_reject+1)); echo "  [vm-reject] $id" >&2; continue
  fi
  if ! "$SC" -sig "$sig" -formula "$mfotl" -prefix "$WORK/hdr" >/dev/null 2>&1; then
    sm_reject=$((sm_reject+1)); echo "  [sm-reject] $id: $(head -c120 <("$SC" -sig "$sig" -formula "$mfotl" -prefix "$WORK/hdr" 2>&1))" >&2; continue
  fi
  d="$CASES/$id"; mkdir -p "$d"
  cp "$sig" "$d/sig"; cp "$mfotl" "$d/formula"; cp "$log" "$d/trace"
  $MP -verified -sig "$sig" -formula "$mfotl" -log "$log" > "$d/expected" 2>/dev/null
  kept=$((kept+1))
done
echo "cases-frz: kept=$kept  verimon-rejected=$vm_reject  staticmon-rejected=$sm_reject"

# ---- hand-written edge cases (x_*) beyond the adopted monpoly suite ---------
# Format: name | formula  (shared sig/log below unless a case writes its own).
XSIG="$WORK/x.sig"
cat > "$XSIG" <<'EOF'
p(int)
q(int)
r(int,int)
EOF
XLOG="$WORK/x.log"
cat > "$XLOG" <<'EOF'
@0 p(1) q(1);
@5 p(2) q(1) q(2);
@5 p(3) q(3);
@10 q(1) q(2) q(3);
@10 p(1);
@11 q(1);
@25 p(2) q(2);
@26 q(2);
EOF
extras() {
  cat <<'EOF'
x_win_exact|FRZ F(x) = p(x) IN q(x) AND ONCE[0,5] F(x)
x_win_zero|FRZ F(x) = p(x) IN q(x) AND ONCE[0,0] F(x)
x_eq_ts|FRZ F(x) = p(x) IN ONCE[5,10] F(x)
x_alpha_future|FRZ F(x) = (EVENTUALLY[0,10] p(x)) IN ONCE[0,5] F(x)
x_double_lag|FRZ F(x) = (EVENTUALLY[0,5] p(x)) IN EVENTUALLY[0,5] F(x)
x_beta_until|FRZ F(x) = p(x) IN F(x) UNTIL[0,10] q(x)
x_prev|FRZ F(x) = p(x) IN q(x) AND PREV[0,10] F(x)
x_next|FRZ F(x) = p(x) IN q(x) AND NEXT[0,10] F(x)
x_triple_nest|FRZ F(x) = p(x) IN FRZ G(x) = (F(x) AND q(x)) IN FRZ H(x) = (G(x) OR p(x)) IN ONCE[0,10] H(x)
x_nest_inner_freeze|FRZ F(x) = p(x) IN ONCE[0,10] (FRZ G(x) = (F(x) OR q(x)) IN ONCE[0,10] G(x))
x_let_in_body|FRZ F(x) = p(x) IN LET U(x) = F(x) AND q(x) IN ONCE[0,10] U(x)
x_enclosing_let|LET U(x) = ONCE[0,25] q(x) IN FRZ F(x) = p(x) IN ONCE[0,10] (F(x) AND U(x))
x_shadow_sig|FRZ p(x) = q(x) IN ONCE[0,10] p(x)
x_unused|FRZ F(x) = p(x) IN ONCE[0,5] q(x)
x_current_only|FRZ F(x) = p(x) IN F(x) AND (ONCE[0,10] q(x))
x_agg_over_frozen|FRZ F(x) = p(x) IN c <- CNT x ONCE[0,10] F(x)
x_two_freezes|FRZ F(x) = p(x) IN FRZ G(x) = q(x) IN ONCE[0,10] (F(x) AND G(x))
x_since_lhs|FRZ F(x) = p(x) IN F(x) SINCE[0,10] q(x)
x_since_rhs|FRZ F(x) = p(x) IN q(x) SINCE[0,10] F(x)
x_negated_lhs|FRZ F(x) = p(x) IN (NOT F(x)) SINCE[0,10] q(x)
x_exists_body|FRZ T(x,y) = r(x,y) IN EXISTS y. ONCE[0,10] T(x,y)
x_lag_trap|FRZ P2(x) = (EVENTUALLY[0,2] p(x)) IN FRZ G(x) = q(x) IN P2(x) AND ONCE[0,3] G(x)
x_lag_control|FRZ P2(x) = (ONCE[0,2] p(x)) IN FRZ G(x) = q(x) IN P2(x) AND ONCE[0,3] G(x)
x_enclosing_future_let|LET U(x) = EVENTUALLY[0,5] q(x) IN FRZ F(x) = p(x) IN ONCE[0,3] (F(x) AND U(x))
x_win_edge15|FRZ F(x) = p(x) IN q(x) AND ONCE[0,15] F(x)
EOF
}
xkept=0
while IFS='|' read -r name f; do
  [ -z "$name" ] && continue
  printf '%s\n' "$f" > "$WORK/x.mfotl"
  if ! $MP -verified -sig "$XSIG" -formula "$WORK/x.mfotl" -check 2>&1 | grep -q "is monitorable"; then
    echo "  [x-vm-reject] $name" >&2; continue
  fi
  if ! "$SC" -sig "$XSIG" -formula "$WORK/x.mfotl" -prefix "$WORK/hdr" >/dev/null 2>&1; then
    echo "  [x-sm-reject] $name" >&2; continue
  fi
  d="$CASES/$name"; mkdir -p "$d"
  cp "$XSIG" "$d/sig"; printf '%s\n' "$f" > "$d/formula"; cp "$XLOG" "$d/trace"
  $MP -verified -sig "$d/sig" -formula "$d/formula" -log "$d/trace" > "$d/expected" 2>/dev/null
  xkept=$((xkept+1))
done < <(extras)
# a 0-ary frozen predicate (own sig: needs a 0-ary trace predicate too)
d="$CASES/x_nullary"; mkdir -p "$d"
printf 'p(int)\ne()\n' > "$d/sig"
printf 'FRZ B() = (EXISTS x. p(x)) IN p(y) AND ONCE[0,10] B()\n' > "$d/formula"
printf '@0 p(1);\n@5 e();\n@8 p(2);\n@30 p(3) e();\n' > "$d/trace"
if $MP -verified -sig "$d/sig" -formula "$d/formula" -check 2>&1 | grep -q "is monitorable" \
   && "$SC" -sig "$d/sig" -formula "$d/formula" -prefix "$WORK/hdr" >/dev/null 2>&1; then
  $MP -verified -sig "$d/sig" -formula "$d/formula" -log "$d/trace" > "$d/expected" 2>/dev/null
  xkept=$((xkept+1))
else
  echo "  [x-reject] x_nullary" >&2; rm -rf "$d"
fi
echo "cases-frz extras: kept=$xkept"
