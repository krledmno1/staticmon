#!/usr/bin/env bash
# Shared fixture replay (used by both monitor/fixture case-sets). For each
# <cases_dir>/<id>/{sig, formula, trace, expected}: compile the staticmon monitor
# for (sig, formula) and check its verdicts on `trace` match the stored `expected`
# (VeriMon's output). No monpoly / external corpus at runtime.
#
# Compile/run mode is resolved NATIVE-first, docker fallback:
#   * native  -- staticmon-impl compiles against the configured builddir and the
#                monitor runs directly. Chosen when the native build is configured.
#   * docker  -- staticmon-impl compiles inside the $CONTAINER (default smbench)
#                and the monitor runs there (docker cp/exec). Fallback.
# Override with STATICMON_TEST_MODE=native|docker|auto (default auto).
#
# Usage: replay_cases.sh <cases_dir> <staticmon-headers> [cache_dir]
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
CASES=${1:?cases dir}
SC=${2:?path to staticmon-headers}
CACHE=${3:-$HOME/.cache/staticmon-replay}
CTR=${CONTAINER:-smbench}
IMPL="$here/../../../scripts/staticmon-impl"
CMP="$here/comparator.py"
BUILDDIR="$(cd "$(dirname "$SC")/.." 2>/dev/null && pwd || true)"   # <repo>/builddir
mkdir -p "$CACHE"
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

command -v python3 >/dev/null 2>&1 || { echo "python3 not found; skipping" >&2; exit 77; }
{ [ -d "$CASES" ] && [ -n "$(ls -A "$CASES" 2>/dev/null)" ]; } \
  || { echo "no fixtures in $CASES (run regen); skipping" >&2; exit 77; }

native_ok() { [ -n "$BUILDDIR" ] && [ -f "$BUILDDIR/CMakeCache.txt" ] && [ -x "$SC" ]; }
docker_ok() { docker exec "$CTR" true 2>/dev/null; }

MODE=${STATICMON_TEST_MODE:-auto}
if [ "$MODE" = auto ]; then
  if native_ok; then MODE=native
  elif docker_ok; then MODE=docker
  else echo "no configured native builddir and no running '$CTR' container; skipping" >&2; exit 77; fi
elif [ "$MODE" = native ]; then
  native_ok || { echo "native build not configured ($BUILDDIR); skipping" >&2; exit 77; }
elif [ "$MODE" = docker ]; then
  docker_ok || { echo "container '$CTR' not running; skipping" >&2; exit 77; }
else echo "bad STATICMON_TEST_MODE='$MODE'"; exit 2; fi
echo "replay: mode=$MODE  cases=$CASES" >&2

# compile (sig, formula) -> $WORK/mon ; echo the run command onto stdout
compile_one() {
  if [ "$MODE" = native ]; then
    STATICMON_HEADERS="$SC" STATICMON_BUILDDIR="$BUILDDIR" \
      "$IMPL" compile -sig "$1" -formula "$2" -cache "$CACHE" -keep "$WORK/mon"
  else
    STATICMON_HEADERS="$SC" \
      "$IMPL" compile -sig "$1" -formula "$2" -container "$CTR" -cache "$CACHE" -keep "$WORK/mon"
  fi
}
# run $WORK/mon on the trace -> $WORK/sm.out
run_one() {
  if [ "$MODE" = native ]; then
    "$WORK/mon" --log "$1" > "$WORK/sm.out" 2>/dev/null
  else
    docker cp "$WORK/mon" "$CTR:/tmp/mon" >/dev/null
    docker cp "$1" "$CTR:/replay.log" >/dev/null
    docker exec "$CTR" /tmp/mon --log /replay.log > "$WORK/sm.out" 2>/dev/null
  fi
}

total=0 build_err=0 match=0 mism=0
for d in "$CASES"/*/; do
  [ -f "$d/sig" ] || continue
  id=$(basename "$d"); total=$((total+1))
  # Any non-zero exit is a regression: this case compiled + matched when the
  # fixtures were generated. (1 = staticmon now rejects; 2 = build error.)
  if ! compile_one "$d/sig" "$d/formula" >"$WORK/b.log" 2>&1; then
    build_err=$((build_err+1))
    [ $build_err -le 12 ] && { echo "BUILD ERR $id: $(head -1 "$d/formula")"; grep -m1 'error:' "$WORK/b.log"; }
    continue
  fi
  run_one "$d/trace"
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
