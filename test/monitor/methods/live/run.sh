#!/usr/bin/env bash
# ctest wrapper for the randomized differential test (staticmon vs VeriMon).
# Skips (exit 77 -> ctest "Skipped") unless the user supplies a VeriMon binary
# via $STATICMON_VERIMON (a monpoly build, run with -verified) and python3 + a
# built staticmon-headers are available. No monpoly dependency in the repo.
#
# Usage: run.sh [<staticmon-headers>]   (path passed by ctest; run.py self-resolves)
# Tunables via env: RANDOM_DIFF_ITERS (default 40), RANDOM_DIFF_SEED (default 0).
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"

command -v python3 >/dev/null 2>&1 || { echo "python3 not found; skipping" >&2; exit 77; }
if [ -z "${STATICMON_VERIMON:-}" ]; then
  echo "no \$STATICMON_VERIMON (path to a monpoly build for the VeriMon oracle); skipping" >&2
  exit 77
fi

exec python3 "$here/run.py" \
  --verimon "$STATICMON_VERIMON" \
  --iterations "${RANDOM_DIFF_ITERS:-40}" \
  --seed "${RANDOM_DIFF_SEED:-0}"
