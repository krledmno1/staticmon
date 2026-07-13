#!/usr/bin/env bash
# Entrypoint for the standalone StaticMon image. Paths are relative to the
# mounted work directory /work. The flag style follows MonPoly.
#
# Subcommands:
#   compile -sig SIG -formula FORMULA
#       Compile FORMULA (monitorable MFOTL, MonPoly syntax) with signature
#       SIG into a dedicated monitor binary ./FORMULA-basename_staticmon.
#       If that binary already exists and was compiled from the same formula
#       and signature contents (md5 fingerprint stored next to it), it is
#       reused and nothing is rebuilt.
#
#   run -monitor MONITOR [-log LOG]
#       Run a previously compiled MONITOR binary on LOG, or on standard
#       input when -log is omitted (use `docker run -i`). A ';'-terminated
#       copy of the log (StaticMon's dialect) is derived automatically.
#       Verdicts go to stdout as '@ts (time point tp): (tuples)'.
#
# Typical use:
#   docker run --rm -v "$PWD":/work staticmon \
#     compile -sig formula.sig -formula formula.mfotl
#   docker run --rm -v "$PWD":/work staticmon \
#     run -monitor formula_staticmon -log trace.log
#   cat trace.log | docker run -i --rm -v "$PWD":/work staticmon \
#     run -monitor formula_staticmon
set -euo pipefail

STATICMON_DIR=/opt/staticmon
STATICMON_COMPILE="$STATICMON_DIR/builddir/bin/staticmon_compile"
HDRDIR="$STATICMON_DIR/src/staticmon/input_formula"

usage() { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

cmd_compile() {
  local formula= sig=
  while [[ $# -gt 0 ]]; do
    case $1 in
      -sig)     sig=${2:?-sig needs an argument};     shift 2;;
      -formula) formula=${2:?-formula needs an argument}; shift 2;;
      *) echo "error: unknown compile option '$1'" >&2; usage;;
    esac
  done
  [[ -n $sig && -n $formula ]] || { echo "error: compile requires -sig SIG and -formula FORMULA" >&2; usage; }
  for f in "$formula" "$sig"; do
    [[ -f $f ]] || { echo "error: '$f' not found in work directory" >&2; exit 2; }
  done

  local base bin key
  base=$(basename "${formula%.*}")
  bin=${base}_staticmon
  # reuse the existing binary iff it was built from identical inputs
  key=$(cat "$sig" "$formula" | md5sum | cut -d' ' -f1)
  if [[ -x $bin && -f $bin.md5 && $(cat "$bin.md5") == "$key" ]]; then
    echo "reusing ./$bin (formula and signature unchanged)"
    return 0
  fi

  mkdir -p "$HDRDIR"
  if ! "$STATICMON_COMPILE" -sig "$sig" -formula "$formula" \
         -prefix "$HDRDIR" > /dev/null 2>&1; then
    echo "error: header generation failed - is the formula monitorable, "\
"well-typed, and in the supported fragment?" >&2
    exit 3
  fi
  ninja -C "$STATICMON_DIR/builddir" > /dev/null
  cp "$STATICMON_DIR/builddir/bin/staticmon" "./$bin"
  echo "$key" > "$bin.md5"
  echo "compiled ./$bin"
}

cmd_run() {
  local monitor= log=
  while [[ $# -gt 0 ]]; do
    case $1 in
      -monitor) monitor=${2:?-monitor needs an argument}; shift 2;;
      -log)     log=${2:?-log needs an argument};         shift 2;;
      *) echo "error: unknown run option '$1'" >&2; usage;;
    esac
  done
  [[ -n $monitor ]] || { echo "error: run requires -monitor MONITOR" >&2; usage; }
  [[ -x ./$monitor ]] || { echo "error: monitor './$monitor' not found or not executable (compile it first)" >&2; exit 2; }

  if [[ -n $log ]]; then
    [[ -f $log ]] || { echo "error: log '$log' not found in work directory" >&2; exit 2; }
    local slog=$log
    if grep -qv ';$' "$log"; then
      slog=$(mktemp --suffix=.slog)
      sed '/;$/!s/$/;/' "$log" > "$slog"
    fi
    exec "./$monitor" --log "$slog"
  else
    # stream stdin through the dialect filter into the monitor
    sed --unbuffered '/;$/!s/$/;/' | "./$monitor" --log /dev/stdin
  fi
}

case ${1:-} in
  compile) shift; cmd_compile "$@";;
  run)     shift; cmd_run "$@";;
  *)       usage;;
esac
