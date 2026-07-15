#!/usr/bin/env bash
# Regenerate the parser_diff fixtures: freeze the corpus (frames.bin) and the
# MonPoly oracle's per-frame verdict -- the parsed AST, or the parse_error
# category for inputs that must fail (expected.txt). OFFLINE maintainer tool:
# needs the parser oracle, either the natively built ../parser_oracle
# (cd ../../components/oracle && dune build --release ./oracle.exe) or the
# monpoly-oracle docker image (docker/oracle.Dockerfile). Override with
# ORACLE_CMD. Writes parser/methods/fixture/{frames.bin, expected.txt}.
#
# Usage: regen.sh [n_random] [seed]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
N=${1:-800}; SEED=${2:-20260713}
NATIVE="$here/../../components/oracle/_build/default/oracle.exe"
if [ -n "${ORACLE_CMD:-}" ]; then :
elif [ -x "$NATIVE" ]; then ORACLE_CMD="$NATIVE"
elif command -v docker >/dev/null 2>&1 && docker image inspect monpoly-oracle >/dev/null 2>&1; then
  ORACLE_CMD="docker run --rm -i monpoly-oracle"
else echo "no parser oracle (build ../../components/oracle or the monpoly-oracle image)" >&2; exit 1; fi

python3 "$here/../../components/generator/gen_corpus.py" "$N" "$SEED" > "$here/frames.bin"
# parse_error lines: store the category only (messages differ from staticmon by design)
$ORACLE_CMD < "$here/frames.bin" | sed 's/^(parse_error.*/(parse_error)/' > "$here/expected.txt"
echo "wrote $(wc -l <"$here/expected.txt" | tr -d ' ') frames to expected.txt (+ frames.bin)" >&2
