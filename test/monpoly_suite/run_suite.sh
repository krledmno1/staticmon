#!/usr/bin/env bash
# Replay MonPoly's test suite against staticmon.
#
# Extracts (signature, formula, trace) cases from monpoly-develop/tests, keeps
# the ones inside staticmon's supported fragment (both staticmon and VeriMon
# accept the formula, and the monitor compiles), and checks that staticmon's
# verdicts match VeriMon (monpoly -verified) on the test's own trace.
#
# Usage: run_suite.sh <staticmon_compile> [monpoly_tests_dir]
# Requires the docker container `smbench` (docker/behavioral.Dockerfile) and
# native monpoly ($MONPOLY).
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
SC=${1:?path to staticmon_compile}
TESTS=${2:-/Users/krle/Data/Projects/monpoly-develop/tests}
MP=${MONPOLY:-/Users/krle/.opam/4.14.2/bin/monpoly}
CTR=${CONTAINER:-smbench}
CACHE=${CACHE:-$HOME/.cache/staticmon-suite}
BUILD="$here/../../scripts/staticmon-build"
CMP="$here/../behavioral/compare_verdicts.py"
mkdir -p "$CACHE"
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

docker exec "$CTR" true 2>/dev/null || { echo "container $CTR not running" >&2; exit 2; }

python3 "$here/extract_tests.py" "$TESTS" "$WORK/stage" > "$WORK/manifest_full.tsv" 2>"$WORK/ex.err"
cat "$WORK/ex.err" >&2
# Optional: LIMIT=N to sample the first N cases; FILTER=regex to select ids.
if [ -n "${FILTER:-}" ]; then grep -E "^($FILTER)" "$WORK/manifest_full.tsv" > "$WORK/manifest.tsv"
else cp "$WORK/manifest_full.tsv" "$WORK/manifest.tsv"; fi
if [ -n "${LIMIT:-}" ]; then head -n "$LIMIT" "$WORK/manifest.tsv" > "$WORK/m2.tsv" && mv "$WORK/m2.tsv" "$WORK/manifest.tsv"; fi

total=0 out_of_frag=0 mp_reject=0 build_err=0 match=0 mism=0
while IFS=$'\t' read -r id sig log mfotl; do
  total=$((total+1))
  # VeriMon must accept (the oracle); otherwise the case is not comparable.
  if ! $MP -verified -sig "$sig" -formula "$mfotl" -check 2>&1 | grep -q "is monitorable"; then
    mp_reject=$((mp_reject+1)); continue
  fi
  # staticmon-build exit code: 1 = staticmon's front-end rejects the formula
  # (not monitorable / ill-typed / unsupported construct -- out of fragment,
  # same as MonPoly's -explicitmon); 2 = the monitor failed to compile.
  "$BUILD" -sig "$sig" -formula "$mfotl" --container "$CTR" \
        --staticmon-compile "$SC" --cache-dir "$CACHE" -o "$WORK/mon" \
        >"$WORK/b.log" 2>&1; rc=$?
  if [ $rc -eq 1 ]; then
    out_of_frag=$((out_of_frag+1)); continue
  elif [ $rc -ne 0 ]; then
    build_err=$((build_err+1))
    [ $build_err -le 12 ] && { echo "BUILD ERR $id: $(head -1 "$mfotl")"; grep -m1 "error:" "$WORK/b.log" | head -1; }
    continue
  fi
  docker cp "$WORK/mon" "$CTR:/tmp/mon" >/dev/null
  docker cp "$log" "$CTR:/suite.log" >/dev/null
  docker exec "$CTR" /tmp/mon --log /suite.log > "$WORK/sm.out" 2>/dev/null
  $MP -verified -sig "$sig" -formula "$mfotl" -log "$log" > "$WORK/mp.out" 2>/dev/null
  if python3 "$CMP" "$WORK/sm.out" "$WORK/mp.out" > "$WORK/d.txt" 2>&1; then
    match=$((match+1))
  else
    mism=$((mism+1))
    if [ $mism -le 20 ]; then
      echo "=== MISMATCH $id: $(head -1 "$mfotl") ==="
      head -6 "$WORK/d.txt"
    fi
  fi
done < "$WORK/manifest.tsv"

echo
echo "total=$total  out_of_fragment=$out_of_frag  verimon_rejected=$mp_reject  build_error=$build_err"
echo "compared: match=$match mismatch=$mism"
[ "$mism" -eq 0 ] && [ "$build_err" -eq 0 ]
