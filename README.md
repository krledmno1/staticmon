# staticmon

Similar to [Cppmon](https://github.com/matthieugras/cppmon), but compiles an
optimized monitor for each formula. A formula + signature is preprocessed into
two header files (`formula_in.h`, `formula_csts.h`) that describe the formula;
these instantiate the C++ template monitor in `src/staticmon/`, producing an
optimized monitor via template metaprogramming.

## Generating the formula headers

There are two ways to produce `src/staticmon/input_formula/{formula_in,formula_csts}.h`:

### Native (recommended): `staticmon_compile`

staticmon now ships its **own** MFOTL front-end in `src/staticmon/{parser,compile}/`,
so the header-generation step no longer depends on the OCaml
[modified MonPoly fork](https://github.com/matthieugras/monpoly). The
`staticmon_compile` tool parses the signature and formula, infers and checks
types, checks monitorability, and emits the two headers:

```
staticmon_compile -sig bla.sig -formula bla.mfotl -prefix ./src/staticmon/input_formula
```

It is functionally equivalent to the newest MonPoly's parser/typing/
monitorability plus the fork's `-explicitmon` codegen. Additional modes,
useful for scripting and for checking against MonPoly:

```
staticmon_compile -sig bla.sig -formula bla.mfotl -check    # print monitorability verdict
staticmon_compile -sig bla.sig -formula bla.mfotl -sigout   # print free-variable types
```

Ill-typed, non-monitorable, malformed-interval and unbounded-future formulas
are rejected with a MonPoly-compatible message. The MFOTL/signature grammar and
the full pipeline are documented in [`docs/monpoly-grammar.md`](docs/monpoly-grammar.md)
and [`docs/explicitmon-pipeline.md`](docs/explicitmon-pipeline.md).

`staticmon_compile` is built as part of the normal build (see below) and is
also available standalone (header-only, no external dependencies):

```
clang++ -std=c++20 -I src -o staticmon_compile src/tools/staticmon_compile.cpp
```

### Alternative: MonPoly `-explicitmon`

The upstream MonPoly `-explicitmon` fork
([matthieugras/monpoly](https://github.com/matthieugras/monpoly)) can also
produce the two headers, but is no longer required — `staticmon_compile` covers
the same fragment and then some (it fixes several `-explicitmon` codegen bugs
and adds MED and LET/LETPAST):

```
monpoly -sig bla.sig -formula bla.mfotl -explicitmon -explicitmon_prefix=./src/staticmon/input_formula
```

## Requirements
  - A C++ standard library: static libstdc++ on Linux, or libc++ on macOS
  - GCC >= 11, Clang >= 13, or Apple clang
  - Conan package manager (v2)
  - CMake >= 3.22
  - Ninja build tool

Builds natively on both Linux (libstdc++) and macOS/Apple Silicon (Apple
clang + libc++); `./setup.sh` detects the host toolchain.

## Compilation steps
1. Move to root of repository
2. `./setup.sh` and select compiler and build mode
3. `./configure.sh`
4. Generate the formula headers (see above), e.g.
   `staticmon_compile -sig bla.sig -formula bla.mfotl -prefix ./src/staticmon/input_formula`
5. `ninja -C builddir`
6. Resulting in binary `./builddir/bin/staticmon`
7. To run the monitor: `./builddir/bin/staticmon --log bla.log`

The log is in MonPoly format, one timepoint per line, each terminated by `;`:

```
@0 p(1) q(1,"a");
@2 p(3);
```

## Building a monitor, with caching

`scripts/staticmon-build` is the one-shot "compile a monitor for this formula"
interface. It generates the headers (`staticmon_compile`), then compiles the
per-formula monitor — **but only if a monitor for that exact formula is not
already cached**. Compiled binaries are cached by a hash of the generated
headers, so recompiling the same `(signature, formula)` is instant:

```
scripts/staticmon-build -sig bla.sig -formula bla.mfotl -o bla_monitor
# [compiled] 2eed8a2b...        (first time; runs ninja)
scripts/staticmon-build -sig bla.sig -formula bla.mfotl -o bla_monitor
# [cache hit] 2eed8a2b...       (instant; no compilation)
```

Options: `--cache-dir DIR` (default `$HOME/.cache/staticmon`, or `$STATICMON_CACHE`),
`--builddir DIR` (a configured cmake/ninja tree, default `builddir`),
`--no-cache` to force a recompile, and `--container NAME` to compile inside a
running Docker container built from `docker/behavioral.Dockerfile` (used when a
native build tree is unavailable). It exits non-zero and prints the reason if
the formula is not monitorable or ill-typed. The behavioral test harness
(`test/behavioral/`) uses this same script for its cached, per-formula builds.

## Docker

`docker/behavioral.Dockerfile` builds a warm, native-architecture build tree
that recompiles a single translation unit per formula; it is used by the
behavioral test harness (`test/behavioral/`). The standalone `Dockerfile`
bundles the monitor toolchain and `staticmon_compile` (self-contained; no
MonPoly needed).

## Testing

The front-end is differentially tested against the newest MonPoly:

  - `test/parser_diff/`     — C++ parser vs the MonPoly parser (canonical ASTs)
  - `test/pipeline_diff/`   — `staticmon_compile` typing/monitorability
                              vs `monpoly -sigout`/`-check`
  - `test/behavioral/`      — compiled monitor verdicts vs VeriMon (`monpoly -verified`)
  - `test/monpoly_suite/`   — replays MonPoly's own test corpus (`monpoly-develop/tests`)
                              through staticmon and compares verdicts to VeriMon
                              (288/288 in-fragment cases match)

## Notes
- Has some easy to fix performance regressions.
- Debug builds can be several orders of magnitude slower to run than release builds.
- GCC seems to generate better code.
- The compilation step with Ninja can produce very long compiler warnings.
- Regex operators (`MATCHF`/`MATCHP`), `FRZ`, `SUBSTRING`/`MATCHES`, and the
  string/date term conversions (`i2s`, `s2i`, `f2s`, `s2f`, `r2s`, `s2r`,
  `DAY_OF_MONTH`, `MONTH`, `YEAR`, `FORMAT_DATE`) are not yet supported by the
  native pipeline. `LET`/`LETPAST` and all aggregations (`CNT`/`SUM`/`MIN`/
  `MAX`/`AVG`/`MED`) are supported.
- The native monitorability check does not yet apply MonPoly's full rewriting
  (`rr`) pass, so it can be slightly more conservative than `monpoly -check`
  (sound, never unsound).
