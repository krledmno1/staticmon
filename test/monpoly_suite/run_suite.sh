#!/usr/bin/env bash
# Replay the stored monpoly-develop cases against staticmon -- no monpoly and no
# external corpus at runtime. The cases (test/monpoly_suite/cases) are extracted
# from the monpoly-develop corpus with VeriMon's verdicts stored, by regen.sh
# (an offline maintainer tool). This just replays them via ../replay_cases.sh.
#
# Usage: run_suite.sh <staticmon-headers>
here="$(cd "$(dirname "$0")" && pwd)"
exec bash "$here/../replay_cases.sh" "$here/cases" "${1:?path to staticmon-headers}" \
  "${CACHE:-$HOME/.cache/staticmon-suite}"
