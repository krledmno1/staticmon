# FRZ optimization program

Plan of record for the freeze-operator performance work. Written after the
first FRZ benchmark round (see `experiments/runner/frz_config.yaml`), which
established the baseline below and the two infrastructure gaps that must be
closed before any optimization can be honestly evaluated.

## Baseline (2026-07-17, warm-up corrected)

staticmon ties or beats monpoly on every FRZ body shape except **nested**,
and VeriMon (the verified implementation) is the slow reference throughout:

| shape @800 tps | staticmon | monpoly | verimon |
|---|---:|---:|---:|
| current | 0.018 | 0.019 | 0.81 |
| once [0,10] | **0.033** | 0.039 | 6.49 |
| once [0,\*) | **0.354** | 0.488 | 5.13 |
| since [0,10] | **0.038** | 0.080 | TIMEOUT |
| eventually [0,10] | **0.512** | 1.780 | TIMEOUT |
| nested @400 | TIMEOUT | 1.237 | disq |

Measured scaling of the nested shape (doubling ratios): staticmon ~8x
(**cubic**, Θ(j³)), monpoly ~4x (**quadratic**, Θ(j²)). An event-rate sweep at
fixed length showed a *flat* ~5x gap across evr 10..80 and equal peak RSS:
copies are a constant factor here, not the growth term. The cubic comes from
the window guard (see step 0).

## Process

- **One branch per optimization**, in dependency order:
  `opt/log-shapes` and `opt/profiling` (infrastructure), then
  `opt/frz-window-guard` (step 0), `opt/frz-depth-nested` (B),
  `opt/frz-fusion`, `opt/shared-db` (A).
- **Merge gates**, per branch:
  - *Correctness — oracle is VeriMon:* all fixture suites (98 FRZ + corpus +
    generated + regression), live random differential with a fresh seed **and**
    a pinned seed, plus the branch-specific targeted generator described under
    each optimization. Zero mismatches.
  - *Performance — reference is `master`:* the FRZ config and the general
    microbenchmark config, run on both branches in the same host session,
    warm-up corrected. Inside the targeted fragment the predicted improvement
    must materialize; outside it every `ok` row must stay within ±3%.
- After each merge, store the harness CSVs under
  `experiments/baselines/<short-sha>/` so "master as performance reference" is
  a number, not a memory.
- Host constraints while benchmarking: never run a second build against
  `builddir` concurrently (ninja dep-log races produce silently stale
  binaries); always warm up a freshly copied binary before timing it (macOS
  charges ~0.2s to the first exec of a new inode).

## Infrastructure (prerequisites)

### I1 — Log-shape diversity in the generator (`opt/log-shapes`)

**Gap.** The FRZ benchmarks fix index rate (1 tp/ts), timestamp step (1), int
payloads, one event rate and one value domain. Three of the four optimizations
have log-dependent benefits, so single-point logs cannot power the gates.

**Plan.** A reusable `LogShape` record in `EventGenerators.hs`, threaded
through `FrzConfig` (and later the other families):

- `eventrate` — events per time-point per predicate;
- `tpPerTs` — stuttering: how many time-points share one timestamp
  (already expressible via `numtpperts`, folded in for symmetry);
- `tsStep` — sparsity: how far the timestamp advances between time-points;
- `domain` — value range (small: dense collisions and large intermediate
  tables; large: mostly distinct);
- `payload` — `int` or `string` (the expensive-copy path; needs a `P(string)`
  signature variant).

`gen_config.py` emits one-factor-at-a-time sweeps; each optimization sweeps the
axis its benefit hinges on (B: `tsStep`, `tpPerTs`; A: `payload`, `eventrate`;
step 0 / fusion: length). Defaults reproduce today's logs byte-for-byte so the
existing baseline stays comparable. Every new generator config gets one
differential run against VeriMon before it is trusted for timing.

### I2 — Profiling mode in the harness (`opt/profiling`)

**Gap.** Hotspots have so far been *inferred* from scaling ratios.
Prioritization — A's fate in particular — needs direct evidence.

**Plan.** A `--profile` flag on the `bench` subcommand: run each
(benchmark, monitor) once untimed with a sampling profiler attached
(`sample` on macOS — no entitlements needed for our ad-hoc-signed binaries;
`perf record` on the Linux/container path), storing
`<bench>/profile-<monitor>.txt` alongside peak RSS. A summarizer extracts the
top inclusive frames and the share of the categories that decide the plan:
copying (`memcpy`, `operator new`, string constructors), hashing (absl), real
monitoring (`msince`/`monce` eval), and replay machinery (`mfrz::feed`,
instance construction).

**First deliverable:** baseline profiles of `frz_nested`, `frz_once[0,*)` and
`frz_eventually` at two sizes on `master`. This decides A before any A code
exists, and checks the step-0/B diagnosis (expectation: instance-replay frames
dominate; copies are a minority share).

**Baseline results (2026-07-17, `profile.py`, self-time shares + inclusive
copy):**

| shape | eval | hash | join | alloc | mfrz::feed | incl. copy |
|---|---:|---:|---:|---:|---:|---:|
| nested@400 (int) | 24% | 18% | 14% | 32% | 0.6% | **18.5%** |
| once[0,\*)@800 (int) | — | 16% | 22% | 27% | 27% | **17.5%** |
| eventually@800 (int) | — | 19% | 16% | 24% | 20% | **12.1%** |
| once[0,\*) long (str) | 12% | 12% | 8% | 22% | **35%** | 13.7% |

Diagnosis confirmed: on nested, replay *computation* (eval+hash+join ≈ 57%)
dominates — the win is in doing fewer replays (step 0, B), not cheaper copies.
Copy-attributable time is 12–18% on int logs; the string payload costs +28%
wall with the broadcast copy surfacing in `mfrz::feed` (27% → 35%). That is
opt A's ceiling on these shapes — a modest constant factor, to be re-profiled
post-B per the decision rule.

## Optimizations

### Step 0 — Lag-aware window guard (`opt/frz-window-guard`)

**What is handled inefficiently.** The bounded-past replay window is disabled
whenever the body reads *any* enclosing-binder-bound predicate
(`references_bound_pred` in `translate.h`), on the grounds that binder streams
are positionally aligned and could lag. But only a binder whose definition
contains a future operator can lag: a frozen predicate is a per-batch constant
broadcast (`mfrz::feed` emits exactly `ts.size()` entries every batch) and can
never lag. Since a nested freeze's body references the outer frozen predicate
by construction, the inner window is always killed, and every inner instance
replays its whole inner prefix: Σ_k O(k²) = **Θ(j³)**.

**Implementation.** `translate.h` only. `bound_scope_` entries carry a
`lag_free` bit. **Corrected during implementation** (the original sketch said
"FRZ always lag-free", which is wrong): lag-freeness depends on the binder's
*runtime representation*, not its surface kind —

- temporal-mode FRZ (`mfrz`): always lag-free, even with future operators in
  the definition — those delay *instance creation* (verdict latency), never
  the per-batch completeness of the broadcast stream;
- current-only FRZ (compiled to `mlet`) and LET: lag-free iff the definition
  has no future operator — a current-only freeze over `EVENTUALLY …` *is* a
  lagging binder (regression fixture `x_lag_trap` pins exactly this: naively
  marking it lag-free would window the inner replay across a lagging
  positional stream and produce wrong verdicts);
- LETPAST: conservatively lagging.

The guard becomes "references a *lagging* enclosing bound predicate". This
rests on an asymmetry worth stating: an enclosing predicate's *values* are
already recorded in the replayed history (it contributes depth 0 — nothing is
recomputed); only its stream alignment matters.

**Largest affected fragment.** Temporal-mode FRZ with a purely bounded-past
body that references an enclosing binder-bound predicate — dominated by nested
FRZ of any depth, plus FRZ under lag-free LETs.

**Testing.** Existing `x_triple_nest`, `x_nest_inner_freeze`,
`x_enclosing_let` fixtures target this guard directly; plus the full suites and
live differential. New targeted generator: bodies referencing (a) outer frozen
predicates with events placed exactly at and just outside `ts_k − d`
(window-edge exactness), (b) enclosing past-only LETs (the newly windowed,
risky path), (c) enclosing future LETs, which must *stay* unwindowed —
verified by verdict equality, (d) stuttering-timestamp logs where the window
spans many time-points.

**Measurement.** The nested doubling ratio must fall from ~8 to ~4
(`frz_nested@400`: TIMEOUT → monpoly class). Add a depth-3 nesting benchmark
(the win compounds per level). Everything else within noise.

**Results (2026-07-17, seeded logs, branch vs master, staticmon):**

| nested @ | master | branch | speedup |
|---|---:|---:|---:|
| 100 | 0.214 | 0.085 | 2.5× |
| 200 | 1.418 | 0.260 | 5.5× |
| 400 | 10.73 | 0.985 | **10.9×** |
| 800 | TIMEOUT (60s) | 3.91 | ≥15× |

Master ratios ×6.6/×7.6 (cubic); branch ×3.1/×3.8/×4.0 (quadratic) —
Θ(j³) → Θ(j²·w) as predicted. All other rows within the ±3% gate
(eventually@800 +1.4%, once[0,\*)@800 +1.7%). Emitted-Depth introspection:
nested inner freeze `frz_no_depth → 10UL`; enclosing-past-LET windowed at
its true depth; `x_lag_trap` and enclosing-future-LET correctly stay
unwindowed. Correctness: 102/102 FRZ fixtures (incl. 4 new lag-guard
fixtures) + 3/3 regression vs VeriMon.

### Opt B — Depth analysis through nested binders (`opt/frz-depth-nested`)

**What is handled inefficiently.** `frz_body_depth` returns `nullopt` at any
nested LET/FRZ, so an outer freeze over a nested-binder body replays the whole
prefix per instance even when its true temporal reach is finite. Post-step-0
nested FRZ is Θ(j²·w); the true reach permits **Θ(j·w²)** — linear in trace
length, a class better than MonPoly.

**Implementation.** `translate.h` only: make the analysis compositional over an
environment `pred -> depth contribution`.

- `FRZ p = α IN β`: `max(depth α, depth β with p ↦ 0)` — frozen `p` is
  constant, so temporal operators over it add no data reach beyond α at the
  freeze index.
- `LET p = α IN β`: `depth β with p ↦ depth α` — a `p`-atom at temporal offset
  `b` contributes `b + depth α` (p@j = α@j, recomputed by the instance from
  replayed data — the opposite of the enclosing case above).
- LETPAST: unbounded (the recursion chains arbitrarily far back).
- PREV/NEXT: still excluded; a separate later extension (the bounded-interval
  argument exists but carries its own risk).

**Largest affected fragment.** Temporal-mode FRZ whose body is bounded-past
*including* arbitrarily nested FRZ and non-recursive LET: atoms, boolean /
quantifier / aggregation structure, finite-upper ONCE/SINCE, nested binders.
Excludes future operators, PREV/NEXT, LETPAST, regex.

**Testing.** Two layers.

1. *Analysis introspection:* a debug flag on `staticmon-headers` printing each
   freeze's computed depth; a table of hand-computed formulas (especially the
   additive cases: `p` under `ONCE[0,b]` with definition `ONCE[0,c]` ⇒ `b+c`)
   asserted exactly. Catches analysis bugs that need no verdict divergence.
2. *Differential:* adversarial logs with events at `ts_k − depth` (must matter)
   and `ts_k − depth − 1` (must not); randomized nested-binder formulas with
   timestamp-gap logs against VeriMon; full suites.

**Measurement.** Nested ratios → ~2 per doubling (linear). The `tsStep` /
`tpPerTs` sweeps from I1 characterize the honest boundary: stuttering or dense
timestamps inflate `w` until the window is useless — report that curve rather
than hide it. Single-level bounded FRZ must be unchanged (already windowed).

### FRZ fusion — same-index multi-freeze (`opt/frz-fusion`)

**What is handled inefficiently.** `FRZ F = α₁ IN FRZ G = α₂ IN β` freezes both
predicates at the *same* outer index (the inner node is the outer body's root,
so it is only ever evaluated at the outer time-point), yet the runtime nests:
every outer instance contains a whole inner `mfrz` that re-runs α₂'s instance
machinery. That is duplicated *computation*, not just data — the nesting
multiplication itself. Step 0 and B shrink its factors; fusion removes it.

**Implementation.** Translator: recognize maximal chains where each FRZ is the
immediate body-root of the previous and collapse them into an n-ary node
`ex_frz_n{[(p₁,α₁),…,(pₙ,αₙ)], β}`. Dependent definitions (αᵢ referencing pⱼ,
j<i) fuse with *ordered* evaluation. Bail on shadowing (same name/arity rebound
mid-chain) and on any inner freeze that is not at the body root (different
index — not fusable). Runtime: an `mfrz_n` template with one instance set —
per outer time-point compute all frozen tables in order, then one β replay with
all bindings broadcast. Depth-d chains collapse from d instance layers to one:
post-B Θ(j·w²) → **Θ(j·w)**. Composes with B (a fused node is single-level, so
the depth analysis simplifies).

**Largest affected fragment.** Body-root chains of FRZ of any length,
independent or dependent definitions — precisely the shape the paper's rewrite
rules (1′–10′) produce when several rules fire at one position. Not: freezes
under temporal operators inside the body, or freezes inside definitions.

**Testing.** Unique advantage — **staticmon vs staticmon differential**: keep a
translator flag that disables fusion and compare fused against unfused on
randomized fusable chains (depth 2–4, dependent, independent, shadowing, and
non-fusable lookalikes), as well as against VeriMon. An introspection flag
asserts that fusion fired (or correctly did not) per formula, so silent
non-fusion cannot masquerade as a pass.

**Measurement.** `frz_nested` should converge onto the single-freeze
`frz_once` curve; the depth-3 benchmark shows the compounding win; single
freezes must be untouched (fusion must not even trigger — asserted by
introspection).

### Opt A — Shared history via refcounted database payloads (`opt/shared-db`)

**What is handled inefficiently.** Every recorded batch is a deep copy of
`vector<vector<variant<int64,double,string>>>`, and every feed broadcasts a
copy of the frozen table. Under instance multiplication these copies are
O(j·w) (bounded, post-B) or O(j²) (unbounded/future shapes, which B cannot
window), duplicated again per nesting level — plus permanently duplicated
retention for unbounded histories. Caveat from the measurements: at fixed
length the gap to monpoly is flat across event rates and peak RSS is at parity,
so copies are a constant factor here, not the growth term. Hence: last, and
evidence-gated.

**Implementation.** Shared immutable tables at per-table granularity:
`mapped_type = vector<shared_ptr<const database_table>>`. The frozen broadcast
becomes n pointer copies of one table; history entries share event data across
nesting levels. The map keeps value semantics (a shallow copy shares payloads);
producers (trace parser, `tabs_to_db_tabs`, `mfrz` feed/history) wrap once,
consumers (`mpredicate`) dereference. Tables are write-once after construction
(verify and assert), so no copy-on-write is needed. Explicitly *not* an
overlay-view database type — that would change every operator's lookup
interface.

**Largest affected fragment.** Performance-wise: all temporal-mode FRZ,
dominated by the shapes B cannot window (future bodies, unbounded intervals,
PREV/NEXT bodies) and by string-heavy logs. Risk-wise: **everything** — the
representation flows through every operator, which is exactly why it is a
separate branch with a whole-suite gate.

**Testing.** A pure refactor, so verdicts must be *byte-identical* on every
suite, the live differential and the `-verbose` parity check. Add an
ASan/LSan pass over a sample of monitor runs (the lifetime/leak bug class) and
a determinism check (repeated runs identical).

**Measurement.** This settles the log-dependence question quantitatively: the
I1 matrix {int vs string payload} x {event-rate sweep} x {`once[0,*)`,
`eventually`, post-B `nested`}, plus peak RSS at large length on unbounded
shapes (the retention win). The critical *negative* gate: non-FRZ suite rows
within noise (the added indirection in `mpredicate` is the regression risk).
**Decision rule:** if the I2 baseline profile shows copy-category frames below
~10% on the post-B branch and the string-log delta is small, defer A — a valid
outcome of this plan.

## Order

```
I1 (log shapes) ─┬─→ baseline profiles (I2) ─→ step 0 ─→ B ─→ fusion ─→ A (evidence-gated)
I2 (profiling) ──┘
```

Step 0 precedes B because B's window is only as good as the guard that permits
it. B precedes fusion so that fusion is measured against an already-linear
baseline instead of collecting credit for B's work. A is last because the
evidence so far says its benefit is a log-dependent constant, and the profile
after B tells us whether it is worth the only change in the set that touches
every operator's data path.

The targeted generators built for these gates (window-edge logs, nested-binder
formulas, fusable chains) should be folded back into `gen_bench.py` and the
live differential after each merge, so that optimization-specific tests become
permanent regression pressure rather than branch artifacts.
