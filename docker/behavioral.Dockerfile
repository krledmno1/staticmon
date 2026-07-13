# Native-arch image for the behavioral end-to-end oracle: a warm StaticMon
# monitor build tree that recompiles ONE translation unit (staticmon.cpp) per
# formula. Headers are injected from the host (docker cp) by the harness, so
# this image does NOT need any MonPoly install. Built for the host architecture (no
# emulation) for fast per-formula compiles.
#
#   docker build -f docker/behavioral.Dockerfile -t staticmon-bench .
#   docker run -d --name smbench staticmon-bench sleep infinity
#   # then, per formula: docker cp formula_in.h smbench:.../ ; docker exec ... ninja
#
# Build tuned for COMPILE speed, not runtime: Debug (-O0, no LTO, no
# -march=native), jemalloc and the socket interface off. Traces are tiny
# (<=100 timepoints) so runtime is irrelevant.

FROM ubuntu:22.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install \
      --no-install-recommends -y \
      ca-certificates git clang lld ninja-build cmake make \
      python3 python3-pip libgmp10 \
    && rm -rf /var/lib/apt/lists/*
RUN pip3 install --no-cache-dir conan
RUN ln -sf /usr/bin/ld.lld /usr/local/bin/ld

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
      '[settings]' 'os=Linux' "arch=$CONAN_ARCH" 'compiler=clang' \
      'compiler.libcxx=libstdc++11' 'compiler.cppstd=20' 'compiler.version=14' \
      'build_type=Debug' \
      > profile
RUN conan install . --output-folder=builddir --build=missing -pr:h=profile -pr:b=profile && \
    CXX=clang++ CC=clang cmake -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$PWD/builddir/conan_toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=Debug -DENABLE_IPO=OFF -DUSE_JEMALLOC=OFF \
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
