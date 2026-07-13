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
# extracted from. Nothing in the upstream repo is modified; the oracle driver
# is copied NEXT TO it in this image so dune resolves libmonpoly in-workspace.
ARG MONPOLY_REPO=https://bitbucket.org/jshs/monpoly.git
ARG MONPOLY_COMMIT=fe89b7da43c1c15edd615a749c56e643cb2e10b8
RUN git clone "$MONPOLY_REPO" /home/opam/monpoly && \
    cd /home/opam/monpoly && git checkout "$MONPOLY_COMMIT"

COPY --chown=opam test/parser_oracle /home/opam/monpoly/oracle
RUN cd /home/opam/monpoly && opam exec -- dune build --release oracle/oracle.exe

ENTRYPOINT ["/home/opam/monpoly/_build/default/oracle/oracle.exe"]
