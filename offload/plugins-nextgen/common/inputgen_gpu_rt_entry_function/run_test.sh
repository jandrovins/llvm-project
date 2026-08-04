#!/bin/sh
# End-to-end smoke test for the InputGen GPU runtime: clean rebuild, generate,
# replay, and check the expected values from AGENTS.md's test plan.
set -eu

GPU_ARCH=${GPU_ARCH:-gfx942}
ROOT=$(cd "$(dirname "$0")" && pwd)
TEST_DIR="$ROOT/test"

echo "==> Cleaning"
make -C "$ROOT" -f Makefile clean >/dev/null 2>&1 || true
rm -rf "$ROOT/build"
make -C "$TEST_DIR" -f Makefile clean GPU_ARCH="$GPU_ARCH" >/dev/null 2>&1 || true

echo "==> Building instrumented device image + test_gpu.bin"
make -C "$TEST_DIR" gpu GPU_ARCH="$GPU_ARCH"

echo "==> Running generate"
cd "$TEST_DIR"
GEN_OUT=$(./build/test_gpu.bin generate)
echo "$GEN_OUT"
echo "$GEN_OUT" | grep -qx "b = 81" || { echo "FAIL: expected 'b = 81' in generate output"; exit 1; }
echo "$GEN_OUT" | grep -qx "generated value = 9" || { echo "FAIL: expected 'generated value = 9'"; exit 1; }
[ "$(cat input.txt)" = "9" ] || { echo "FAIL: expected input.txt to contain 9, got '$(cat input.txt)'"; exit 1; }

echo "==> Running replay"
REPLAY_OUT=$(./build/test_gpu.bin replay)
echo "Replay output: $REPLAY_OUT"

echo "==> All checks passed"
