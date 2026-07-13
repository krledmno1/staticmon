#!/usr/bin/env bash
# Header-diff test (pipeline stages 4-5): staticmon_compile vs
# `monpoly-exp -no_rw -explicitmon` on monitorable formulas.
#
# Both sides skip rewriting (staticmon does only elim_syntactic_sugar; monpoly
# runs -no_rw), so the comparison isolates translation + codegen. monpoly-exp
# aborts on non-monitorable input (no headers written); those formulas are
# skipped here and belong to the stage-3 monitorability tests.
#
# Usage: run_header_diff.sh <staticmon_compile> [n_random] [seed]
set -uo pipefail

cd "$(dirname "$0")"
SC=${1:?path to staticmon_compile binary}
N=${2:-2000}
SEED=${3:-7}
EXP=${MONPOLY_EXP:-/Users/krle/Data/Projects/staticmon/monpoly-exp/_build/default/src/main.exe}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

SIG=$(python3 gen_formulas.py "$N" "$SEED" 2>&1 >"$WORK/formulas.txt")
echo "$SIG" > "$WORK/s.sig"

# Both header texts are template/brace soup where whitespace is never
# semantically significant (string literals aside, which are transformed
# identically on both sides), so compare with all whitespace removed. This
# ignores OCaml Format's line-wrapping (e.g. `tvar<\n  2>` vs `tvar<2>`).
norm() { tr -d ' \n\t\r' < "$1"; }

total=0 compared=0 skipped=0 mismatch=0
while IFS= read -r formula; do
  [ -z "$formula" ] && continue
  total=$((total+1))
  printf '%s' "$formula" > "$WORK/f.mfotl"
  rm -f "$WORK/formula_in.h" "$WORK/formula_csts.h"
  # Reference (monpoly-exp). Aborts (nonzero) if not monitorable.
  if ! "$EXP" -sig "$WORK/s.sig" -formula "$WORK/f.mfotl" -no_rw \
        -explicitmon -explicitmon_prefix "$WORK" >/dev/null 2>&1; then
    skipped=$((skipped+1)); continue
  fi
  [ -f "$WORK/formula_in.h" ] || { skipped=$((skipped+1)); continue; }
  cat "$WORK/formula_in.h" > "$WORK/ref_in.txt"
  cat "$WORK/formula_csts.h" >> "$WORK/ref_in.txt" 2>/dev/null
  # Candidate (staticmon).
  if ! "$SC" -sig "$WORK/s.sig" -formula "$WORK/f.mfotl" \
        > "$WORK/cand_raw.txt" 2>"$WORK/cand_err.txt"; then
    mismatch=$((mismatch+1))
    if [ $mismatch -le 25 ]; then
      echo "=== MISMATCH #$mismatch (staticmon rejected an accepted formula) ==="
      echo "  formula: $formula"
      echo "  error:   $(cat "$WORK/cand_err.txt")"
    fi
    continue
  fi
  # strip the //---CSTS--- separator; concatenate in/csts
  sed 's#//---CSTS---##' "$WORK/cand_raw.txt" > "$WORK/cand.txt"
  compared=$((compared+1))
  if [ "$(norm "$WORK/ref_in.txt")" != "$(norm "$WORK/cand.txt")" ]; then
    mismatch=$((mismatch+1))
    if [ $mismatch -le 25 ]; then
      echo "=== MISMATCH #$mismatch ==="
      echo "  formula: $formula"
      echo "  ref:  $(norm "$WORK/ref_in.txt")"
      echo "  cand: $(norm "$WORK/cand.txt")"
    fi
  fi
done < "$WORK/formulas.txt"

echo
echo "total=$total compared=$compared skipped_nonmonitorable=$skipped mismatch=$mismatch"
[ "$mismatch" -eq 0 ]
