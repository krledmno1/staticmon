#!/usr/bin/env bash
# Regenerate every committed test fixture from the reference tools. OFFLINE
# maintainer tool -- this is the ONLY place the repo shells out to the monpoly /
# parser-oracle aliases. The tests themselves replay the committed fixtures and
# never touch these tools (see test/*/run_*.sh).
#
# Prerequisites (only for regenerating; not for running the tests):
#   - monpoly on PATH (or $MONPOLY)               -- VeriMon verdicts + typing
#   - the parser oracle: test/parser_oracle built (dune build --release
#     ./oracle.exe) or the monpoly-oracle docker image
#   - the monpoly-develop corpus (default ~/Data/Projects/monpoly-develop/tests,
#     override as arg 2 below)
#   - a built staticmon-headers (builddir/bin) + python3
#
# Usage: regen-test-fixtures.sh [staticmon-headers] [monpoly_tests_dir]
# Each suite's own regen.sh takes finer-grained args (sizes/seeds); the values
# below are the ones the committed fixtures were generated with. behavioral
# yields a superset that the maintainer then curates down before committing.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
SC=${1:-$root/builddir/bin/staticmon-headers}
CORPUS=${2:-$HOME/Data/Projects/monpoly-develop/tests}
[ -x "$SC" ] || { echo "staticmon-headers not found: $SC (build it, or pass as arg 1)" >&2; exit 1; }

echo "== monpoly_suite (needs monpoly + corpus) =="
bash "$root/test/monpoly_suite/regen.sh" "$SC" "$CORPUS"
echo "== behavioral (needs monpoly) =="
bash "$root/test/behavioral/regen.sh" "$SC" 100 11 2 40
echo "== pipeline_diff (needs monpoly) =="
bash "$root/test/pipeline_diff/regen.sh" "$SC" 400 7
echo "== parser_diff (needs the parser oracle) =="
bash "$root/test/parser_diff/regen.sh" 800 20260713
echo "== done. review 'git status test/' and commit the refreshed fixtures. =="
