#!/usr/bin/env bash
# Regenerate the stored monpoly_suite fixtures from the monpoly-develop corpus.
#
# OFFLINE maintainer tool -- it needs monpoly (VeriMon) and the corpus; nothing
# in the ctest flow runs it. It extracts (sig, formula, trace) cases, keeps the
# ones both VeriMon and staticmon accept, and writes
#   monitor/methods/fixture/cases-corpus/<id>/{sig, formula, trace, expected}
# where `expected` is VeriMon's verdict stream. run_suite.sh then replays these
# with no monpoly / corpus dependency.
#
# Usage: regen.sh <staticmon-headers> [monpoly_tests_dir]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
SC=${1:?path to staticmon-headers binary}
TESTS=${2:-$HOME/Data/Projects/monpoly-develop/tests}
MP=${MONPOLY:-$(command -v monpoly 2>/dev/null || ls "$HOME"/.opam/*/bin/monpoly 2>/dev/null | head -1)}
CASES="$here/cases-corpus"

{ [ -n "$MP" ] && [ -x "$MP" ]; } || { echo "monpoly not found (set \$MONPOLY)" >&2; exit 1; }
[ -d "$TESTS" ] || { echo "corpus not found: $TESTS (arg 2)" >&2; exit 1; }
[ -x "$SC" ]    || { echo "staticmon-headers not found: $SC" >&2; exit 1; }

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/hdr"
python3 "$here/extract_tests.py" "$TESTS" "$WORK/stage" > "$WORK/manifest.tsv" 2>/dev/null

rm -rf "$CASES"; mkdir -p "$CASES"
kept=0 vm_reject=0 sm_reject=0
while IFS=$'\t' read -r id sig log mfotl; do
  # keep only cases VeriMon accepts and staticmon can fully compile. Use the
  # full front-end (-prefix generates headers = parse+typecheck+monitorability+
  # translate+codegen), not just -check: some formulas pass -check but fail
  # translation (e.g. aggregation result fed through i2f/f2i into a new var).
  $MP -verified -sig "$sig" -formula "$mfotl" -check 2>&1 | grep -q "is monitorable" \
    || { vm_reject=$((vm_reject+1)); continue; }
  "$SC" -sig "$sig" -formula "$mfotl" -prefix "$WORK/hdr" >/dev/null 2>&1 \
    || { sm_reject=$((sm_reject+1)); continue; }
  d="$CASES/$id"; mkdir -p "$d"
  cp "$sig" "$d/sig"; cp "$mfotl" "$d/formula"; cp "$log" "$d/trace"
  $MP -verified -sig "$sig" -formula "$mfotl" -log "$log" > "$d/expected" 2>/dev/null
  kept=$((kept+1))
done < "$WORK/manifest.tsv"

echo "wrote $kept cases to $CASES" >&2
echo "  (skipped: VeriMon-rejected=$vm_reject, staticmon-rejected=$sm_reject)" >&2
