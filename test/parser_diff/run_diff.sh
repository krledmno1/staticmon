#!/usr/bin/env bash
# Differential test: staticmon C++ parser vs MonPoly oracle.
# Usage: run_diff.sh <parser_dump-binary> [n_random] [seed]
# Oracle: the natively built ../parser_oracle/_build/default/oracle.exe if
# present (build: cd ../parser_oracle && dune build --release ./oracle.exe),
# otherwise the monpoly-oracle docker image (docker/oracle.Dockerfile).
# Override with ORACLE_CMD.
set -euo pipefail

cd "$(dirname "$0")"
PARSER=${1:?path to parser_dump binary}
N_RANDOM=${2:-3000}
SEED=${3:-20260713}
NATIVE_ORACLE=../parser_oracle/_build/default/oracle.exe
if [ -z "${ORACLE_CMD:-}" ]; then
  if [ -x "$NATIVE_ORACLE" ]; then
    ORACLE_CMD=$NATIVE_ORACLE
  else
    ORACLE_CMD="docker run --rm -i monpoly-oracle"
  fi
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

python3 gen_corpus.py "$N_RANDOM" "$SEED" > "$WORK/frames.bin"

"$PARSER" < "$WORK/frames.bin" > "$WORK/cpp.out"
$ORACLE_CMD < "$WORK/frames.bin" > "$WORK/ml.out"

# parse_error lines: compare category only (messages differ by design)
norm() { sed 's/^(parse_error.*/(parse_error)/' "$1"; }
norm "$WORK/cpp.out" > "$WORK/cpp.norm"
norm "$WORK/ml.out" > "$WORK/ml.norm"

# Reconstruct the formula list for reporting
python3 - "$WORK/frames.bin" "$WORK/cpp.norm" "$WORK/ml.norm" <<'EOF'
import sys

frames = []
with open(sys.argv[1], "rb") as f:
    data = f.read()
i = 0
while i < len(data):
    j = data.index(b"\n", i)
    n = int(data[i:j])
    frames.append(data[j + 1 : j + 1 + n].decode())
    i = j + 1 + n

cpp = open(sys.argv[2]).read().splitlines()
ml = open(sys.argv[3]).read().splitlines()
assert len(cpp) == len(frames), f"cpp lines {len(cpp)} != frames {len(frames)}"
assert len(ml) == len(frames), f"ml lines {len(ml)} != frames {len(frames)}"

bad = 0
for k, (f, c, m) in enumerate(zip(frames, cpp, ml)):
    if c != m:
        bad += 1
        if bad <= 25:
            print(f"=== MISMATCH #{bad} (frame {k}) ===")
            print(f"  formula: {f!r}")
            print(f"  cpp:     {c[:300]}")
            print(f"  ocaml:   {m[:300]}")
ok = len(frames) - bad
print(f"\n{ok}/{len(frames)} agree, {bad} mismatches")
sys.exit(1 if bad else 0)
EOF
