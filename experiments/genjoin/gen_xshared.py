#!/usr/bin/env python3
"""Minimized x-shared quadratic-intermediate microbenchmark
(docs/LFTJ-STATICMON.md §1/§9): join two ONCE-accumulated relations on x only,
with a dense negative discarding most of the per-x thread cross product.

  (ONCE p(t1,x)) AND (ONCE q(t2,x)) AND (NOT r(t1,t2))

Binary plans materialize X*T^2 rows per time-point before the anti-join;
the generic join enumerates and vetoes without materializing.
"""
import random, sys

X       = int(sys.argv[1]) if len(sys.argv) > 1 else 50    # distinct x values
T       = int(sys.argv[2]) if len(sys.argv) > 2 else 40    # threads per x
L       = int(sys.argv[3]) if len(sys.argv) > 3 else 50    # trace length (tps)
DENSITY = float(sys.argv[4]) if len(sys.argv) > 4 else 0.95  # r pair density
rng = random.Random(13)

POOL = T * 2  # global thread pool; each x touched by a random T-subset
with open("sig", "w") as f:
    f.write("p(int,int)\nq(int,int)\nr(int,int)\n")
with open("formula", "w") as f:
    f.write("((ONCE p(t1,x)) AND (ONCE q(t2,x))) AND (NOT r(t1,t2))\n")

with open("trace", "w") as f:
    # tp 0: r pairs (the 'locked' relation, dense) -- keep r constant via ONCE?
    # NOT r must see r at the CURRENT tp; emit r at every tp instead: cheaper
    # to emit once and use ONCE r? Keep the cluster shape of the plan: NOT r
    # checks the current tp, so restate r each tp.
    pairs = [(a, b) for a in range(POOL) for b in range(POOL)
             if rng.random() < DENSITY]
    # spread p/q accumulation over the first 10 tps, then L-10 steady tps
    per_x_threads = {x: rng.sample(range(POOL), T) for x in range(X)}
    events_pq = []
    for x in range(X):
        for t in per_x_threads[x]:
            events_pq.append(("p", t, x))
            if rng.random() < 0.5:
                events_pq.append(("q", t, x))
    rng.shuffle(events_pq)
    chunk = max(1, len(events_pq) // 10)
    rtxt = " ".join(f"r({a},{b})" for a, b in pairs)
    for i in range(L):
        evs = ""
        if i < 10:
            evs = " ".join(f"{n}({t},{x})" for (n, t, x) in
                           events_pq[i*chunk:(i+1)*chunk])
        f.write(f"@{i} {rtxt} {evs};\n")
print(f"X={X} T={T} L={L} |r|={len(pairs)} pq_events={len(events_pq)}", file=sys.stderr)
