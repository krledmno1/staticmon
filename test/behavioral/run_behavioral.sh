#!/usr/bin/env bash
# Behavioral end-to-end oracle: staticmon (compiled per formula) vs VeriMon
# (monpoly -verified) on small random traces.
#
# Pipeline per formula: staticmon-headers generates headers -> a per-formula
# monitor is compiled inside a warm native container (staticmon-bench) -> run
# on a trace; monpoly -verified runs the same formula+trace on the host; the
# verdict streams are compared semantically.
#
# Optimizations: binaries are cached by formula hash (unique formulas compiled
# once, persisted across runs in CACHE); cache hits skip compilation entirely;
# conan deps + a warm build tree live in the prebuilt image; traces are capped
# at <=100 timepoints (VeriMon is slow).
#
# Usage: run_behavioral.sh <staticmon-headers> [n_random] [seed] [traces_per_formula]
# Requires: docker image staticmon-bench + running container `smbench`; native
# monpoly on PATH via $MONPOLY.
set -uo pipefail
cd "$(dirname "$0")"
SC=${1:?path to staticmon-headers}
N=${2:-40}
SEED=${3:-11}
TRACES=${4:-3}
NTP=${5:-40}
MP=${MONPOLY:-/Users/krle/.opam/4.14.2/bin/monpoly}
CTR=${CONTAINER:-smbench}
CACHE=${CACHE:-$HOME/.cache/staticmon-bench-bin}
# cwd is already this script's dir (test/behavioral) from the cd above.
BUILD=${BUILD:-"$(cd ../../scripts && pwd)/staticmon-build"}
mkdir -p "$CACHE"
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

command -v "$MP" >/dev/null || { echo "monpoly not found: $MP"; exit 2; }
docker exec "$CTR" true 2>/dev/null || { echo "container $CTR not running"; exit 2; }

SIG=$(python3 gen_bench.py "$N" "$SEED" 2>&1 >"$WORK/formulas.txt")
SIG=${SIG#SIG }
echo "$SIG" > "$WORK/s.sig"

INDIR=/opt/staticmon/src/staticmon/input_formula
total=0 skipped=0 compiled=0 cache_hits=0 match=0 mism=0

while IFS= read -r formula; do
  [ -z "$formula" ] && continue
  total=$((total+1))
  printf '%s' "$formula" > "$WORK/f.mfotl"

  # VeriMon must accept the formula (it is the oracle); skip otherwise.
  if ! $MP -verified -sig "$WORK/s.sig" -formula "$WORK/f.mfotl" -check \
        2>&1 | grep -q "is monitorable"; then
    skipped=$((skipped+1)); continue
  fi

  # staticmon-build gates staticmon's monitorability/typing and compiles the
  # monitor with binary caching (keyed by formula hash). exit 1 = staticmon
  # rejects; exit 2 = build error.
  if ! "$BUILD" -sig "$WORK/s.sig" -formula "$WORK/f.mfotl" \
        --container "$CTR" --staticmon-compile "$SC" --cache-dir "$CACHE" \
        -o "$WORK/mon" >"$WORK/build.log" 2>&1; then
    skipped=$((skipped+1)); continue
  fi
  if grep -q "cache hit" "$WORK/build.log"; then
    cache_hits=$((cache_hits+1)); else compiled=$((compiled+1)); fi
  binpath=/tmp/mon
  docker cp "$WORK/mon" "$CTR:$binpath" >/dev/null

  formula_bad=0
  for ((t=0; t<TRACES; t++)); do
    python3 gen_trace.py "$SIG" "$NTP" "$((SEED*100 + t))" > "$WORK/t.log"
    docker cp "$WORK/t.log" "$CTR:/t.log" >/dev/null
    docker exec "$CTR" "$binpath" --log /t.log > "$WORK/sm.out" 2>/dev/null
    $MP -verified -sig "$WORK/s.sig" -formula "$WORK/f.mfotl" -log "$WORK/t.log" \
      > "$WORK/mp.out" 2>/dev/null
    if ! python3 compare_verdicts.py "$WORK/sm.out" "$WORK/mp.out" \
          > "$WORK/diff.txt" 2>&1; then
      formula_bad=1
      if [ $mism -le 20 ]; then
        echo "=== VERDICT MISMATCH: $formula (trace seed $((SEED*100+t))) ==="
        cat "$WORK/diff.txt"
      fi
      break
    fi
  done
  if [ $formula_bad -eq 1 ]; then mism=$((mism+1)); else match=$((match+1)); fi
done < "$WORK/formulas.txt"

echo
echo "total=$total skipped=$skipped compiled=$compiled cache_hits=$cache_hits"
echo "verdicts: match=$match mismatch=$mism"
[ "$mism" -eq 0 ]
