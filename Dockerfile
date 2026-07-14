# Standalone StaticMon image: compile-per-formula monitoring on a mounted
# work directory, self-contained (no external MonPoly needed).
#
#   docker build -t staticmon .                          # native to the host (fast)
#   docker build --platform linux/amd64 -t staticmon .   # portable x86-64 image (qemu on arm64)
#
#   docker run --rm -v "$PWD":/work staticmon \
#     compile -sig formula.sig -formula formula.mfotl
#   docker run --rm -v "$PWD":/work staticmon \
#     run -monitor formula_staticmon -log trace.log
#   cat trace.log | docker run -i --rm -v "$PWD":/work staticmon \
#     run -monitor formula_staticmon
#
# The image contains:
#   - this repository, configured and compiled once (Clang 19 + ld.lld + Ninja;
#     dependencies resolved by Conan 2 at build time), so that a per-formula
#     rebuild recompiles a single translation unit and relinks;
#   - the native front-end `staticmon_compile` (built from this repo) which
#     turns a monitorable formula + signature into the two C++ headers that
#     instantiate StaticMon's templates -- no OCaml / MonPoly dependency;
#   - the entrypoint script (docker/staticmon-entrypoint.sh) wiring them
#     together over the /work mount.
#
# Notes:
#   - Toolchain: Clang 19 on ubuntu 24.04. A statistically-significant A/B
#     (docker/behavioral.Dockerfile variants) found Clang 19 ~8.7% faster at
#     runtime than Clang 14 on this monitor (and GCC 14 slower on both axes),
#     at the cost of ~11% slower per-formula compiles -- a worthwhile trade for
#     a deployment image, since compiles are one-time/cached and runtime is
#     recurring.
#   - Optimized for runtime: Release (-O3) + LTO (ENABLE_IPO, llvm-ar from the
#     llvm-19 package) + jemalloc + host-native codegen (-march=native on
#     x86-64, -mcpu=native on aarch64 -- Clang 19 resolves both in Docker's VM;
#     Clang 14 could not do -march=native on aarch64).
#   - The Conan profile derives its `arch` from the build platform (dpkg reports
#     amd64/arm64, tracking `--platform` when set and the host otherwise), so a
#     plain `docker build` compiles natively -- fast on any host, including
#     Apple Silicon. Pass `--platform linux/amd64` only for a portable x86-64
#     image (built under qemu on arm64; timings then aren't representative).

FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install \
      --no-install-recommends -y \
      ca-certificates git clang-19 lld-19 libstdc++-14-dev llvm-19 \
      ninja-build cmake make python3 python3-pip libgmp10 \
    && rm -rf /var/lib/apt/lists/*
# ubuntu 24.04's pip is PEP-668 externally-managed; allow the system install.
RUN pip3 install --no-cache-dir --break-system-packages conan
# Expose the versioned lld as the default `ld` (Clang LTO objects link via lld).
RUN lld_bin="$(ls /usr/bin/ld.lld-* /usr/bin/ld.lld 2>/dev/null | sort -V | tail -1)" && \
    ln -sf "$lld_bin" /usr/local/bin/ld && ln -sf "$lld_bin" /usr/bin/ld.lld
ENV CC=clang-19
ENV CXX=clang++-19

COPY . /opt/staticmon
WORKDIR /opt/staticmon

# Conan 2 profile (Clang 19 / release). arch tracks the build platform.
RUN case "$(dpkg --print-architecture)" in \
      amd64) CONAN_ARCH=x86_64 ;; \
      arm64) CONAN_ARCH=armv8 ;; \
      *) echo "unsupported build arch: $(dpkg --print-architecture)" >&2; exit 1 ;; \
    esac && \
    printf '%s\n' \
      '[settings]' 'os=Linux' "arch=$CONAN_ARCH" 'compiler=clang' \
      'compiler.libcxx=libstdc++11' 'compiler.cppstd=20' 'compiler.version=19' \
      'build_type=Release' \
      > profile
# conan resolves boost/abseil/fmt/jemalloc from conancenter and cmake fetches
# lexy via FetchContent: this step needs network access at image build time.
# Host-native codegen flag is x86-64 vs aarch64 specific.
RUN case "$(dpkg --print-architecture)" in \
      amd64) ARCH_FLAG='-march=native' ;; \
      arm64) ARCH_FLAG='-mcpu=native' ;; \
      *) ARCH_FLAG='' ;; \
    esac && \
    conan install . --output-folder=builddir --build=missing -pr:h=profile -pr:b=profile && \
    cmake -G Ninja -DUSE_JEMALLOC=ON -DENABLE_IPO=ON \
      -DCMAKE_TOOLCHAIN_FILE="$PWD/builddir/conan_toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="$ARCH_FLAG" \
      -S . -B builddir

# Build the header generator (a dependency-free translation unit that does not
# include the formula headers), prime the template monitor with a trivial
# formula (the repo ships only input_formula/formula.h, which #includes the two
# generated headers), then a per-formula rebuild only recompiles staticmon.cpp.
RUN ninja -C builddir bin/staticmon_compile && \
    mkdir -p src/staticmon/input_formula && \
    echo 'p(int)' > /tmp/smoke.sig && echo 'ONCE[0,10] p(x)' > /tmp/smoke.mfotl && \
    ./builddir/bin/staticmon_compile -sig /tmp/smoke.sig -formula /tmp/smoke.mfotl \
      -prefix "$PWD/src/staticmon/input_formula" && \
    ninja -C builddir && test -x builddir/bin/staticmon

COPY docker/staticmon-entrypoint.sh /usr/local/bin/staticmon-entrypoint
RUN chmod +x /usr/local/bin/staticmon-entrypoint

WORKDIR /work
VOLUME /work
ENTRYPOINT ["staticmon-entrypoint"]
