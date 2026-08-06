#!/bin/sh
# End-to-end smoke test for the InputGen GPU runtime: clean rebuild, generate,
# replay, and check the expected values from AGENTS.md's test plan.
set -eu

GPU_ARCH=${GPU_ARCH:-gfx942}
INPUTGEN_GPU=${INPUTGEN_GPU:-llvm-inputgen-gpu}
ROOT=$(cd "$(dirname "$0")" && pwd)
TEST_DIR="$ROOT/test"

echo "==> Cleaning"
make -C "$ROOT" -f Makefile clean >/dev/null 2>&1 || true
rm -rf "$ROOT/build"
make -C "$TEST_DIR" -f Makefile clean GPU_ARCH="$GPU_ARCH" >/dev/null 2>&1 || true

echo "==> Building instrumented device image"
make -C "$TEST_DIR" image GPU_ARCH="$GPU_ARCH"

echo "==> Running generate"
cd "$TEST_DIR"
IMAGE="build/test.$GPU_ARCH.image"
RECORD_JSON="build/vvv_foo.json"
INPUTGEN_DATA="input.txt"

GEN_OUT=$("$INPUTGEN_GPU" generate "$IMAGE" "$RECORD_JSON" --inputgen-data "$INPUTGEN_DATA")
echo "$GEN_OUT"
echo "$GEN_OUT" | grep -qx "b = 81" || { echo "FAIL: expected 'b = 81' in generate output"; exit 1; }
echo "$GEN_OUT" | grep -qx "generated value = 9" || { echo "FAIL: expected 'generated value = 9'"; exit 1; }
[ "$(cat input.txt)" = "9" ] || { echo "FAIL: expected input.txt to contain 9, got '$(cat input.txt)'"; exit 1; }

echo "==> Running replay"
REPLAY_OUT=$("$INPUTGEN_GPU" replay "$IMAGE" "$RECORD_JSON" --inputgen-data "$INPUTGEN_DATA")
echo "$REPLAY_OUT"
echo "$REPLAY_OUT" | grep -qx "b = 81" || { echo "FAIL: expected 'b = 81' in replay output"; exit 1; }
echo "$REPLAY_OUT" | grep -qx "replay value = 9" || { echo "FAIL: expected 'replay value = 9'"; exit 1; }

echo "==> All checks passed"
