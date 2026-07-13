#!/usr/bin/env bash
# Behavioral end-to-end oracle: staticmon (compiled per formula) vs VeriMon
# (monpoly -verified) on small random traces.
#
# Pipeline per formula: staticmon_compile generates headers -> a per-formula
# monitor is compiled inside a warm native container (staticmon-bench) -> run
# on a trace; monpoly -verified runs the same formula+trace on the host; the
# verdict streams are compared semantically.
#
# Optimizations: binaries are cached by formula hash (unique formulas compiled
# once, persisted across runs in CACHE); cache hits skip compilation entirely;
# conan deps + a warm build tree live in the prebuilt image; traces are capped
# at <=100 timepoints (VeriMon is slow).
#
# Usage: run_behavioral.sh <staticmon_compile> [n_random] [seed] [traces_per_formula]
# Requires: docker image staticmon-bench + running container `smbench`; native
# monpoly on PATH via $MONPOLY.
set -uo pipefail
cd "$(dirname "$0")"
SC=${1:?path to staticmon_compile}
N=${2:-40}
SEED=${3:-11}
TRACES=${4:-3}
NTP=${5:-40}
MP=${MONPOLY:-/Users/krle/.opam/4.14.2/bin/monpoly}
CTR=${CONTAINER:-smbench}
CACHE=${CACHE:-$HOME/.cache/staticmon-bench-bin}
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

  # staticmon front-end: generate headers (also gates monitorability/typing)
  if ! "$SC" -sig "$WORK/s.sig" -formula "$WORK/f.mfotl" -prefix "$WORK" \
        >/dev/null 2>"$WORK/sc_err.txt"; then
    skipped=$((skipped+1)); continue
  fi
  # VeriMon must accept it too (it is the oracle); skip otherwise
  if ! $MP -verified -sig "$WORK/s.sig" -formula "$WORK/f.mfotl" -check \
        2>&1 | grep -q "is monitorable"; then
    skipped=$((skipped+1)); continue
  fi

  hash=$(shasum "$WORK/formula_in.h" "$WORK/formula_csts.h" | shasum | cut -c1-16)
  binpath=/tmp/mon_$hash
  if [ -f "$CACHE/$hash" ]; then
    cache_hits=$((cache_hits+1))
    docker cp "$CACHE/$hash" "$CTR:$binpath" >/dev/null
  else
    docker cp "$WORK/formula_in.h" "$CTR:$INDIR/formula_in.h" >/dev/null
    docker cp "$WORK/formula_csts.h" "$CTR:$INDIR/formula_csts.h" >/dev/null
    # docker cp preserves the host mtime; Docker Desktop's VM clock can be
    # ahead of the macOS host, so the injected header looks OLDER than the .o
    # and ninja skips the rebuild (stale binary). touch inside the container
    # to force a rebuild.
    if ! docker exec "$CTR" bash -c \
          "touch $INDIR/formula_in.h $INDIR/formula_csts.h && ninja -C builddir" \
          >"$WORK/ninja.log" 2>&1; then
      echo "COMPILE FAILED: $formula"; skipped=$((skipped+1)); continue
    fi
    docker cp "$CTR:/opt/staticmon/builddir/bin/staticmon" "$CACHE/$hash" >/dev/null
    docker cp "$CACHE/$hash" "$CTR:$binpath" >/dev/null
    compiled=$((compiled+1))
  fi

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
