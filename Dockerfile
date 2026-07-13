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
#   - this repository, configured and compiled once (Clang 14 + ld.lld +
#     Ninja; dependencies resolved by Conan v1 at build time), so that a
#     per-formula rebuild recompiles a single translation unit and relinks;
#   - the native front-end `staticmon_compile` (built from this repo) which
#     turns a monitorable formula + signature into the two C++ headers that
#     instantiate StaticMon's templates -- no OCaml / MonPoly dependency;
#   - the entrypoint script (docker/staticmon-entrypoint.sh) wiring them
#     together over the /work mount.
#
# Notes:
#   - The Conan profile (Clang 14, matching upstream's setup.sh answers
#     "Clang / 14 / release") derives its `arch` from the build host, so a plain
#     `docker build` compiles natively -- fast on any host, including Apple
#     Silicon. Pass `--platform linux/amd64` only when you want a portable
#     x86-64 image (built under qemu on arm64; timings then aren't
#     representative).
#   - check_ipo_supported() is gated behind ENABLE_IPO in CMakeLists.txt, so
#     configuring also works on toolchains without LTO support.

FROM ubuntu:22.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install \
      --no-install-recommends -y \
      ca-certificates git clang lld ninja-build cmake make \
      python3 python3-pip libgmp10 \
    && rm -rf /var/lib/apt/lists/*
RUN pip3 install --no-cache-dir conan
# link through ld.lld (Clang LTO objects; GCC cannot be mixed in)
RUN ln -sf /usr/bin/ld.lld /usr/local/bin/ld

COPY . /opt/staticmon
WORKDIR /opt/staticmon

# A minimal Conan 2 profile matching ./setup.sh's "Clang / 14 / release". The
# arch is taken from the build platform (dpkg reports amd64/arm64, which tracks
# `--platform` when set and the host otherwise), so the image builds natively
# unless a specific --platform is requested. (The monitor's optimization flags
# come from CMakeLists' IPO, not the profile, so no custom CFLAGS are needed;
# -march=native is intentionally omitted -- Clang 14 cannot resolve it for
# aarch64 in Docker's VM.)
RUN case "$(dpkg --print-architecture)" in \
      amd64) CONAN_ARCH=x86_64 ;; \
      arm64) CONAN_ARCH=armv8 ;; \
      *) echo "unsupported build arch: $(dpkg --print-architecture)" >&2; exit 1 ;; \
    esac && \
    printf '%s\n' \
      '[settings]' 'os=Linux' "arch=$CONAN_ARCH" 'compiler=clang' \
      'compiler.libcxx=libstdc++11' 'compiler.cppstd=20' 'compiler.version=14' \
      'build_type=Release' \
      > profile
# conan resolves boost/abseil/fmt/jemalloc from conancenter and cmake fetches
# lexy via FetchContent: this step needs network access at image build time
RUN conan install . --output-folder=builddir --build=missing -pr:h=profile -pr:b=profile && \
    CXX=clang++ CC=clang cmake -G Ninja -DUSE_JEMALLOC=ON \
      -DCMAKE_TOOLCHAIN_FILE="$PWD/builddir/conan_toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Release -S . -B builddir

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
