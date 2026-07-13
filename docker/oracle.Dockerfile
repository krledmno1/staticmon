# Parser-equivalence oracle: the newest MonPoly's formula parser
# (jshs/monpoly, pinned) wrapped in a driver that prints parsed ASTs as
# canonical s-expressions. Used by the differential test harness; see
# test/parser_oracle/ and docs/monpoly-grammar.md.
#
#   docker build -f docker/oracle.Dockerfile -t monpoly-oracle .
#   docker run --rm -v "$PWD/corpus":/corpus monpoly-oracle /corpus/*.mfotl
#
# Runs on the host's native architecture (no emulation needed; OCaml builds
# fine on Linux arm64/amd64 — unlike on this repo's macOS host, see STATUS.md).

FROM ocaml/opam:ubuntu-24.04-ocaml-4.14
RUN sudo apt-get update && sudo apt-get install -y --no-install-recommends \
      libgmp-dev m4 \
    && sudo rm -rf /var/lib/apt/lists/*
RUN opam install -y dune zarith

# Pin the exact commit the grammar baseline (docs/monpoly-grammar.md) was
# extracted from; opam builds it in its own copy (upstream repo unmodified).
ARG MONPOLY_REPO=https://bitbucket.org/jshs/monpoly.git
ARG MONPOLY_COMMIT=fe89b7da43c1c15edd615a749c56e643cb2e10b8
RUN git clone "$MONPOLY_REPO" /home/opam/monpoly && \
    cd /home/opam/monpoly && git checkout "$MONPOLY_COMMIT" && \
    opam pin add -y libmonpoly /home/opam/monpoly

# The oracle is its own dune project building against the installed
# libmonpoly — same layout as the native build on the host.
COPY --chown=opam test/parser_oracle /home/opam/oracle
RUN cd /home/opam/oracle && opam exec -- dune build --release ./oracle.exe

ENTRYPOINT ["/home/opam/oracle/_build/default/oracle.exe"]
