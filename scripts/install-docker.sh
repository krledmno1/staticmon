#!/usr/bin/env bash
# Install a docker-backed `staticmon` on PATH: build the image if needed, then
# drop a self-contained launcher (no repo/native tree required at run time).
#
#   scripts/install-docker.sh [PREFIX]     # default PREFIX=~/.local
#
# Uninstall: rm "$PREFIX/bin/staticmon"  (and optionally `docker volume rm
# staticmon-cache`, `docker rmi staticmon`).
set -euo pipefail
prefix="${1:-$HOME/.local}"
repo="$(cd "$(dirname "$0")/.." && pwd)"
image="${STATICMON_IMAGE:-staticmon}"

command -v docker >/dev/null 2>&1 || { echo "docker is required" >&2; exit 1; }
if ! docker image inspect "$image" >/dev/null 2>&1; then
  echo "building image '$image' from $repo/Dockerfile ..." >&2
  docker build -t "$image" "$repo"
fi

mkdir -p "$prefix/bin"
dst="$prefix/bin/staticmon"
cat > "$dst" <<EOF
#!/bin/sh
# staticmon (docker-backed). Cache persists in the 'staticmon-cache' volume;
# the current directory is mounted at /work, so pass paths relative to it.
exec docker run --rm -i \\
  -v staticmon-cache:/cache -v "\$PWD":/work -w /work '$image' "\$@"
EOF
chmod 755 "$dst"
echo "installed docker-backed staticmon -> $dst" >&2
case ":$PATH:" in *":$prefix/bin:"*) ;; *) echo "note: add $prefix/bin to your PATH" >&2;; esac
