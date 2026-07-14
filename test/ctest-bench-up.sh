#!/usr/bin/env bash
# ctest FIXTURES_SETUP for the docker-labeled tests: build the behavioral image
# (if missing) and start the smbench container. Exits 77 (ctest "skip") when
# docker is unavailable or provisioning can't complete, so the dependent tests
# are skipped rather than failed.
set -uo pipefail
command -v docker >/dev/null 2>&1 || { echo "docker not found; skipping docker tests" >&2; exit 77; }

repo=$(cd "$(dirname "$0")/.." && pwd)
img=staticmon-bench
ctr=smbench

if ! docker image inspect "$img" >/dev/null 2>&1; then
  echo "building $img from docker/behavioral.Dockerfile (first run) ..." >&2
  if ! docker build -f "$repo/docker/behavioral.Dockerfile" -t "$img" "$repo" >&2; then
    echo "image build failed; skipping docker tests" >&2; exit 77
  fi
fi

if ! docker exec "$ctr" true 2>/dev/null; then
  docker rm -f "$ctr" >/dev/null 2>&1 || true
  if ! docker run -d --name "$ctr" "$img" sleep infinity >/dev/null; then
    echo "could not start container '$ctr'; skipping docker tests" >&2; exit 77
  fi
fi
echo "bench container '$ctr' ready" >&2
