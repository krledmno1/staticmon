#!/usr/bin/env bash
# Replay the stored behavioral cases (test/behavioral/cases) against staticmon --
# a frozen set of generated (sig, formula, trace) triples with VeriMon's verdict
# stream stored. No monpoly at runtime; only the bench container (for
# compilation) + python3. The fixtures are (re)generated offline by regen.sh.
#
# Usage: run_behavioral.sh <staticmon-headers>
here="$(cd "$(dirname "$0")" && pwd)"
exec bash "$here/../replay_cases.sh" "$here/cases" "${1:?path to staticmon-headers}" \
  "${CACHE:-$HOME/.cache/staticmon-bench-bin}"
