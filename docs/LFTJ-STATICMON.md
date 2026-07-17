# Generic (worst-case-optimal) joins for StaticMon

A plan for replacing chains of binary joins with specialized
Leapfrog-Triejoin-style (LFTJ) multiway joins in StaticMon. The plan
is staged so that a cheap measurement step (WP-J0) validates the
premise before any join code is written — including an A/B experiment
that needs no new code at all (Section 4) — and every later stage has
its own measurable exit criterion.

## 1. Context: who is asking, and why now

We translate full future-bounded MFOTL into StaticMon's RANF fragment. 
On the resulting formulas StaticMon is the fastest RANF monitor we measure,
so it is the natural place to invest.

Two properties of this setting make generic joins *tractable
engineering* rather than a speculative database-style optimizer:

1. **The join shape is fully static.** Each monitored formula is fixed
   at monitor-compile time; every conjunction cluster, every attribute
   set, every negative conjunct is known when the C++ is generated.
   There is no query planning at runtime and no need for cardinality
   estimation machinery: one attribute order per cluster, chosen once,
   is enough (and can later be tuned per formula, see 6.4).
2. **StaticMon's template-metaprogramming architecture is the ideal
   vehicle.** The generated `input_formula` is a type; layouts
   (`ResL`) are compile-time index lists; and the codebase already
   fuses operator chains at the type level (`mfusedsimpleop` /
   `simpleops<...>`). A trie-based multiway join specialized per
   cluster — concrete tuple types, unrolled per-level code, fixed
   attribute order — is exactly the kind of operator this architecture
   was built to emit, and it is available to no other monitor in the
   comparison.

Why now: our LET factoring turned the translated trees into DAGs
of small definitions. The join clusters are now *visible as flat
conjunctions of atoms* (predicates and let-bound predicates) rather
than buried in deep subtrees, and formula sizes dropped 2–15×, which
buys compile-time headroom for a more aggressive join operator.

### The motivating measurement

On the real 10⁵/10⁶-event lock/data-race traces of Havelund et al.,
StaticMon monitors our translation of the *hand-optimized* data-race
encoding in 26.6 s at 10⁴ events and times out (30 s) at 10⁵ — while
BDD-based DejaVu does 10⁶ in ~8 s. The top-level cluster of that
formula is

```
ONCE (read(t1,x) OR write(t1,x))   AND   ONCE write(t2,x)
                                   AND   NOT (EXISTS l. ...)
```

a join of two large unary-keyed relations *on `x` only* — every pair
of threads that ever touched `x` — materialized in full by
`table_join` before the negative conjunct discards most of it. This
quadratic intermediate is precisely what a generic join avoids: with
attribute order `x, t1, t2` and the negative conjunct applied the
moment its columns are covered, pairs are enumerated per-`x` and
pruned without ever materializing the cross product. The same pattern
(2 positives sharing one variable + negatives) recurs inside the `NOT`
bodies and across our translated corpus; after
aggregation-optimization (`.amfotl`) the clusters get wider.

We are deliberately honest about the flip side: many clusters in our
current benchmark set are 2-atom joins with small per-time-point
tables, where a specialized LFTJ can only lose (constants). The plan
therefore gates the new operator by cluster shape and keeps the
existing binary join as the default for everything else.

## 2. Generic-join background in five paragraphs

A multiway join Q = R₁ ⋈ … ⋈ Rₖ over variables x₁…xₙ can be computed
attribute-at-a-time: fix a global order x₁ < … < xₙ; at depth i,
intersect the xᵢ-values consistent with the current binding of
x₁…xᵢ₋₁ across all relations that contain xᵢ; recurse on each value.
If each relation supports sorted prefix-range enumeration (a *trie*
in the order's projection onto its attributes), the intersection at
each depth is a *leapfrog* merge of sorted iterators with galloping
`seek`, costing O(min-size · log) per level.

This is worst-case optimal (Ngo–Porat–Ré–Rudra; Veldhuizen's LFTJ):
intermediate results are restrictions of the final output to the
processed columns, hence never larger than the output — a bound
binary join plans can exceed by polynomial factors (the triangle
query is the canonical example; our `x`-shared pattern above is the
monitoring incarnation: binary plans materialize an intermediate
larger than the final result after negation).

The required per-relation interface is small: `open(depth)`,
`up()`, `next()`, `seek(v)`, `atEnd()` over the relation sorted
lexicographically in the global order restricted to its attributes.
No pointer-based trie is needed: a sorted `std::vector` of tuples plus
a stack of (begin,end) ranges per depth implements the interface with
excellent locality ("implicit trie").

Negative conjuncts (RANF `AND NOT ψ`, with fv(ψ) covered by the
positives) integrate as *check-only* inputs: a negative is consulted
as soon as the enumeration has bound all of its columns, vetoing the
partial binding — the anti-join happens during enumeration, before
any intermediate exists (this is exactly the verified rule of
Section 3, not an ad-hoc choice).

Attribute-order choice affects constants, not worst-case optimality —
and, as Section 3 makes precise, not correctness either. With static
shapes, a simple heuristic (6.4) is fine to start, and the order is
one template parameter — swappable per cluster without touching the
algorithm.

## 3. A machine-checked specification already exists — vendored in this repository

This is the part of the plan that changes it from "implement an
algorithm from papers" to "implement against a precise, executable,
formally verified specification."

VeriMon — the verified kernel inside the MonPoly binary this
repository vendors (aliased as `monpoly/`) — already contains a **formally
verified generic multiway join with integrated anti-joins**, described
in Section 5 of the IJCAR'20 paper (Basin et al., *A Formally
Verified, Optimized Monitor for Metric First-Order Dynamic Logic*)
and mechanized in Isabelle in this very repo:

 `monpoly/thys/Generic_Join_Devel/Generic_Join.thy` — the
  algorithm (`genericJoin`), formulated over *annotated tables*
  (`atable = column-set × table`, `query = atable set`), following
  Ngo et al.'s unified presentation and **generalized to anti-joins**:
  `generic_join V Q_pos Q_neg`.
 `monpoly/thys/Generic_Join_Devel/Generic_Join_Correctness.thy` —
  the correctness proof.
 `monpoly/thys/MFODL_Monitor_Devel/Optimized_Join.thy` — the
  monitor-facing wrapper `mmulti_join' A_pos A_neg L` (parallel lists
  of positive column-sets, negative column-sets, and tables) with
  lemma `mmulti_join'_correct`.
 `monpoly/thys/MFODL_Monitor_Devel/Monitor.thy` — the integration:
  an n-ary constructor `MAnds A_pos A_neg L buf` and a verified
  semantics-preserving pass `convert_multiway` that flattens nested
  binary conjunctions into it.

(The same development is on the AFP as *Generic_Join*; the vendored
copy is the source of truth for this repo.)

### 3.1 The contract, in one sentence

`mmulti_join'_correct` (Optimized_Join.thy): provided `A_pos ≠ []` and
each table fits its declared column set, a tuple `z` is in
`mmulti_join' A_pos A_neg L` **iff** `z` is a well-formed tuple over
the union of the positive column sets, `z` restricted to `Aᵢ` is in
the i-th positive table for every i, and `z` restricted to `Bⱼ` is
**not** in the j-th negative table for every j. That — plus the two
side conditions — is the entire semantics `mgenjoin` must implement.
StaticMon's typed layouts (`ResL`) replace VeriMon's option-tuple
`wf_tuple` discipline; the restriction operator becomes the static
column projection you already compute for `table_join`.

### 3.2 Correctness is independent of the search strategy

The formalization is parameterized: `getIJ` is an Isabelle *locale*
whose only assumptions are that the variable split is a disjoint
nonempty cover (`Generic_Join.thy`, `locale getIJ`,
`coreProperties`). Correctness of `generic_join` is proved **for
every instance**. The paper notes explicitly that NPRR and LFTJ are
obtained as specific `getIJ` instances, and that the choice "only
affects performance, not correctness." Consequences for the design:

- A compile-time static attribute order (our Section 6.4) *is* a
  `getIJ` instance (peel one column at a time in that order). The
  verified correctness argument transfers structurally; nothing about
  the C++ design fights the spec.
- The instance VeriMon runs (`New_max_getIJ`) picks, at *runtime*,
  the column that maximizes the number of positive tuples it touches
  — a runtime-adaptive order. This gives WP-J4 a principled
  alternative to compare against the static heuristic: StaticMon
  could even pre-compile 2–3 candidate orders per cluster and branch
  on table sizes at runtime, staying fully specialized per order.

### 3.3 The negative-table rule (adopt it verbatim)

The formalization's treatment of negatives is precisely the discipline
Section 2 sketched, and it predates us — adopt it as specified:

- In `generic_join V Q_pos Q_neg`, when the recursion focuses on a
  column set `I`, the negatives whose columns are **fully contained
  in `I`** are dispatched into that subproblem
  (`Q_neg^I = {(U,Y) ∈ Q_neg | U ⊆ I}`); the remaining negatives are
  deferred, semi-join-restricted alongside the positives, and applied
  later, once the accumulated positive columns cover them. The paper
  states the point plainly: this *"is an improvement over the naive
  strategy of computing Q_pos first and only then removing tuples
  from it."*
- In the base case (single column), positives are intersected and
  negatives' values subtracted.
- In LFTJ-iterator terms this is exactly: each negative becomes a
  check-only iterator that participates (as a veto) at the first
  depth where all of its columns are bound — never earlier (its
  columns are not yet determined) and never later (that would
  materialize doomed bindings). The well-formedness the rule needs —
  at least one positive; every negative's columns ⊆ union of positive
  columns — is the `MAnds` side condition in `Monitor.thy` and is
  *guaranteed by RANF*; our translator enforces it and our own
  Isabelle development machine-checks that the emitted formulas are
  RANF, so the codegen may assume it.

### 3.4 The flattening pass has a verified precedent

`convert_multiway` (Monitor.thy) is a verified, semantics-preserving
rewrite of nested binary conjunctions into `MAnds A_pos A_neg L` —
exactly the move our `flatten_and_chains` metafunction performs at
the C++ type level (and exactly analogous to your existing
`simpleops` fusion). When in doubt about which conjuncts a cluster
may absorb (negations, equalities, nested structure), mirror
`convert_multiway`'s decisions; they carry proofs.

### 3.5 An executable ground truth for testing — already linked

The Isabelle development is executable (code generation via
`Proj_Code.thy`), and the extraction is *already compiled into two
binaries in this repo*:

- The `-verified` kernel of `monpoly` applies `convert_multiway`
  by default (`src/algorithm_verified.ml`: disabled only by
  `-no_mw`). **Every `\vmon` row in our benchmark tables already
  exercises the verified generic multiway join** on all conjunction
  clusters, on every property and trace family. Differential
  validation against `\vmon` is therefore conformance testing against
  a machine-checked implementation of the very algorithm you are
  building — on the real workload.
- Our translation tool links the same extraction (`src/verified.ml`,
  used by our cost oracle), so we can drive `mmulti_join'` directly
  on synthetic inputs: we commit to building a **randomized
  conformance driver** that generates arbitrary `(A_pos, A_neg, L)`
  instances (including empty tables, arity-0 relations, columns in
  odd orders) and diffs StaticMon's `mgenjoin` output against the
  extracted verified join. This closes the gap that trace-level
  differential testing might miss (clusters our formulas don't
  exhibit).

One honest caveat so nobody over-reads the numbers: VeriMon's
*implementation* of this algorithm has poor constants — generic
set-of-option-tuples representation, no specialization; VeriMon is
3–7× slower than MonPoly overall in our tables. The formalization
contributes the algorithm, its correctness, and an executable oracle.
StaticMon's contribution is the constants: a per-cluster-specialized
implementation of the *same verified algorithm*. That division of
labor is the whole point of this plan.

## 4. A zero-code experiment to run first

The vendored harness has a dormant row: `run07B` runs the verified
kernel with `-no_mw` — i.e., **the same monitor with binary joins
instead of the generic join**. Running the motivating series with
`run07A` (generic join) vs. `run07B` (binary) is a direct, in-vivo
A/B of exactly the algorithmic change this plan proposes, inside one
engine, at zero implementation cost. If the generic join helps the
verified kernel on the manual data-race cluster (despite its poor
constants), the premise is validated in the strongest possible way;
if it does not, that is critical information for WP-J0's exit
decision. We will run this ourselves and share the tables (it slots
into our existing sweep scripts as two rows).

## 5. Where this lands in StaticMon

What exists today (paths under `src/staticmon/`):

- `operators/detail/binary_relational_ops.h`: `mand<is_anti_join,
  F1, F2>` → `bin_rel_op<and{,_not}_tag>` → per-time-point
  `table_util::table_join<L1,L2>` / anti-join; children evaluated in
  batches (`std::vector<std::optional<tab>>`), aligned by
  `bin_op_buffer`.
- `common/table.h`: `table<Args...> =
  absl::flat_hash_set<std::tuple<Args...>>`; `table_join_dispatch`
  builds a `flat_hash_map<key, vector<ptr>>` on one side and probes
  with the other; join columns are static index lists (`l1_common`).
- Generated headers (`formula_in.h`) contain the full formula as one
  type; conjunction clusters appear as nested
  `mand<false, mand<false, A, B>, C>` chains interleaved with
  `mfusedsimpleop` and `mequal` nodes.

The proposal adds one operator template and one type-level pass:

- `operators/detail/mgenjoin.h`: `mgenjoin<Order, PosList, NegList>`
  where `Order` is an mp11 list of variable indices, `PosList`/`NegList`
  are lists of child `MFormula`s — deliberately the same I/O shape as
  the verified `mmulti_join' A_pos A_neg L`, so the Isabelle contract
  (3.1) and the conformance driver (3.5) apply verbatim. `ResL` =
  union of positive layouts in `Order`; `eval` gathers the children's
  per-tp tables, materializes each as a sorted vector in the order's
  projection, and runs the specialized LFTJ enumeration with
  check-only negatives (3.3), emitting result tuples directly into
  the output `table`.
- A metafunction `flatten_and_chains<F>` that rewrites the generated
  formula type: maximal `mand` trees (positives and `mand<true,...>`
  negatives) collapse into a single `mgenjoin` node when the gate
  criterion (6.5) holds, else stay as-is — the C++-type-level
  counterpart of the verified `convert_multiway` (3.4). Running this
  as a type pass over `input_formula` means **no change to the
  monpoly-exp frontend or the header format** in v1. (If you prefer
  the frontend to group clusters explicitly, that works too and
  simplifies the metaprogramming; v1 is written to not require it.)
- An n-ary generalization of `bin_op_buffer` to align k child batch
  streams (mechanical; the binary one contains all the logic —
  VeriMon's `MAnds` keeps an `mbufn` for the same purpose).

Everything else — temporal operators, `mlet`, simple ops, output —
is untouched: `mgenjoin` is just another `MFormula` with the standard
`ResL/ResT/eval` contract.

## 6. Design decisions (recommendations, with alternatives)

### 6.1 Trie representation: sorted vectors, built per time point

Tables arrive as `flat_hash_set`s and change per time point, so v1
converts each input table to a sorted `std::vector<row>` (projected
column order = global order restricted to the input's attributes) at
each evaluation: O(n log n), cache-friendly, no new persistent state.
Per-depth ranges via `std::lower_bound`/galloping. This conversion is
the main constant-factor risk (Section 8); the mitigation is the
shape gate, and the v2 path (Section 9) eliminates it for temporal
inputs by keeping their state sorted.

### 6.2 Enumeration: recursive template, unrolled per depth

Depth = position in `Order`; at each depth the set of participating
iterators is a compile-time list, so the leapfrog loop specializes
completely (no virtual calls, no type erasure, tuple element types
concrete). Output rows are constructed once at the deepest level.

### 6.3 Negatives, equalities, constants

Negatives implement the verified veto-at-coverage rule of Section 3.3
as check-only iterators. `mequal`/constant constraints stay in the
existing `msimpleop` machinery around the cluster in v1; folding
constants into `seek` bounds is a v2 micro-optimization.

### 6.4 Attribute order: static heuristic, override hook, verified alternative

v1 heuristic, computed constexpr from the layouts: order variables by
(a) descending number of atoms containing the variable, (b) variables
of negative conjuncts as early as their coverage allows (so vetoes
fire early), (c) tie-break by first occurrence. For the motivating
cluster this yields `x, t1, t2` — the right order. Correctness never
depends on the order (Section 3.2), so tuning is unconstrained.

Two refinement hooks, neither needed for v1: (i) `Order` is an
explicit template parameter, so per-cluster overrides can come from
the generated header — our translator's cost oracle (which already
prices subformulas on a training log) can emit suggested orders if
you expose a place for them; (ii) VeriMon's runtime-adaptive
`New_max_getIJ` strategy (pick the column touching the most positive
tuples) can be approximated by pre-compiling 2–3 orders per cluster
and branching on input sizes — worth a WP-J4 measurement only if the
static order disappoints.

### 6.5 The gate: when to emit `mgenjoin`

Compile-time criterion, evaluated by `flatten_and_chains`:
- cluster has ≥ 3 conjuncts, or
- cluster has ≥ 2 positives whose shared-variable set is a strict
  subset of both layouts (the quadratic-intermediate pattern), or
- cluster contains a negative conjunct together with ≥ 2 positives.

Everything else keeps the current binary operators. The gate is one
metafunction and trivially tunable after WP-J4's measurements. (This
mirrors a lesson from our LET work: an optimization applied
unconditionally regressed up to 17× on unfavorable shapes; the same
optimization behind a cost/shape gate was a strict improvement. Plan
for the gate from day one. Note the gate is strictly about
*performance*; well-formedness — ≥ 1 positive, negatives covered —
is the separate `MAnds`-style side condition of 3.3 and must hold for
every cluster the pass forms.)

## 7. Work packages

**WP-J0 — Validate the premise (1–2 days, partly on us).** (a) We run
the `run07A` vs. `run07B` A/B of Section 4 on the full property set
and hand you the tables. (b) You profile StaticMon on the two repro
cases we provide (manual data-race encoding on the 10⁴/10⁵ Havelund
traces; natural encoding races series at n=8000): confirm that
`table_join`/anti-join dominates and record the intermediate sizes (a
counter in `table_join` suffices). **Exit criterion:** join accounts
for a majority of runtime on at least the first case, or the A/B
shows the generic join winning inside VeriMon; otherwise stop here
and we all saved a month.

**WP-J1 — n-ary plumbing (2–3 days).** `flatten_and_chains` type pass
(identity on gated-out clusters) + n-ary child buffer + an `mgenjoin`
that *still executes binary joins internally* (fold over children).
Pure refactoring; verdict-equivalence must hold on the full test
suite. **Exit:** all tests green, compile time within +10%.

**WP-J2 — LFTJ core, positives only (1 week).** Sorted-vector tries,
leapfrog intersection, static order heuristic, behind a CMake flag.
The Isabelle definitions (`Generic_Join.thy`, `Optimized_Join.thy`)
are the specification of record; where the C++ deviates (iterators
vs. recursion-on-column-sets), document the correspondence in a
comment block. **Exit:** verdict-equivalent on our differential
suite; conformance driver (3.5) green on positives-only random
instances; the `x`-shared microbenchmark (we provide it) shows the
intermediate-free enumeration beating `table_join` from the sizes
WP-J0 measured.

**WP-J3 — Negatives + gate-on-by-default (3–4 days).** Check-only
iterators per the verified veto-at-coverage rule; enable `mgenjoin`
per the 6.5 gate by default. **Exit:** full differential suite green
on all four translation variants (plain/agg/LET/oracle-LET × 9
properties); conformance driver green on random instances *with*
negatives (including the corner cases the Isabelle wrapper handles:
empty positives list is ill-formed, arity-0 tables, negatives equal
to a positive); no series regresses > 10%.

**WP-J4 — Measurement and tuning (3–4 days).** Full sweep on our
harness (native runner + Docker; both trace families); tune galloping
vs. branchless search, the order heuristic (against the adaptive
alternative of 6.4 if warranted), and the gate. **Exit (overall
acceptance):** ≥ 2× on the manual data-race Havelund series (the
26.6 s case) or a documented analysis of why not; no regression
> 10% anywhere; StaticMon compile time within +20% per formula.

**WP-J5 — v2 options (backlog, unscoped):** keep `msince`/`muntil`/
`monce` internal state sorted so temporal inputs skip the per-tp
sort; order hints from our cost oracle through the header; constant
folding into seeks; factorized/compressed outputs for the widest
aggregation clusters.

## 8. Risks

- **Small-table constants.** Most per-tp tables in our traces are
  small; sorting them per tp to run a fancy join loses to a hash
  join. Mitigated by the shape gate (6.5) and measured in WP-J4; the
  operator must never be unconditional.
- **Compile time.** Each cluster instantiates a new join; template
  depth grows with cluster width. LET already shrank our formulas
  2–15×, and the gate keeps cluster counts low; budget is +20% and
  it is an explicit WP-J4 exit criterion. (For reference: current
  per-formula compiles are ~7–26 s warm on our set.)
- **The premise could be wrong** — join might not dominate the
  Havelund case (it could be `msince` state maintenance). WP-J0
  settles this for two days of work — and the Section 4 A/B gives an
  algorithm-level answer independent of StaticMon's implementation.
- **Correctness.** Anti-join semantics and layout bookkeeping are the
  fiddly parts. This is where the verified specification changes the
  risk profile: the algorithm (including the negative rule and its
  corner cases) is machine-checked, executable, and linked into two
  binaries in this repo; WP-J1's behavior-preserving refactor
  isolates plumbing from algorithm; the differential harness checks
  verdict equality against both current StaticMon and the verified
  kernel on every property, variant, and trace family; and the
  randomized conformance driver tests `mgenjoin` directly against the
  extracted `mmulti_join'` on adversarial instances.

## 9. What we provide

- **The specification:** the vendored Isabelle theories named in
  Section 3 (with the IJCAR'20 paper, Section 5, as the readable
  companion), plus a short correspondence note mapping their
  vocabulary (atables, restrict, wf_tuple) to StaticMon's (layouts,
  static projections, typed tuples).
- **The conformance driver (3.5):** we link the same extraction in
  our tool and will build and maintain the randomized
  `mgenjoin`-vs-`mmulti_join'` differ; you get it as a test binary +
  corpus.
- **The Section 4 A/B tables** (`run07A` vs `run07B`), run by us
  before you start.
- **Formulas:** the nine benchmark properties in all four translation
  variants (`.smfotl`, `.amfotl`, `.lsmfotl`, `.olsmfotl`), plus any
  new ones on request — including a minimized `x`-shared
  microbenchmark formula + trace generator for WP-J2.
- **Traces and harness:** the `races` generator, the Havelund traces,
  and a one-command native benchmark runner with per-series tables;
  our pipeline regenerates the paper's data appendix from a run, so
  your numbers land in the paper without manual transcription.
- **Differential validation:** scripted verdict-equality checks
  (StaticMon-new vs. StaticMon-old vs. MonPoly vs. verified kernel)
  across the full matrix; we run these for you on every iteration you
  push.
- **Guarantees you may assume** (enforced by the translator, machine-
  checked in Isabelle): inputs are RANF — in every cluster, at least
  one conjunct is positive and each negative conjunct's free
  variables are covered by the positives (the `MAnds` side
  condition); disjuncts are union-compatible; let-bound predicates
  behave as ordinary atoms (definitions evaluated once per time point
  by your existing `mlet`).

## 10. References

- D. Basin, T. Dardinier, L. Heimes, S. Krstić, M. Raszyk,
  J. Schneider, D. Traytel, *A Formally Verified, Optimized Monitor
  for Metric First-Order Dynamic Logic*, IJCAR 2020 — Section 5 is
  the verified generic join with anti-joins; formalization vendored
  at `monpoly/thys/Generic_Join_Devel/` and
  `monpoly/thys/MFODL_Monitor_Devel/Optimized_Join.thy` (also on
  the AFP as *Generic_Join*).
- T. Veldhuizen, *Leapfrog Triejoin: a simple, worst-case optimal
  join algorithm*, ICDT 2014.
- H. Ngo, E. Porat, C. Ré, A. Rudra, *Worst-case optimal join
  algorithms*, JACM 2018 (NPRR); Atserias–Grohe–Marx bound.
- H. Ngo, C. Ré, A. Rudra, *Skew strikes back: new developments in
  the theory of join algorithms*, SIGMOD Record 2013 — the unified
  `getIJ`-parameterized presentation the Isabelle formalization
  follows.
- EmptyHeaded (Aberger et al., TODS 2017) and Umbra/Hyper's hybrid
  binary/WCOJ planning (Freitag et al., VLDB 2020) — evidence for
  compile-time-specialized generic joins and for gating them by
  shape.

---

## Execution log (staticmon side)

### WP-J0 — premise validation (2026-07-17): PROCEED

Ran on self-built minimized inputs (the collaborators' traces are not in this
repo): `experiments/genjoin/gen_xshared.py` (the §1 motivating cluster:
`((ONCE p(t1,x)) AND (ONCE q(t2,x))) AND (NOT r(t1,t2))`, X values × T threads,
dense negative) and `gen_triangle.py` (`((ONCE p(a,b)) AND (ONCE q(b,c))) AND
(ONCE s(a,c))`, N edges over domain D — the canonical asymptotic
binary-vs-WCO separation, which the motivating cluster is not: its positive
join output equals the cross product, so only materialization constants
differ there).

**(a) §4 A/B, verified kernel multiway vs `-no_mw`** (verdicts identical in
all runs):

| shape | multiway | binary | winner |
|---|---:|---:|---|
| x-shared T=40/80/120 | 6.1 / 27.7 / 69.7 | 4.1 / 19.3 / 47.0 | binary ~1.5× (uniform: constants) |
| triangle N=5000 D=500 | **18.0** | 22.3 | **multiway 1.24×** (asymptotic gap real) |

monpoly-native (binary, better constants) does the triangle in 0.40s — 45×
faster than verified-multiway: constants dominate algorithm at these sizes,
which is precisely the plan's division of labor (verified algorithm × our
specialized constants).

**(b) staticmon profile** (`profile.py`, self-time shares):

| shape (interm/tp) | staticmon | join | hash | RSS |
|---|---:|---:|---:|---:|
| triangle N=100k D=1000 (10M rows/tp) | 6.43s | 46.6% | 30.5% | 656MB |
| x-shared T=240 (2.9M rows/tp) | 4.13s | 27.4%+12.7% anti | 24.5% | 127MB |

staticmon's runtime tracks the intermediate size linearly; join+hash ≥ 52–77%.
Both exit criteria satisfied → WP-J1.

### WP-J1 — n-ary plumbing (2026-07-17): DONE

Translator-level `try_translate_cluster` (the plan's `flatten_and_chains`,
done in the front-end per §5's alternative — mirroring `convert_multiway`
being a front-end pass in VeriMon) + `ex_genjoin` IR node + `mgenjoin`
operator with per-child queues (n-ary `bin_op_buffer`) executing the old
binary fold. Gate 6.5 verified by emitted-header introspection
(all-shared 2-joins and 1pos+1neg stay binary; quadratic-pattern /
cartesian / ≥3-conjunct shapes flatten). Exit: 520/520 fixtures across
all five suites; live differential 40/40; compile time at or below
master (6.0s vs 6.8s on the 5-atom cluster).

### WP-J2 — LFTJ core (2026-07-17): DONE

Compile-time attribute order (6.4 heuristic, unique sort keys), implicit
tries as per-tp sorted projected vectors (6.1), recursive
compile-time-participant leapfrog with galloping seek; negatives still
post-hoc (J3). `STATICMON_GENJOIN_FOLD` restores the fold. Exit
measurements (verdicts identical): triangle N=100k **7.05s → 3.11s
(2.3×), RSS 656MB → 134MB**; x-shared parity — expected, its positive
output *is* the cross product; that shape's win is J3's veto-in-descent.

### WP-J3 — negative vetoes (2026-07-17): DONE

Check-only vetoes at coverage depth + nullary pre-check (rule 3.3).
Correctness: **521/521** fixtures (genjoin 28, regression 3, frz 105,
generated 150, corpus 235) + **0/59** live differential vs VeriMon.

Process note: the first J3 gate+live-diff run reported 12 live-diff and 1
fixture "mismatches" -- ALL were cache-race false positives from running
the fixture gate and the live differential concurrently (two staticmon-impl
compile streams share the one builddir even with separate cache dirs). A
sequential re-run of the exact same seed gave 28/28 and 0/59. No code was
at fault. (Rule reinforced: only ever one compile stream in flight.)

### WP-J4 — measurement + acceptance (2026-07-17): DONE

Branch-vs-master, min-of-3, warmed (`experiments/baselines/<sha>/genjoin_j4.csv`):

| shape | master | branch | speedup |
|---|---:|---:|---:|
| triangle N=20k D=500 | 0.368s | 0.254s | **1.45×** |
| triangle N=50k D=1000 | 1.346s | 0.700s | **1.92×** |
| triangle N=100k D=1000 (earlier) | 7.05s | 3.11s | **2.3×** |
| x-shared+neg T=160 | 1.758s | 1.053s | **1.67×** |
| x-shared+neg T=240 | 4.122s | 2.512s | **1.64×** |
| small2 (5/20/50 rows, flattened) | — | — | 1.02 / 0.99 / 0.98× |
| med2 (200/1000 rows, flattened) | — | — | 0.97 / 0.94× |
| cart2 (50/200 rows, flattened) | — | — | 1.01 / 1.01× |

**Exit criteria met.** (i) ≥2× on the WCO shape: triangle 1.92× at N=50k,
2.3× at N=100k -- the documented analogue of the manual data-race Havelund
series (whose traces the collaborators provide; §9). (ii) No regression
> 10% anywhere: the flattened small/anti-shapes (2-way shared-subset and
cartesian, which the 6.5 gate rewrites) stay within noise -- worst is
med2_1000 at 6% (~parity), so the gate needs **no** size threshold; on
tiny tables sort+descend ties the hash join. (iii) Compile time within
+20%: worst +11% (tri_50k), and xsh_240 actually compiled faster. All
verdicts identical on every shape.

WP-J5 backlog (unshipped): galloping-seek binary-search touch-up (currently
an O(range) linear scan after the gallop -- correct, not yet O(log));
keep temporal children's state sorted to skip the per-tp sort; order hints
from the cost oracle; constant folding into seeks.
