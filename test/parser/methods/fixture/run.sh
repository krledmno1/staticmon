#!/usr/bin/env bash
# Replay the stored parser fixtures: run staticmon's C++ parser (parser_dump)
# over the frozen corpus (frames.bin) and check each line matches the stored
# MonPoly-oracle verdict (expected.txt) -- the AST when it parses, else the
# parse_error category. No MonPoly oracle at runtime. Fixtures are (re)generated
# offline by regen.sh.
#
# Usage: run_diff.sh <parser_dump-binary>
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
PARSER=${1:?path to parser_dump binary}
command -v python3 >/dev/null 2>&1 || { echo "python3 not found; skipping" >&2; exit 77; }
{ [ -f "$here/frames.bin" ] && [ -f "$here/expected.txt" ]; } \
  || { echo "no fixtures (run regen.sh); skipping" >&2; exit 77; }
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

# parse_error lines: compare category only (messages differ from the oracle by design)
"$PARSER" < "$here/frames.bin" | sed 's/^(parse_error.*/(parse_error)/' > "$WORK/cpp.norm"

python3 - "$here/frames.bin" "$WORK/cpp.norm" "$here/expected.txt" <<'EOF'
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
exp = open(sys.argv[3]).read().splitlines()
assert len(cpp) == len(frames), f"cpp lines {len(cpp)} != frames {len(frames)}"
assert len(exp) == len(frames), f"expected lines {len(exp)} != frames {len(frames)}"

bad = 0
for k, (f, c, m) in enumerate(zip(frames, cpp, exp)):
    if c != m:
        bad += 1
        if bad <= 25:
            print(f"=== MISMATCH #{bad} (frame {k}) ===")
            print(f"  formula:  {f!r}")
            print(f"  cpp:      {c[:300]}")
            print(f"  expected: {m[:300]}")
ok = len(frames) - bad
print(f"\n{ok}/{len(frames)} agree, {bad} mismatches")
sys.exit(1 if bad else 0)
EOF
