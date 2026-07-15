#!/usr/bin/env bash
# Replay one monitor fixture case-set (staticmon verdicts vs the stored VeriMon
# output), via the shared components/replay_cases.sh (native-first, docker fallback).
#
# Usage: run.sh <case-set> <staticmon-headers>
#   <case-set> = cases-generated (random formulas) | cases-corpus (monpoly-develop)
here="$(cd "$(dirname "$0")" && pwd)"
set_name=${1:?case-set name (cases-generated|cases-corpus)}
exec bash "$here/../../components/replay_cases.sh" \
  "$here/$set_name" "${2:?path to staticmon-headers}" \
  "${CACHE:-$HOME/.cache/staticmon-fixture}"
