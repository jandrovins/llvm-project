#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
INSTRUMENT_WITH_OPT=${INSTRUMENT_WITH_OPT:-"$SCRIPT_DIR/instrument_with_opt.sh"}

APP=${1:-./test_gpu}
RECORD_DIR=${RECORD_DIR:-records}
REPLAY_JSON=${REPLAY_JSON:-}
REPLAY_BC=${REPLAY_BC:-}
REPLAY_REPETITIONS=${REPLAY_REPETITIONS:-1}
LLVM_OMP_KERNEL_REPLAY=${LLVM_OMP_KERNEL_REPLAY:-llvm-omp-kernel-replay}

if [ ! -x "$APP" ]; then
  echo "error: application '$APP' does not exist or is not executable" >&2
  echo "usage: $0 ./test_gpu" >&2
  exit 2
fi

if [ ! -x "$INSTRUMENT_WITH_OPT" ]; then
  echo "error: instrumentation helper '$INSTRUMENT_WITH_OPT' does not exist or is not executable" >&2
  exit 2
fi

run() {
  printf '+'
  printf ' %q' "$@"
  printf '\n'
  "$@"
}

mkdir -p "$RECORD_DIR"

run env \
  LIBOMPTARGET_RECORD=1 \
  LIBOMPTARGET_RECORD_REPORT=1 \
  LIBOMPTARGET_RECORD_DIR="$RECORD_DIR" \
  "$APP"

if [ -z "$REPLAY_JSON" ]; then
  REPLAY_JSON=$(find "$RECORD_DIR" -maxdepth 1 -type f -name '*.json' | sort | head -n 1 || true)
fi

if [ -z "$REPLAY_JSON" ]; then
  echo "error: no kernel record JSON found in '$RECORD_DIR'" >&2
  exit 1
fi

if [ ! -f "$REPLAY_JSON" ]; then
  echo "error: replay JSON '$REPLAY_JSON' does not exist" >&2
  exit 1
fi

if [ -z "$REPLAY_BC" ]; then
  REPLAY_BC="${REPLAY_JSON%.json}.bc"
fi

if [ ! -f "$REPLAY_BC" ]; then
  echo "error: recorded bitcode '$REPLAY_BC' does not exist" >&2
  exit 1
fi

echo "replay debug: selected JSON: $REPLAY_JSON"
echo "replay debug: selected bitcode: $REPLAY_BC"

run env LIBOMPTARGET_INPUTGEN=true "$INSTRUMENT_WITH_OPT" "$REPLAY_BC"

run env LIBOMPTARGET_INPUTGEN=true "$LLVM_OMP_KERNEL_REPLAY" --load-bitcode --repetitions="$REPLAY_REPETITIONS" "$REPLAY_JSON"
