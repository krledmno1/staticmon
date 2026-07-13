# Standalone StaticMon image: compile-per-formula monitoring on a mounted
# work directory, with nothing but the pieces that requires.
#
#   docker build -t staticmon .                    # (x86-64 host)
#   docker build --platform linux/amd64 -t staticmon .   # (Apple Silicon)
#
#   docker run --rm -v "$PWD":/work staticmon \
#     compile -sig formula.sig -formula formula.mfotl
#   docker run --rm -v "$PWD":/work staticmon \
#     run -monitor formula_staticmon -log trace.log
#   cat trace.log | docker run -i --rm -v "$PWD":/work staticmon \
#     run -monitor formula_staticmon
#
# The image contains exactly:
#   - the MonPoly '-explicitmon' fork (vendored at monpoly-exp/, patched for
#     modern OCaml; built in a discarded stage; only its native binary is
#     kept) - it turns a monitorable formula + signature into the two C++
#     headers that instantiate StaticMon's templates;
#   - this repository, configured and compiled once (Clang 14 + ld.lld +
#     Ninja; dependencies resolved by Conan v1 at build time), so that a
#     per-formula rebuild recompiles a single translation unit and relinks;
#   - the entrypoint script (docker/staticmon-entrypoint.sh) wiring the two
#     together over the /work mount.
#
# Notes:
#   - The Conan profile targets x86-64/Clang 14, matching upstream's setup.sh
#     answers "Clang / 14 / release"; on arm64 hosts build with
#     --platform linux/amd64 (qemu). Timings under emulation are not
#     representative.
#   - check_ipo_supported() is gated behind ENABLE_IPO in CMakeLists.txt, so
#     configuring also works on toolchains without LTO support.

# ---- stage 1: MonPoly -explicitmon header generator -------------------------
FROM ubuntu:22.04 AS monpoly-exp
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install \
      --no-install-recommends -y \
      ca-certificates git m4 gcc make libgmp3-dev ocaml opam \
    && rm -rf /var/lib/apt/lists/*
RUN opam init -y --disable-sandboxing --bare && \
    opam switch create default ocaml-system && \
    opam install -y dune dune-build-info menhir ppx_yojson_conv qcheck zarith
# The fork is vendored in-repo at monpoly-exp/ (upstream commit b68b2da with
# docker/monpoly-exp-modern-ocaml.patch already applied as a commit on top:
# modern OCaml/dune + ppx_yojson_conv compatibility). .dockerignore keeps
# _build, .git and evaluation/ out of the build context.
COPY monpoly-exp /monpoly-exp
# Build against a synthetic one-package workspace (the repo's own workspace
# drags in unrelated packages).
RUN tmp=$(mktemp) && printf '(lang dune 3.0)\n' > "$tmp" && \
    eval "$(opam config env)" && cd /monpoly-exp && \
    dune build --workspace="$tmp" --release src/main.exe

# ---- stage 2: StaticMon toolchain + warm build tree -------------------------
FROM ubuntu:22.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install \
      --no-install-recommends -y \
      ca-certificates git clang lld ninja-build cmake make \
      python3 python3-pip libgmp10 \
    && rm -rf /var/lib/apt/lists/*
# StaticMon's build setup predates Conan 2 (v1 profile format, `-if`)
RUN pip3 install --no-cache-dir "conan<2"
# link through ld.lld (Clang LTO objects; GCC cannot be mixed in)
RUN ln -sf /usr/bin/ld.lld /usr/local/bin/ld

COPY . /opt/staticmon
WORKDIR /opt/staticmon

# Replicate the effects of the interactive ./setup.sh for "Clang / 14 /
# release": the Conan profile and the configure invocation it would generate.
RUN printf '%s\n' \
      '[settings]' 'os=Linux' 'os_build=Linux' 'arch=x86_64' \
      'arch_build=x86_64' 'compiler=clang' 'compiler.libcxx=libstdc++11' \
      'compiler.cppstd=20' 'compiler.version=14' 'build_type=Release' \
      '[options]' '[build_requires]' '[env]' 'CC=clang' 'CXX=clang++' \
      'CFLAGS="-flto=full -march=native -g"' \
      'CXXFLAGS="-flto=full -march=native -g"' \
      > profile
# conan resolves boost/abseil/fmt/jemalloc from conancenter and cmake fetches
# lexy via FetchContent: this step needs network access at image build time
RUN conan install . -if builddir --build=missing -pr=profile && \
    CXX=clang++ CC=clang cmake -G Ninja -DUSE_JEMALLOC=ON \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -S . -B builddir

# Prime the build with a trivial formula: the repo ships only
# input_formula/formula.h, which #includes the two generated headers, so a
# first generation + full compile is required; afterwards a per-formula
# rebuild only recompiles staticmon.cpp and relinks.
COPY --from=monpoly-exp /monpoly-exp/_build/default/src/main.exe /opt/monpoly-exp/main.exe
RUN mkdir -p src/staticmon/input_formula && \
    echo 'p(int)' > /tmp/smoke.sig && echo 'ONCE[0,10] p(x)' > /tmp/smoke.mfotl && \
    /opt/monpoly-exp/main.exe -sig /tmp/smoke.sig -formula /tmp/smoke.mfotl \
      -no_rw -explicitmon -explicitmon_prefix "$PWD/src/staticmon/input_formula" && \
    ninja -C builddir && test -x builddir/bin/staticmon

COPY docker/staticmon-entrypoint.sh /usr/local/bin/staticmon-entrypoint
RUN chmod +x /usr/local/bin/staticmon-entrypoint

WORKDIR /work
VOLUME /work
ENTRYPOINT ["staticmon-entrypoint"]
