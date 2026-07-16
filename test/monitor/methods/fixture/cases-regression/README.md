# cases-regression

Hand-written monitor fixtures pinning bugs that the auto-generated `cases-*`
suites cannot cover, because `regen-*.sh` *drops* any formula staticmon fails to
compile (so a translation/codegen regression would silently shrink those suites
instead of failing). Cases here are committed directly and never regenerated.

Each `<case>/` has `sig`, `formula`, `trace`, and `expected` (monpoly -verified
verdicts). Replayed by `run.sh cases-regression` (ctest: `monitor_regression`).

- `agg_sibling__0` — `p(a) AND (b <- CNT ...)`: a variable from a sibling
  conjunct must survive an aggregation (the aggregation used to reset the whole
  variable environment, dropping it).
- `agg_exists__0` — `EXISTS b. (b <- CNT ...)`: an EXISTS binding an
  aggregation *result* (the result must reuse the quantifier's variable id).
- `lock_agg__0` — a full lock-discipline property (the reported formula) that
  hit both bugs at once.
