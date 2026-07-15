# Staticmon


Staticmon is an online monitor for Metric First-Order Temporal Logic (MFOTL) 
formulas. It compiles an **optimized binary for each formula**. A signature + 
MFOTL formula is preprocessed into two header (`.h`) files that describe the 
formula; these instantiate the C++ template monitor, producing a specialized 
monitor via template metaprogramming.

The preprocessing (parse, type-check, monitorability check, codegen) is done by
`staticmon-headers`, Staticmon's own dependency-free MFOTL front-end — no OCaml
or MonPoly install is needed to build or run a monitor.

You drive everything through one command, **`staticmon`**, which behaves like a
normal monitor (à la MonPoly): hand it a signature and formula and it compiles a
specialized monitor the first time, caches it, and monitors your trace. It runs
the same whether backed by a **native** build tree or a **self-contained Docker
image** — both are described below.

## Requirements

### Native

- OS: Linux or macOS (including Apple Silicon).
- Compiler: GCC ≥ 11, Clang ≥ 13, or Apple clang.
- C++ standard library: static libstdc++ on Linux, or libc++ on macOS.
- [Conan](https://conan.io) 2, CMake ≥ 3.22, Ninja.

`./setup.sh` detects the host toolchain. The correctness tests need only Python 3
— the fixtures are committed, so **no MonPoly runs at test time**. The *live*
randomized test additionally needs a user-provided VeriMon (`monpoly -verified`,
via `$STATICMON_VERIMON`); regenerating fixtures needs MonPoly.

### Docker

- Docker.
- Docker Desktop: enable file sharing for your working directory — the current
  directory is bind-mounted at `/work` so the image can read your
  signature/formula/trace. (The per-formula binary cache lives in a Docker-managed
  named volume, not in your directory.)
- On Apple Silicon the image builds natively (arm64). Pass
  `--platform linux/amd64` for a portable x86-64 image (built under qemu).

No compiler, Conan, or Python are needed on the host for the Docker path — the
image bundles the whole toolchain.

## Building

### Native

```
./setup.sh                                        # pick compiler + build mode
./configure.sh                                    # Conan installs deps; CMake configures builddir/
cmake --build builddir --target staticmon-headers # build the front-end
```

That gives you a configured build tree and the front-end; `staticmon` compiles
the per-formula monitor on demand from here (see [Use](#use)). Optionally put
`staticmon` on your PATH — a thin launcher that uses this build tree:

```
cmake --install builddir --prefix ~/.local        # -> ~/.local/bin/staticmon
```

The front-end is also buildable on its own, without Conan/CMake:

```
clang++ -std=c++20 -I src -o staticmon-headers src/tools/staticmon-headers.cpp
```

### Docker

```
docker build -t staticmon .                          # native to the host (fast)
docker build --platform linux/amd64 -t staticmon .   # portable x86-64 image
```

The image is self-contained (ubuntu 24.04 + Clang 19, Release + LTO + jemalloc).
To put a Docker-backed `staticmon` on your PATH (builds the image if needed):

```
scripts/install-docker.sh                            # -> ~/.local/bin/staticmon
```

No image is published to a registry yet; if one is, `docker pull` it instead.

## Use

If you installed `staticmon` on your PATH, use `staticmon`; otherwise run
`scripts/staticmon` from the repo. Either way the launcher prefers the native
build tree and falls back to the Docker image (`STATICMON_MODE=native|docker`
overrides).

Formula syntax is MonPoly's; see [`docs/monpoly-grammar.md`](docs/monpoly-grammar.md).
A signature declares predicate argument types, e.g. `p(int) q(int,string)`.

### Monitor a trace

```
staticmon -sig bla.sig -formula bla.mfotl -log trace.log
cat trace.log | staticmon -sig bla.sig -formula bla.mfotl     # or stream on stdin
```

The trace is in MonPoly format — one timepoint per line, each terminated by `;`:

```
@0 p(1) q(1,"a");
@2 p(3);
```

Verdicts print as `@<ts> (time point <tp>): (<tuple>) ...`, one line per
satisfied timepoint. Redirect them with `-verdicts FILE`; read events from a
Unix socket instead of a trace with `-socket [PATH]`.

The first run for a formula compiles the monitor (a few seconds); every later
run is an instant cache hit. Binaries are cached under `~/.cache/staticmon`
(override with `$STATICMON_CACHE`), or, in Docker, in a `staticmon-cache` volume
— keyed by the formula and **namespaced by the toolchain fingerprint**, so a
compiler/dependency/monitor-source change never serves a stale binary.

### Compile once, run many

To keep the monitor binary yourself:

```
staticmon compile -sig bla.sig -formula bla.mfotl -keep bla_monitor
staticmon run -monitor bla_monitor -log trace.log
```

### Inspect a formula

```
staticmon info -sig bla.sig -formula bla.mfotl   # free-variable types + monitorability
```

Non-monitorable, ill-typed, malformed-interval and unbounded-future formulas are
rejected with a MonPoly-compatible message (exit code 1).

## Testing

Build the test tools, then run via `ctest` **against the build directory** — from
the repo root pass `--test-dir builddir` (or `cd builddir` first); a bare `ctest`
in the repo root finds nothing:

```
cmake --build builddir --target staticmon-headers parser_dump
ctest --test-dir builddir                # everything
ctest --test-dir builddir -L fast        # parser + frontend (instant)
ctest --test-dir builddir -L monitor     # compiled-monitor verdicts (compiles per formula)
STATICMON_VERIMON=/path/to/monpoly \
  ctest --test-dir builddir -L live      # live randomized diff vs a user-provided VeriMon
```

A test whose prerequisite is missing (Python 3; a configured native build or
docker; `$STATICMON_VERIMON`) is reported **Skipped**, not failed.

### Layout

Tests live under `test/<module>/{components,methods}`, organized by *what* is
tested (`parser`, `frontend`, `monitor`) and *how* (`fixture` = replay committed
golden data; `live` = run an oracle now). `components/` holds the reusable parts
(generator, oracle, comparator, coverage). Fixture methods replay committed
outputs, so no MonPoly runs at test time — each method's `regen*.sh` regenerates
its fixtures offline (the only place the oracle runs).

### Correctness

- `parser`     *(fast)*  — the C++ parser vs the MonPoly parser (canonical ASTs).
- `frontend`   *(fast)*  — `staticmon-headers` typing/monitorability vs
                           `monpoly -sigout` / `monpoly -check`.
- `monitor_generated`, `monitor_corpus` *(monitor)* — compiled-monitor verdicts
   vs VeriMon (`monpoly -verified`), on random formulas and on MonPoly's own test
   corpus respectively.
- `monitor_live` *(live)* — a live randomized differential test: generates random
   formulas + traces, compares staticmon to a user-provided VeriMon
   (`$STATICMON_VERIMON`), and reports structural coverage of the fragment.

Monitor compilation is **native-first, docker fallback**: the `monitor` and
`live` suites compile each formula with the native build tree when one is
configured, else in a `staticmon-bench` container that `ctest` builds and starts
automatically (the `bench_up`/`bench_down` fixtures — a no-op when native).
Force a mode with `STATICMON_TEST_MODE=native|docker` (default `auto`).

### Performance

- `experiments/runner/` — a Haskell/cabal benchmark harness that generates
  traces and times monitors.
- `docker/behavioral.Dockerfile` can build **optimized** variants (Release + LTO
  + jemalloc + host-native codegen) and select the compiler via build args
  (`BASE`, `APT_COMPILER`, `CC`/`CXX`, `CONAN_COMPILER[_VERSION]`, `BUILD_TYPE`,
  `ENABLE_IPO`, `USE_JEMALLOC`, `EXTRA_CXXFLAGS`), for compiler/flag A/B testing.

## Notes

- The deployment Docker image uses Clang 19: in a controlled A/B it produced
  ~8.7% faster runtime than Clang 14 (and than GCC 14) on the tested monitor,
  at ~11% slower compilation — a good trade, since per-formula compiles are
  cached and runtime is recurring.
- Debug builds can be several orders of magnitude slower to run than release
  builds.
- The Ninja compile step can produce very long template warnings.
- Traces are read in MonPoly format: `@<ts>` time-points delimited by `;` or by
  the next `@` (so the trailing `;` is optional), one or more per line, `@`/`;`
  inside quoted string arguments are respected. A malformed trace fails with a
  parse error, and a missing/unreadable log with `cannot open log file …` — both
  a non-zero exit, never a silent empty output or a crash.
- Not yet supported by the front-end: regex operators (`MATCHF`/`MATCHP`),
  `FRZ`, `SUBSTRING`/`MATCHES`, and the string/date conversions (`i2s`, `s2i`,
  `f2s`, `s2f`, `r2s`, `s2r`, `DAY_OF_MONTH`, `MONTH`, `YEAR`, `FORMAT_DATE`).
  `LET`/`LETPAST` and all aggregations (`CNT`/`SUM`/`MIN`/`MAX`/`AVG`/`MED`)
  are supported.
- The native monitorability check does not yet apply MonPoly's full rewriting
  (`rr`) pass, so it can be slightly more conservative than `monpoly -check`
  (sound, never unsound).
