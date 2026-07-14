# Native-arch image for the behavioral end-to-end oracle: a warm StaticMon
# monitor build tree that recompiles ONE translation unit (staticmon.cpp) per
# formula. Headers are injected from the host (docker cp) by the harness, so
# this image does NOT need any MonPoly install. Built for the host architecture
# (no emulation) for fast per-formula compiles.
#
#   docker build -f docker/behavioral.Dockerfile -t staticmon-bench .
#   docker run -d --name smbench staticmon-bench sleep infinity
#   # then, per formula: docker cp formula_in.h smbench:.../ ; docker exec ... ninja
#
# The compiler is selectable at build time (default: ubuntu 22.04 + clang 14,
# the current shipping toolchain). To A/B a newer toolchain (step 4 of the
# modernization plan), override the build args -- clang 19:
#   docker build -f docker/behavioral.Dockerfile \
#     --build-arg BASE=ubuntu:24.04 \
#     --build-arg APT_COMPILER='clang-19 lld-19 libstdc++-14-dev' \
#     --build-arg CC=clang-19 --build-arg CXX=clang++-19 \
#     --build-arg CONAN_COMPILER_VERSION=19 \
#     -t staticmon-bench:clang19 .
# or gcc 14:
#   docker build -f docker/behavioral.Dockerfile \
#     --build-arg BASE=ubuntu:24.04 --build-arg APT_COMPILER='g++-14' \
#     --build-arg CC=gcc-14 --build-arg CXX=g++-14 \
#     --build-arg CONAN_COMPILER=gcc --build-arg CONAN_COMPILER_VERSION=14 \
#     -t staticmon-bench:gcc14 .
#
# Build tuned for COMPILE speed, not runtime: Debug (-O0, no LTO, no
# -march=native), jemalloc and the socket interface off. Traces are tiny
# (<=100 timepoints) so runtime is irrelevant.

ARG BASE=ubuntu:22.04
FROM ${BASE}

# Compiler selection (defaults = the current clang 14 toolchain).
ARG APT_COMPILER="clang lld"
ARG CC=clang
ARG CXX=clang++
ARG CONAN_COMPILER=clang
ARG CONAN_COMPILER_VERSION=14
# Build config. Defaults = Debug/-O0 (fast per-formula compile, for the
# behavioral oracle). Override for an OPTIMIZED runtime-benchmark image, e.g.
# BUILD_TYPE=Release ENABLE_IPO=ON USE_JEMALLOC=ON EXTRA_CXXFLAGS='-mcpu=native'.
ARG BUILD_TYPE=Debug
ARG ENABLE_IPO=OFF
ARG USE_JEMALLOC=OFF
ARG EXTRA_CXXFLAGS=""

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install \
      --no-install-recommends -y \
      ca-certificates git ninja-build cmake make \
      python3 python3-pip libgmp10 ${APT_COMPILER} \
    && rm -rf /var/lib/apt/lists/*
# conan via pip. ubuntu 24.04's pip is PEP-668 externally-managed, so allow the
# system install there; the fallback covers older pip (22.04) without the flag.
RUN pip3 install --no-cache-dir --break-system-packages conan \
      || pip3 install --no-cache-dir conan
# If an lld was installed (clang variants), expose it as `ld`/`ld.lld`; clang
# links through lld, gcc uses the default ld and ignores this.
RUN lld_bin="$(ls /usr/bin/ld.lld-* /usr/bin/ld.lld 2>/dev/null | sort -V | tail -1 || true)"; \
    if [ -n "$lld_bin" ]; then ln -sf "$lld_bin" /usr/local/bin/ld && ln -sf "$lld_bin" /usr/bin/ld.lld; fi

ENV CC=${CC}
ENV CXX=${CXX}
COPY . /opt/staticmon
WORKDIR /opt/staticmon

# Minimal Conan 2 profile (Linux, host arch via dpkg), Debug so dependency
# builds and the monitor both compile fast.
RUN case "$(dpkg --print-architecture)" in \
      amd64) CONAN_ARCH=x86_64 ;; \
      arm64) CONAN_ARCH=armv8 ;; \
      *) echo "unsupported build arch: $(dpkg --print-architecture)" >&2; exit 1 ;; \
    esac && \
    printf '%s\n' \
      '[settings]' 'os=Linux' "arch=$CONAN_ARCH" "compiler=${CONAN_COMPILER}" \
      'compiler.libcxx=libstdc++11' 'compiler.cppstd=20' "compiler.version=${CONAN_COMPILER_VERSION}" \
      "build_type=${BUILD_TYPE}" \
      > profile
RUN conan install . --output-folder=builddir --build=missing -pr:h=profile -pr:b=profile && \
    cmake -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$PWD/builddir/conan_toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DENABLE_IPO=${ENABLE_IPO} -DUSE_JEMALLOC=${USE_JEMALLOC} \
      -DCMAKE_CXX_FLAGS="${EXTRA_CXXFLAGS}" \
      -DENABLE_SOCK_INTF=OFF -DENABLE_FILE_INPUT=ON \
      -S . -B builddir

# Prime the template monitor once with a trivial formula so per-formula
# rebuilds only recompile staticmon.cpp and relink. The repo ships
# input_formula/formula.h which #includes the two generated headers.
# formula.h (the stable includer) is excluded by .dockerignore, so recreate
# it here along with priming headers. csts must precede formula_in (which
# references the constant structs).
RUN mkdir -p src/staticmon/input_formula && \
    printf '%s\n' \
      '#pragma once' \
      '#include <boost/mp11.hpp>' \
      '#include <staticmon/common/mp_helpers.h>' \
      '#include <staticmon/operators/operators.h>' \
      '#include <staticmon/common/table.h>' \
      '#include <string_view>' \
      'using namespace boost::mp11;' \
      'using namespace std::literals;' \
      '#include <staticmon/input_formula/formula_csts.h>' \
      '#include <staticmon/input_formula/formula_in.h>' \
      > src/staticmon/input_formula/formula.h && \
    printf 'using input_formula =\n  monce<mp_size_t<0>, mp_size_t<10>, mpredicate<2, pvar<std::int64_t, 1>>>;\nusing free_variables =\n  mp_list_c<std::size_t, 1>;\ninline static const pred_map_t input_predicates =\n  {{"p", {2, {INT_TYPE}}}};\n' \
      > src/staticmon/input_formula/formula_in.h && \
    printf '\n' > src/staticmon/input_formula/formula_csts.h && \
    ninja -C builddir && test -x builddir/bin/staticmon

CMD ["sleep", "infinity"]
