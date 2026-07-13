# Native C++ reimplementation of the `-explicitmon` pipeline

Requirements + design for AGENT.md task *"Integration of the new parser into
the Staticmon pipeline"*: replace the whole `-explicitmon` pipeline (typing,
monitorability, exformula translation, C++ codegen) — currently OCaml in
`monpoly-exp` — with native C++ in staticmon. Oracle: the newest MonPoly
(`monpoly-develop` @ `fe89b7da`, installed natively as `monpoly`).

Status: **requirements/design** (no pipeline code yet). Parser stage already
done (`src/staticmon/parser/`, see docs/monpoly-grammar.md).

## 1. What the pipeline does today (OCaml)

`monpoly-exp/src/main.ml` (the `-explicitmon` path):

```
parse (formula_parser + log_parser signature)
  -> check_formula sign f      (rewriting.ml)  = (is_mon, pf, vartypes)
       = type inference/check  +  rewriting  +  monitorability
  -> if is_mon:  Explicitmon.write_explicitmon_state sign pf fv
       -> make_exformula: elim_syntactic_sugar pf; translate_formula; 
       -> cpp_of_exformula: emit formula_in.h + formula_csts.h
```

So the C++ replacement needs five stages (stage 1 done):

| # | Stage | OCaml source | C++ target (planned) | Primary oracle |
|---|-------|--------------|----------------------|----------------|
| 1 | Parse formula+signature | formula_parser.mly, log_parser.ml | `parser/` (done) | monpoly parser (done, >1M frames) |
| 2 | Type inference/check | rewriting.ml `check_syntax`/`type_check*` | `types/` | `monpoly -sigout` + type-error parity |
| 3 | Desugar + monitorability | rewriting.ml `elim_syntactic_sugar`, `is_monitorable` | `monitorability/` | `monpoly -check` |
| 4 | Translate to exformula IR | explicitmon.ml `translate_formula` | `translate/` | header diff vs `monpoly-exp -explicitmon` |
| 5 | C++ codegen | explicitmon.ml `cpp_of_exformula` | `codegen/` | header diff vs `monpoly-exp -explicitmon` |
| — | End-to-end | (whole monitor) | compiled `staticmon` | **`monpoly -log` verdicts (gold standard)** |

## 2. Oracle surface (validated against native `monpoly`)

- **Monitorability + rewritten formula** — `monpoly -sig S -formula F -check`:
  prints `The analyzed formula is:\n  <rewritten>`, `The sequence of free
  variables is: (x,...)`, then either `The analyzed formula is monitorable.`
  or `... is NOT monitorable, because of the subformula:\n  <sub>\n<reason>`.
  `-no_rw` prints the un-rewritten formula and skips rewriting.
- **Free-variable types** — `monpoly -sig S -formula F -sigout`: one line
  `var:type` per free variable (`x:int`, `s:string`, `f:float`).
- **Type errors** — thrown as `Fatal error: exception Failure("[Rewriting.
  type_check_term] Type check error on term x: expected type String, actual
  type Int")`, nonzero exit. Parity = C++ rejects iff monpoly throws.
- **Verdicts (behavioral, strongest)** — `monpoly -sig S -formula F -log T`:
  `@<ts> (time point <tp>): (<tuple>) (<tuple>) ...` per satisfied time
  point. End-to-end oracle: compile staticmon with generated headers, run on
  the same trace, compare verdict streams.
- **Reference headers (structural, for stages 4–5)** —
  `monpoly-exp -sig S -formula F -no_rw -explicitmon -explicitmon_prefix D`
  writes `D/formula_in.h` + `D/formula_csts.h`. NB monpoly-exp aborts
  ("abort because not monitorable") without writing headers for
  non-monitorable input, and its monitorability fragment may be narrower than
  monpoly-develop's — monpoly-develop is the semantic authority.

## 3. Stage 5 — C++ codegen contract (validated, from explicitmon.ml + the C++ operators)

`formula_in.h` = three declarations:
```cpp
using input_formula = <exformula rendered as a nested template type>;
using free_variables = mp_list_c<std::size_t, id, ...>;   // or mp_list<> if empty
inline static const pred_map_t input_predicates =
  {{"name", {pred_id, {INT_TYPE|STRING_TYPE|FLOAT_TYPE, ...}}}, ...};
```
`formula_csts.h` = one struct per string/float constant referenced above:
```cpp
struct string_cst_N { using value_type = std::string_view;
                      static constexpr std::string_view value = "..."sv; };
struct float_cst_N  { using value_type = double;
                      static constexpr double value = <ocaml Float.to_string>; };
```

exformula → template rendering (`print_exformula` and helpers). Template names
are the fixed backend contract (headers in `src/staticmon/operators/detail/`):

| exformula node | rendered template |
|---|---|
| `MPredicate(id, args)` | `mpredicate<id, arg...>` |
| `MTp a` / `MTs a` / `MTpts a b` | `mtp<a>` / `mts<a>` / `mtpts<a,b>` |
| predarg `PVar(ty,id)` / `PCst c` | `pvar<ctype(ty), id>` / `pcst<c>` |
| `MAnd(NatJoin,f1,f2)` / `MAnd(AntiJoin,..)` | `mand<false,f1,f2>` / `mand<true,..>` |
| `MOr(f1,f2)` `MNeg f` `MEq(t1,t2)` `MEmptyRel` | `mor<..>` `mneg<f>` `mequal<t1,t2>` `memptyrel` |
| `MPrev/MNext/MOnce/MEventually(intv,f)` | `mprev/mnext/monce/meventually<lb,ub,f>` |
| `MSince/MUntil(neg,intv,f1,f2)` | `msince/muntil<neg,lb,ub,f1,f2>` |
| `MOnceAgg(info,intv,f)` | `monceagg<res,op,agg,gvars,lb,ub,f>` |
| `MSinceAgg(info,neg,intv,f1,f2)` | `msinceagg<res,op,agg,gvars,neg,lb,ub,f1,f2>` |
| `MAggregation(info,f)` | `maggregation<res,op,agg,gvars,f>` |
| `MFusedSimpleOp(sops,f)` | `mfusedsimpleop<simpleops<sop...>,f>` |

Leaves: term `TVar id`→`tvar<id>`, `TCst c`→`tcst<c>`, `F2i/I2f`→`tf2i/ti2f<t>`,
`UMinus`→`tuminus<t>`, `Plus/Minus/Mult/Mod`→`tplus/tminus/tmult/tmod<t1,t2>`,
`Div`→**`tdiv<t1,t2>`** (see deviation D1). cst `Int i`→`mp_int64_t<i>`,
`Str`→`string_cst_N`, `Float`→`float_cst_N`. ctype: TInt→`std::int64_t`,
TStr→`std::string`, TFloat→`double`. arg_type: TInt→`INT_TYPE`,
TStr→`STRING_TYPE`, TFloat→`FLOAT_TYPE`. agg_op: Cnt/Min/Max/Sum/Avg→
`cnt/min/max/sum/avg_agg_op`. cst_type: `cst_eq/cst_less/cst_less_eq`.
interval bound `Bnd i`→`mp_size_t<i>`, `Inf`→`inf_bound`. var list: `[]`→
`mp_list<>`, else `mp_list_c<std::size_t, v...>`. sop: `MAndAssign(v,t)`→
`mandassign<v,t>`, `MAndRel(neg,ty,t1,t2)`→`mandrel<neg,cst_ty,t1,t2>`,
`MExists vs`→`mexists<v...>`.

## 4. Stage 4 — translation spec (from explicitmon.ml, validated)

- **IDs**: variables and predicates share one counter, assigned on first
  encounter during a specific pre-order traversal. `pred_id` keys on
  `(name, arity)`; `predarg` types come from the *signature*, not inference.
  (explicitmon.ml seeds the counter at 1 via a module-init `incr`, so real IDs
  start at 2 — a cosmetic quirk; correctness only needs consistent IDs. See D2.)
- **Special predicates**: `tp/1`→MTp, `ts/1`→MTs, `tpts/2`→MTpts.
- **Temporal**: `Since(Neg f1,f2)`→`MSince(true,..)`, else `false`; same Until.
  Interval bounds via `translate_intv`: lower `OBnd a`→`Bnd(a+1)`, upper
  `OBnd a`→`Bnd(a-1)`, `CBnd a`→`Bnd a`, `Inf`→`Inf`.
- **And**: `And(f1,Neg f2)`→`MAnd(AntiJoin,..)`; special-and (f2 is a fusable
  relop/assignment guarded by f1's free vars, `is_and_relop`+`is_special_case`)
  → fused op; otherwise `MAnd(NatJoin,..)`.
- **Fused ops** (`transform_fused_op`, accumulates a `simple_op list`):
  `Exists vs`→prepend `MExists`; `And(f1, Equal x y)` that `is_safe_assignment`
  →`MAndAssign`; other special-and →`MAndRel`(6 relop variants incl. negated);
  base →`MFusedSimpleOp(sops, translate f)`. sops list is outer-prepended
  (constraint before the enclosing exists — validated).
- **Aggregation**: translate body; if body is `MOnce`→`MOnceAgg`, if
  `MSince`→`MSinceAgg` (fusion), else `MAggregation`. `res_var` is a fresh var
  in the group-by-restricted context; `agg_var`/`gvars` looked up pre-restrict.
- **MEmptyRel**: `Neg(Equal(Var a,Var a))`.
- Desugared-away nodes (`elim_syntactic_sugar`, must not reach translate):
  `Implies`→`Or(Neg f1,f2)`; `Equiv`→`And(Or(Neg f1,f2),Or(f1,Neg f2))`;
  `ForAll v f`→`Neg(Exists(v,Neg f))`; `Always(i,f)`→`Neg(Eventually(i,Neg f))`;
  `PastAlways(i,f)`→`Neg(Once(i,Neg f))`; `Exists` prunes non-free vars and
  drops if none remain. Regex (`Frex/Prex`) is NOT handled by explicitmon
  (fails) — out of scope for the initial port (see D3).

## 5. Stage 3 — monitorability (from monpoly-exp; monpoly-develop is authority)

`is_monitorable` returns `bool * (formula*reason) option`. Key cases:
`Equal` monitorable iff Var/Cst mix (not two terms); `Less/LessEq/Substring/
Matches` alone not monitorable; `Neg(Equal(x,x))` and `Neg(Equal(Cst,Cst))`
ok; `Pred` iff all args are Var/Cst; `Neg f1` ok iff `fv(f1)=∅` (else must be
guarded by AND/SINCE/UNTIL); `And(f1,f2)`: f1 monitorable and (f2 a relop with
`is_special_case fv1`, or `f2=Neg f2'` with `fv2⊆fv1` and f2' monitorable, or
f2 monitorable with matching frees); `Or` needs equal free vars; temporal ops
recurse with their guardedness rules. **The C++ check must match
monpoly-develop's fragment** (validated via `-check`), not monpoly-exp's —
they may differ; differences to be catalogued during implementation.

## 6. Stage 2 — typing

MonPoly types: `tcst = TInt|TStr|TFloat|TRegexp`; symbolic type variables
`tsymb = TSymb(tcl,int) | TCst tcst` with classes `tcl = TNum|TAny`.
Signature predicate slots fix argument types; inference unifies term/variable
types across the formula (arithmetic ⇒ numeric, conversions fix in/out types,
comparisons unify their sides, aggregations constrain agg/result types).
Detailed rules: see the typing/monitorability requirements appendix (being
finalized from monpoly-develop `rewriting.ml`). Oracle: `-sigout` free-var
types + type-error parity. For codegen, per-slot predicate types (from the
signature) suffice for `pvar`/`pcst`/`pred_map`; full inference is needed to
(a) reject ill-typed formulas as monpoly does and (b) type comparison/arith
terms (int vs float vs string) inside fused constraints.

## 7. Proposed C++ architecture (SKILLS-Cpp: pure core, explicit types, oracles)

```
src/staticmon/compile/
  parser/            (existing, moved/aliased) formula + signature -> AST
  types/             typed_ast.h, type_check.h   AST+schema -> typed AST | error
  monitor_check/     desugar.h, monitorable.h    typed AST -> core AST | reason
  translate/         exformula.h, translate.h    core AST -> exformula IR (+id ctx)
  codegen/           emit_headers.h              exformula -> {formula_in,formula_csts}.h
  compile.h          driver stitching stages 1-5 -> Result<Headers, CompileError>
tools/
  staticmon_compile.cpp   CLI: -sig -formula -prefix (drop-in for -explicitmon)
test/pipeline_diff/
  header_diff: staticmon_compile vs monpoly-exp -explicitmon (stages 4-5)
  check_diff:  monitorability/types vs monpoly -check/-sigout (stages 2-3)
  behavior_diff: compiled staticmon verdicts vs monpoly -log (end-to-end)
```

Principles: exformula is a `std::variant` value IR (not templates — templates
are the *output text*); structured `CompileError` variant (parse/type/
not-monitorable/unsupported); reference `translate` mirrors explicitmon.ml
names for traceability; each stage independently testable against its oracle.

## 8. Deviations from monpoly-exp (intentional)

- **D1 `Div`**: explicitmon.ml maps `Div`→exformula `Mod`→template `tplus`
  (two bugs); the C++ backend has a correct `tdiv`. We emit `tdiv` and verify
  behaviorally against monpoly. (Header diff will differ on Div formulas — the
  harness will special-case/skip these against monpoly-exp and rely on the
  behavioral oracle.)
- **D2 ID base**: explicitmon starts IDs at 2 (module-init `incr`). We may
  start at 0/1; the header-diff harness renumbers IDs canonically before
  comparing, so this is invisible.
- **D3 Regex (MFODL) `Frex/Prex`, `LetPast`, `Frz`**: not translated by
  explicitmon (or unsupported per README). Out of scope for the initial port;
  parser already accepts them, pipeline will report `unsupported`.
- **D4 Semantic authority**: monitorability/typing follow monpoly-develop, not
  monpoly-exp, where the two differ.
```
