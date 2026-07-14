# staticmon

Similar to [Cppmon](https://github.com/matthieugras/cppmon), but compiles an
**optimized monitor for each formula**. A signature + MFOTL formula is
preprocessed into two header files (`formula_in.h`, `formula_csts.h`) that
describe the formula; these instantiate the C++ template monitor in
`src/staticmon/`, producing a specialized monitor via template metaprogramming.

The preprocessing (parse, type-check, monitorability check, codegen) is done by
`staticmon_compile`, staticmon's own dependency-free MFOTL front-end — no OCaml
or MonPoly install is needed to build or run a monitor.

There are two ways to work with staticmon: a **native** build tree, or a
**self-contained Docker image**. Both are described below.

## Requirements

### Native

- OS: Linux or macOS (including Apple Silicon).
- Compiler: GCC ≥ 11, Clang ≥ 13, or Apple clang.
- C++ standard library: static libstdc++ on Linux, or libc++ on macOS.
- [Conan](https://conan.io) 2, CMake ≥ 3.22, Ninja.

`./setup.sh` detects the host toolchain. Running the correctness tests
additionally needs a native MonPoly (the `monpoly` binary, used as the oracle)
and Python 3.

### Docker

- Docker.
- Docker Desktop: enable file sharing for your working directory — the image
  reads the signature/formula/log and writes the monitor binary through a
  `-v "$PWD":/work` bind mount.
- On Apple Silicon the image builds natively (arm64). Pass
  `--platform linux/amd64` for a portable x86-64 image (built under qemu).

No compiler, Conan, or Python are needed on the host for the Docker path — the
image bundles the whole toolchain.

## Building

### Native

```
./setup.sh                              # pick compiler and build mode
./configure.sh                          # Conan installs deps, CMake configures builddir/
ninja -C builddir bin/staticmon_compile # build the MFOTL front-end
```

You now have `builddir/bin/staticmon_compile` and a configured build tree.
Per-formula monitors are built from here in [Use](#use).

The front-end is dependency-free and can also be built on its own, without
Conan/CMake:

```
clang++ -std=c++20 -I src -o staticmon_compile src/tools/staticmon_compile.cpp
```

### Docker

```
docker build -t staticmon .                          # native to the host (fast)
docker build --platform linux/amd64 -t staticmon .   # portable x86-64 image
```

The image is self-contained (ubuntu 24.04 + Clang 19, Release + LTO + jemalloc).
No image is published to a registry yet; if one is, `docker pull` it instead.

## Use

A staticmon monitor is specialized to a single `(signature, formula)` pair.
So you first **compile the formula** into a monitor binary, then **monitor a
log** with it.

The formula syntax is MonPoly's; see [`docs/monpoly-grammar.md`](docs/monpoly-grammar.md).
A signature declares the predicates and their argument types, e.g.
`p(int) q(int,string)`.

### Compile the formula

**Native.** `scripts/staticmon-build` is the one-shot interface: it generates
the headers with `staticmon_compile`, compiles the per-formula monitor, and
caches the binary by a hash of the headers — so recompiling the same formula is
instant.

```
scripts/staticmon-build -sig bla.sig -formula bla.mfotl -o bla_monitor
# [compiled] 2eed8a2b...     (first time; runs ninja)
scripts/staticmon-build -sig bla.sig -formula bla.mfotl -o bla_monitor
# [cache hit] 2eed8a2b...    (instant)
```

Useful options: `--builddir DIR` (configured tree, default `builddir`),
`--cache-dir DIR` (default `$HOME/.cache/staticmon`, or `$STATICMON_CACHE`),
`--no-cache` to force a recompile. It exits non-zero and prints the reason if
the formula is not monitorable or is ill-typed.

**Docker.** The `compile` subcommand builds `./<formula>_staticmon` in the
mounted directory (also cached, by an md5 of the inputs):

```
docker run --rm -v "$PWD":/work staticmon compile -sig bla.sig -formula bla.mfotl
# -> ./bla_staticmon
```

Two `staticmon_compile` modes are handy for scripting or checking against
MonPoly:

```
staticmon_compile -sig bla.sig -formula bla.mfotl -check    # monitorability verdict
staticmon_compile -sig bla.sig -formula bla.mfotl -sigout   # free-variable types
```

### Monitor the log

The log is in MonPoly format: one timepoint per line, each terminated by `;`,
with a `@<timestamp>` prefix.

```
@0 p(1) q(1,"a");
@2 p(3);
```

**Native:**

```
./bla_monitor --log bla.log
```

**Docker** (pass a file, or stream on stdin):

```
docker run --rm -v "$PWD":/work staticmon run -monitor bla_staticmon -log bla.log
cat bla.log | docker run -i --rm -v "$PWD":/work staticmon run -monitor bla_staticmon
```

Verdicts print as `@<ts> (time point <tp>): (<tuple>) (<tuple>) ...`, one line
per satisfied timepoint.

## Testing

### Correctness

The front-end and the compiled monitors are differentially tested against the
newest MonPoly / VeriMon (need a native `monpoly` and Python 3):

- `test/parser_diff/`   — C++ parser vs the MonPoly parser (canonical ASTs).
- `test/pipeline_diff/` — `staticmon_compile` typing/monitorability vs
                          `monpoly -sigout` / `monpoly -check`.
- `test/behavioral/`    — compiled-monitor verdicts vs VeriMon (`monpoly -verified`)
                          on random formulas + traces.
- `test/monpoly_suite/` — replays MonPoly's own test corpus through staticmon and
                          compares verdicts to VeriMon (288/288 in-fragment cases match).

The `behavioral` and `monpoly_suite` harnesses compile each formula in a warm
`staticmon-bench` container built from `docker/behavioral.Dockerfile`, reusing
`scripts/staticmon-build` for cached per-formula builds.

### Performance

- `experiments/runner/` — a Haskell/cabal benchmark harness that generates
  traces and times monitors.
- `docker/behavioral.Dockerfile` can build **optimized** variants (Release +
  LTO + jemalloc + host-native codegen) and select the compiler via build args
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
- Not yet supported by the front-end: regex operators (`MATCHF`/`MATCHP`),
  `FRZ`, `SUBSTRING`/`MATCHES`, and the string/date conversions (`i2s`, `s2i`,
  `f2s`, `s2f`, `r2s`, `s2r`, `DAY_OF_MONTH`, `MONTH`, `YEAR`, `FORMAT_DATE`).
  `LET`/`LETPAST` and all aggregations (`CNT`/`SUM`/`MIN`/`MAX`/`AVG`/`MED`)
  are supported.
- The native monitorability check does not yet apply MonPoly's full rewriting
  (`rr`) pass, so it can be slightly more conservative than `monpoly -check`
  (sound, never unsound).
