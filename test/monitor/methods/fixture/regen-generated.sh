#!/usr/bin/env bash
# Regenerate the behavioral fixtures: a *frozen* set of generated (sig, formula,
# trace) triples that both VeriMon and staticmon accept, with VeriMon's verdict
# stream stored. OFFLINE maintainer tool -- needs monpoly; nothing in the ctest
# flow runs it. Writes test/behavioral/cases/<id>/{sig, formula, trace, expected}.
#
# Usage: regen.sh <staticmon-headers> [n_formulas] [seed] [traces_per_formula] [ntp]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
SC=${1:?path to staticmon-headers binary}
N=${2:-100}; SEED=${3:-11}; TRACES=${4:-2}; NTP=${5:-40}
MP=${MONPOLY:-$(command -v monpoly 2>/dev/null || ls "$HOME"/.opam/*/bin/monpoly 2>/dev/null | head -1)}
CASES="$here/cases-generated"

{ [ -n "$MP" ] && [ -x "$MP" ]; } || { echo "monpoly not found (set \$MONPOLY)" >&2; exit 1; }
[ -x "$SC" ] || { echo "staticmon-headers not found: $SC" >&2; exit 1; }

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/hdr"
SIG=$(python3 "$here/../../components/generator/gen_bench.py" "$N" "$SEED" 2>&1 >"$WORK/formulas.txt"); SIG=${SIG#SIG }
echo "$SIG" > "$WORK/s.sig"

rm -rf "$CASES"; mkdir -p "$CASES"
kept=0 rejected=0 fidx=0
while IFS= read -r formula; do
  [ -z "$formula" ] && continue
  fidx=$((fidx+1))
  printf '%s' "$formula" > "$WORK/f.mfotl"
  # keep only formulas VeriMon accepts and staticmon can fully compile (-prefix
  # runs the whole front-end incl. translation, unlike -check).
  $MP -verified -sig "$WORK/s.sig" -formula "$WORK/f.mfotl" -check 2>&1 | grep -q "is monitorable" \
    || { rejected=$((rejected+1)); continue; }
  "$SC" -sig "$WORK/s.sig" -formula "$WORK/f.mfotl" -prefix "$WORK/hdr" >/dev/null 2>&1 \
    || { rejected=$((rejected+1)); continue; }
  for ((t=0; t<TRACES; t++)); do
    python3 "$here/../../components/generator/gen_trace.py" "$SIG" "$NTP" "$((SEED*100 + t))" > "$WORK/t.log"
    d="$CASES/$(printf 'f%03d_t%d' "$fidx" "$t")"; mkdir -p "$d"
    cp "$WORK/s.sig" "$d/sig"; cp "$WORK/f.mfotl" "$d/formula"; cp "$WORK/t.log" "$d/trace"
    $MP -verified -sig "$WORK/s.sig" -formula "$WORK/f.mfotl" -log "$WORK/t.log" > "$d/expected" 2>/dev/null
    kept=$((kept+1))
  done
done < "$WORK/formulas.txt"

echo "wrote $kept behavioral cases to $CASES ($rejected formulas rejected by an oracle)" >&2
