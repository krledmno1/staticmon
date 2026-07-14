#!/usr/bin/env bash
# Regenerate the pipeline_diff fixtures: for a fixed set of generated formulas,
# store the free-variable types (-sigout, classified) and monitorability verdict
# (-check: OK/NOT/ERR) that monpoly and staticmon AGREE on. OFFLINE maintainer
# tool -- needs monpoly. Writes test/pipeline_diff/{sig, expected.tsv}.
#
# The documented rr completeness gap (monpoly OK via rewriting, staticmon NOT) is
# not stored (recorded to stderr). Any *other* disagreement is a real bug and is
# reported as DROPPED (should be zero).
#
# Usage: regen.sh <staticmon-headers> [n_random] [seed]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
SC=${1:?path to staticmon-headers binary}
N=${2:-400}; SEED=${3:-7}
MP=${MONPOLY:-$(command -v monpoly 2>/dev/null || ls "$HOME"/.opam/*/bin/monpoly 2>/dev/null | head -1)}
{ [ -n "$MP" ] && [ -x "$MP" ]; } || { echo "monpoly not found (set \$MONPOLY)" >&2; exit 1; }
[ -x "$SC" ] || { echo "staticmon-headers not found: $SC" >&2; exit 1; }

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
SIG=$(python3 "$here/gen_formulas.py" "$N" "$SEED" 2>&1 >"$WORK/formulas.txt")
printf '%s\n' "$SIG" > "$here/sig"

sig_cls() { if grep -q "Fatal error" <<<"$1"; then echo "ERR"; else head -1 <<<"$1"; fi; }
cls() { if grep -q "NOT monitorable" <<<"$1"; then echo NOT
        elif grep -q "is monitorable" <<<"$1"; then echo OK; else echo ERR; fi; }

: > "$here/expected.tsv"
kept=0 rr_gap=0 dropped=0
while IFS= read -r formula; do
  [ -z "$formula" ] && continue
  printf '%s' "$formula" > "$WORK/f.mfotl"
  mp_sig=$(sig_cls "$("$MP" -sig "$here/sig" -formula "$WORK/f.mfotl" -sigout 2>&1)")
  sc_sig=$(sig_cls "$("$SC" -sig "$here/sig" -formula "$WORK/f.mfotl" -sigout 2>&1)")
  mp_mon=$(cls "$("$MP" -sig "$here/sig" -formula "$WORK/f.mfotl" -check 2>&1)")
  sc_mon=$(cls "$("$SC" -sig "$here/sig" -formula "$WORK/f.mfotl" -check 2>&1)")
  if [ "$mp_sig" = "$sc_sig" ] && [ "$mp_mon" = "$sc_mon" ]; then
    printf '%s\t%s\t%s\n' "$formula" "$mp_sig" "$mp_mon" >> "$here/expected.tsv"
    kept=$((kept+1))
  elif [ "$mp_sig" = "$sc_sig" ] && [ "$mp_mon" = "OK" ] && [ "$sc_mon" = "NOT" ]; then
    rr_gap=$((rr_gap+1))   # documented rr completeness gap; not stored
  else
    dropped=$((dropped+1))
    echo "DROPPED real disagreement: $formula [mp_sig=$mp_sig sc_sig=$sc_sig mp_mon=$mp_mon sc_mon=$sc_mon]" >&2
  fi
done < "$WORK/formulas.txt"

echo "wrote $kept cases to expected.tsv (rr_gap=$rr_gap not stored; real_disagreements=$dropped)" >&2
