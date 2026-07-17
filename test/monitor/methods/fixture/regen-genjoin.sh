#!/usr/bin/env bash
# Regenerate the conjunction-cluster fixtures (cases-genjoin) for the generic
# multiway-join work (docs/LFTJ-STATICMON.md): every cluster shape the
# flatten_and_chains pass may rewrite into mgenjoin, with expected verdicts
# from VeriMon -- whose formally verified generic join (convert_multiway +
# mmulti_join') evaluates every one of these clusters, making this suite a
# conformance test against the machine-checked specification.
#
# OFFLINE maintainer tool -- needs monpoly (VeriMon); nothing in the ctest
# flow runs it. Writes monitor/methods/fixture/cases-genjoin/<id>/{sig,
# formula, trace, expected}.
#
# Usage: regen-genjoin.sh <staticmon-headers>
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
SC=${1:?path to staticmon-headers binary}
MP=${MONPOLY:-$(command -v monpoly 2>/dev/null || ls "$HOME"/.opam/*/bin/monpoly 2>/dev/null | head -1)}
CASES="$here/cases-genjoin"

{ [ -n "$MP" ] && [ -x "$MP" ]; } || { echo "monpoly not found (set \$MONPOLY)" >&2; exit 1; }
[ -x "$SC" ] || { echo "staticmon-headers not found: $SC" >&2; exit 1; }

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/hdr"

SIG="$WORK/sig"
cat > "$SIG" <<'EOF'
p(int,int)
q(int,int)
s(int,int)
u(int)
v(int)
e()
b(int,string)
EOF
# The trace phases: dense tp 0-2 (all preds), one tp with p only, one with q
# only, an EMPTY tp, a tp reintroducing everything with new values, and a
# sparse tail -- so clusters see empty children, empty intersections, and
# re-population. Values overlap across predicates to make joins nonempty.
LOG="$WORK/log"
cat > "$LOG" <<'EOF'
@0 p(1,2) p(2,3) p(3,1) q(2,3) q(3,1) q(1,2) s(1,3) s(2,1) s(3,2) u(1) u(2) v(2) v(3) e() b(1,"a") b(2,"b");
@1 p(1,2) p(4,5) q(2,4) q(5,6) s(1,4) u(4) v(4) b(4,"a");
@2 p(2,2) q(2,2) s(2,2) u(2) v(2) e();
@3 p(7,8) p(8,9);
@4 q(7,8) q(8,9);
@5 ;
@6 p(1,2) q(2,3) s(1,3) u(1) v(3) e() b(1,"c");
@8 u(9) v(9);
@9 p(9,9) q(9,9) s(9,9) u(9) v(9) e();
EOF

cases() {
  cat <<'EOF'
j2_allshared|p(x,y) AND q(x,y)
j2_oneshared|p(x,y) AND q(y,z)
j2_cartesian|u(x) AND v(y)
j3_chain|(p(x,y) AND q(y,z)) AND s(z,w)
j3_star|(u(x) AND p(x,y)) AND q(x,z)
j3_triangle|(p(a,b) AND q(b,c)) AND s(a,c)
j4_clique|((p(a,b) AND q(b,c)) AND s(a,c)) AND p(a,c)
j5_wide|(((p(a,b) AND q(b,c)) AND s(c,d)) AND u(a)) AND v(d)
jneg_covered1|(p(x,y) AND q(y,z)) AND (NOT s(x,y))
jneg_needs_union|(p(x,y) AND q(y,z)) AND (NOT s(x,z))
jneg_two|((p(x,y) AND q(y,z)) AND (NOT s(x,z))) AND (NOT s(y,y))
jneg_eq_positive|(p(x,y) AND q(x,y)) AND (NOT p(x,y))
jneg_unary|(u(x) AND p(x,y)) AND (NOT v(y))
jeq_const|(p(x,y) AND q(y,z)) AND x = 1
jeq_vars|(p(x,y) AND q(y,z)) AND x = z
jassign|(p(x,y) AND q(y,z)) AND w = x + z
jarity0|(e() AND u(x)) AND p(x,y)
jself|p(x,y) AND p(y,x)
jdup_atom|(p(x,y) AND q(y,z)) AND p(x,y)
jstr|(b(x,c) AND u(x)) AND (NOT v(x))
jtemporal_children|((ONCE[0,5] p(t1,x)) AND (ONCE[0,5] q(t2,x))) AND (NOT s(t1,t2))
jcluster_under_once|ONCE[0,4] ((p(x,y) AND q(y,z)) AND (NOT s(x,z)))
jcluster_under_since|(u(x) AND v(y)) SINCE[0,6] ((p(x,z) AND q(z,y)) AND (NOT s(x,y)))
jlet_atoms|LET pq(x,y,z) = p(x,y) AND q(y,z) IN (pq(x,y,z) AND s(x,z)) AND (NOT u(x))
jfrz_cluster|FRZ F(t1,x) = p(t1,x) IN ((ONCE[0,6] F(t1,x)) AND (ONCE[0,6] q(t2,x))) AND (NOT s(t1,t2))
jneg_binary_only|p(x,y) AND (NOT q(x,y))
janti_chain|(p(x,y) AND (NOT q(x,y))) AND (NOT s(y,x))
EOF
}

rm -rf "$CASES"; mkdir -p "$CASES"
kept=0
while IFS='|' read -r name f; do
  [ -z "$name" ] && continue
  printf '%s\n' "$f" > "$WORK/f.mfotl"
  if ! $MP -verified -sig "$SIG" -formula "$WORK/f.mfotl" -check 2>&1 | grep -q "is monitorable"; then
    echo "  [vm-reject] $name" >&2; continue
  fi
  if ! "$SC" -sig "$SIG" -formula "$WORK/f.mfotl" -prefix "$WORK/hdr" >/dev/null 2>&1; then
    echo "  [sm-reject] $name" >&2; continue
  fi
  d="$CASES/$name"; mkdir -p "$d"
  cp "$SIG" "$d/sig"; printf '%s\n' "$f" > "$d/formula"; cp "$LOG" "$d/trace"
  $MP -verified -sig "$d/sig" -formula "$d/formula" -log "$d/trace" > "$d/expected" 2>/dev/null
  kept=$((kept+1))
done < <(cases)
echo "cases-genjoin: kept=$kept"
