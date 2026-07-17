#!/usr/bin/env python3
"""Triangle query: the canonical asymptotic WCO-vs-binary separation.
   (ONCE p(a,b)) AND (ONCE q(b,c)) AND (ONCE s(a,c))
Random relations, N edges over domain D: binary join materializes ~N^2/D
per tp; AGM bound / output is far smaller."""
import random, sys
N = int(sys.argv[1]) if len(sys.argv) > 1 else 5000
D = int(sys.argv[2]) if len(sys.argv) > 2 else 500
L = int(sys.argv[3]) if len(sys.argv) > 3 else 10
rng = random.Random(29)
with open("sig", "w") as f:
    f.write("p(int,int)\nq(int,int)\ns(int,int)\n")
with open("formula", "w") as f:
    f.write("((ONCE p(a,b)) AND (ONCE q(b,c))) AND (ONCE s(a,c))\n")
edges = {r: set() for r in "pqs"}
for r in "pqs":
    while len(edges[r]) < N:
        edges[r].add((rng.randrange(D), rng.randrange(D)))
with open("trace", "w") as f:
    evs = " ".join(f"{r}({a},{b})" for r in "pqs" for (a, b) in edges[r])
    f.write(f"@0 {evs};\n")
    for i in range(1, L):
        f.write(f"@{i};\n")
print(f"N={N} D={D} L={L} est-intermediate/tp={N*N//D}", file=sys.stderr)
