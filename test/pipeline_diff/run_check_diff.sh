#!/usr/bin/env bash
# Stage 2-3 differential test: staticmon-headers -sigout / -check vs the native
# monpoly (monpoly-develop) oracle, over the monitorable-leaning corpus (which
# also contains non-monitorable and some ill-typed formulas).
#
#   -sigout : free-variable types (or a type-error).  Must match exactly.
#   -check  : monitorability verdict (OK / NOT / ERR). Compared by class.
#
# Note: monpoly -check applies full rewriting (rr) before is_monitorable;
# staticmon only desugars. So monpoly may accept some formulas staticmon
# reports NOT monitorable (a documented completeness gap, never a soundness
# violation). These are counted separately as `mon_rr_gap`.
#
# Usage: run_check_diff.sh <staticmon-headers> [n_random] [seed]
set -uo pipefail
cd "$(dirname "$0")"
SC=${1:?path to staticmon-headers}
N=${2:-1000}
SEED=${3:-7}
MP=${MONPOLY:-/Users/krle/.opam/4.14.2/bin/monpoly}
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

SIG=$(python3 gen_formulas.py "$N" "$SEED" 2>&1 >"$WORK/formulas.txt")
echo "$SIG" > "$WORK/s.sig"

# classify -check output into OK / NOT / ERR
cls() { if grep -q "NOT monitorable" <<<"$1"; then echo NOT
        elif grep -q "is monitorable" <<<"$1"; then echo OK
        else echo ERR; fi; }
# classify -sigout output: the type line, or ERR on any Fatal error
# (type clash or check_wff rejection — both abort before types are printed).
sig_cls() { if grep -q "Fatal error" <<<"$1"; then echo "ERR"
            else head -1 <<<"$1"; fi; }

total=0 sig_ok=0 sig_bad=0 mon_ok=0 mon_bad=0 rr_gap=0
while IFS= read -r formula; do
  [ -z "$formula" ] && continue
  total=$((total+1))
  printf '%s' "$formula" > "$WORK/f.mfotl"

  mp_sig=$(sig_cls "$("$MP" -sig "$WORK/s.sig" -formula "$WORK/f.mfotl" -sigout 2>&1)")
  sc_sig=$(sig_cls "$("$SC" -sig "$WORK/s.sig" -formula "$WORK/f.mfotl" -sigout 2>&1)")
  if [ "$mp_sig" = "$sc_sig" ]; then sig_ok=$((sig_ok+1)); else
    sig_bad=$((sig_bad+1))
    if [ $sig_bad -le 15 ]; then
      echo "SIG MISMATCH: $formula"; echo "  mp: $mp_sig"; echo "  sc: $sc_sig"
    fi
  fi

  mp_mon=$(cls "$("$MP" -sig "$WORK/s.sig" -formula "$WORK/f.mfotl" -check 2>&1)")
  sc_mon=$(cls "$("$SC" -sig "$WORK/s.sig" -formula "$WORK/f.mfotl" -check 2>&1)")
  if [ "$mp_mon" = "$sc_mon" ]; then mon_ok=$((mon_ok+1))
  elif [ "$mp_mon" = "OK" ] && [ "$sc_mon" = "NOT" ]; then
    rr_gap=$((rr_gap+1))
    if [ $rr_gap -le 15 ]; then echo "RR-GAP (mp OK, sc NOT): $formula"; fi
  else
    mon_bad=$((mon_bad+1))
    if [ $mon_bad -le 15 ]; then
      echo "MON MISMATCH: $formula"; echo "  mp: $mp_mon  sc: $sc_mon"
    fi
  fi
done < "$WORK/formulas.txt"

echo
echo "total=$total  sigout: ok=$sig_ok bad=$sig_bad  monitorability: ok=$mon_ok bad=$mon_bad rr_gap=$rr_gap"
[ "$sig_bad" -eq 0 ] && [ "$mon_bad" -eq 0 ]
