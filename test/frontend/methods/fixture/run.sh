#!/usr/bin/env bash
# Replay the stored typing/monitorability fixtures against staticmon-headers --
# no monpoly at runtime. For each formula in expected.tsv, check staticmon's
# -sigout (free-variable types, classified) and -check (OK/NOT/ERR) match the
# stored, monpoly-agreed values. Fixtures regenerated offline by regen.sh.
#
# Usage: run_check_diff.sh <staticmon-headers>
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
SC=${1:?path to staticmon-headers}
{ [ -f "$here/expected.tsv" ] && [ -f "$here/sig" ]; } \
  || { echo "no fixtures (run regen.sh); skipping" >&2; exit 77; }
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

sig_cls() { if grep -q "Fatal error" <<<"$1"; then echo "ERR"; else head -1 <<<"$1"; fi; }
cls() { if grep -q "NOT monitorable" <<<"$1"; then echo NOT
        elif grep -q "is monitorable" <<<"$1"; then echo OK; else echo ERR; fi; }

total=0 sig_bad=0 mon_bad=0
# Split on tab manually: `IFS=$'\t' read` collapses consecutive tabs (tab is
# IFS-whitespace), which would eat the empty sig field of free-variable-less
# formulas like `p(5)` (stored as `p(5)\t\tOK`).
while IFS= read -r line; do
  [ -z "$line" ] && continue
  formula=${line%%$'\t'*}; rest=${line#*$'\t'}
  exp_sig=${rest%%$'\t'*}; exp_mon=${rest#*$'\t'}
  total=$((total+1))
  printf '%s' "$formula" > "$WORK/f.mfotl"
  sc_sig=$(sig_cls "$("$SC" -sig "$here/sig" -formula "$WORK/f.mfotl" -sigout 2>&1)")
  sc_mon=$(cls "$("$SC" -sig "$here/sig" -formula "$WORK/f.mfotl" -check 2>&1)")
  if [ "$sc_sig" != "$exp_sig" ]; then
    sig_bad=$((sig_bad+1))
    [ $sig_bad -le 15 ] && { echo "SIG MISMATCH: $formula"; echo "  expected: $exp_sig"; echo "  got:      $sc_sig"; }
  fi
  if [ "$sc_mon" != "$exp_mon" ]; then
    mon_bad=$((mon_bad+1))
    [ $mon_bad -le 15 ] && echo "MON MISMATCH: $formula (expected $exp_mon, got $sc_mon)"
  fi
done < "$here/expected.tsv"


echo
echo "cases=$total  sigout_bad=$sig_bad  monitorability_bad=$mon_bad"
[ "$sig_bad" -eq 0 ] && [ "$mon_bad" -eq 0 ]
