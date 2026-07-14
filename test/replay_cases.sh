#!/usr/bin/env bash
# Shared fixture replay (used by behavioral + monpoly_suite). For each
# <cases_dir>/<id>/{sig, formula, trace, expected}: compile the staticmon
# monitor for (sig, formula) and check its verdicts on `trace` match the stored
# `expected` (VeriMon's output). No monpoly / external corpus at runtime.
#
# Usage: replay_cases.sh <cases_dir> <staticmon-headers> [cache_dir]
# Env: CONTAINER (default smbench) -- the container the monitor is compiled in.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
CASES=${1:?cases dir}
SC=${2:?path to staticmon-headers}
CACHE=${3:-$HOME/.cache/staticmon-replay}
CTR=${CONTAINER:-smbench}
IMPL="$here/../scripts/staticmon-impl"
CMP="$here/behavioral/compare_verdicts.py"
mkdir -p "$CACHE"
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

command -v python3 >/dev/null 2>&1 || { echo "python3 not found; skipping" >&2; exit 77; }
{ [ -d "$CASES" ] && [ -n "$(ls -A "$CASES" 2>/dev/null)" ]; } \
  || { echo "no fixtures in $CASES (run regen.sh); skipping" >&2; exit 77; }
docker exec "$CTR" true 2>/dev/null || { echo "container $CTR not running; skipping" >&2; exit 77; }

total=0 build_err=0 match=0 mism=0
for d in "$CASES"/*/; do
  [ -f "$d/sig" ] || continue
  id=$(basename "$d"); total=$((total+1))
  # Any non-zero exit is a regression: this case compiled + matched when the
  # fixtures were generated. (1 = staticmon now rejects; 2 = build error.)
  STATICMON_HEADERS="$SC" "$IMPL" compile -sig "$d/sig" -formula "$d/formula" \
        -container "$CTR" -cache "$CACHE" -keep "$WORK/mon" >"$WORK/b.log" 2>&1
  if [ $? -ne 0 ]; then
    build_err=$((build_err+1))
    [ $build_err -le 12 ] && { echo "BUILD ERR $id: $(head -1 "$d/formula")"; grep -m1 'error:' "$WORK/b.log"; }
    continue
  fi
  docker cp "$WORK/mon" "$CTR:/tmp/mon" >/dev/null
  docker cp "$d/trace" "$CTR:/replay.log" >/dev/null
  docker exec "$CTR" /tmp/mon --log /replay.log > "$WORK/sm.out" 2>/dev/null
  if python3 "$CMP" "$WORK/sm.out" "$d/expected" >"$WORK/diff.txt" 2>&1; then
    match=$((match+1))
  else
    mism=$((mism+1))
    [ $mism -le 20 ] && { echo "=== MISMATCH $id: $(head -1 "$d/formula") ==="; head -6 "$WORK/diff.txt"; }
  fi
done

echo
echo "cases=$total  match=$match  mismatch=$mism  build_error=$build_err"
[ "$mism" -eq 0 ] && [ "$build_err" -eq 0 ]
