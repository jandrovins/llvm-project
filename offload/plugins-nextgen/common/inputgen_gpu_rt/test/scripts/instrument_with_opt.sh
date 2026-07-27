#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RUNTIME_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)

OPT=${OPT:-opt}
MAKE=${MAKE:-make}
CONFIG=${CONFIG:-"$RUNTIME_DIR/inputgen_gpu_rt_config.json"}
RUNTIME_BC=${RUNTIME_BC:-"$RUNTIME_DIR/build/inputgen_gpu_rt.bc"}

OPT_PATH=$(command -v "$OPT" 2>/dev/null || true)
if [ -n "$OPT_PATH" ]; then
  OPT_BINDIR=$(cd "$(dirname "$OPT_PATH")" && pwd)
else
  OPT_BINDIR=
fi

if [ -z "${LLVM_DIS:-}" ] && [ -n "$OPT_BINDIR" ] && [ -x "$OPT_BINDIR/llvm-dis" ]; then
  LLVM_DIS="$OPT_BINDIR/llvm-dis"
else
  LLVM_DIS=${LLVM_DIS:-llvm-dis}
fi

if [ "$#" -ne 1 ]; then
  echo "usage: $0 input.bc" >&2
  echo "       CONFIG=path/to/config.json OPT=path/to/opt LLVM_DIS=path/to/llvm-dis $0 input.bc" >&2
  exit 2
fi

INPUT=$1

cp $INPUT $INPUT.original.backup
llvm-dis $INPUT.original.backup

if [[ "$INPUT" != *.bc ]]; then
  echo "error: input must end with .bc: $INPUT" >&2
  exit 2
fi

if [ ! -f "$INPUT" ]; then
  echo "error: input bitcode '$INPUT' does not exist" >&2
  exit 2
fi

OUTPUT_PREFIX=${INPUT%.bc}
OUTPUT_BC=$INPUT
OUTPUT_LL=${OUTPUT_PREFIX}.ll
TMP_OPT_BC=${OUTPUT_BC}.opt.tmp
TMP_LL=${OUTPUT_LL}.tmp

cleanup() {
  rm -f "$TMP_OPT_BC" "$TMP_LL"
}

trap cleanup EXIT

run() {
  printf '+'
  printf ' %q' "$@"
  printf '\n'
  "$@"
}

print_bitcode_debug_report() {
  local ll=$1

  local called_symbol_counts
  called_symbol_counts=$(
    rg -o '(call|invoke)( [^@]+)? *@__instrumentor_[A-Za-z0-9_]+' "$ll" 2>/dev/null | \
      sed -E 's/.*@(__instrumentor_[A-Za-z0-9_]+).*/\1/' | \
      sort | uniq -c | sed -E 's/^[[:space:]]+//' || true
  )

  local call_count
  call_count=$(printf '%s\n' "$called_symbol_counts" | awk 'NF {sum += $1} END {print sum + 0}')

  echo "bitcode debug: instrumentor call sites: $call_count"

  local called_symbols
  called_symbols=$(printf '%s\n' "$called_symbol_counts" | awk 'NF {print $2}')
  if [ -n "$called_symbol_counts" ]; then
    echo "bitcode debug: instrumentor calls by symbol:"
    printf '%s\n' "$called_symbol_counts" | sed 's/^/  /'
    echo "bitcode debug: called runtime symbols:"
    printf '  %s\n' $called_symbols
  else
    echo "bitcode debug: instrumentor calls by symbol: none"
    echo "bitcode debug: called runtime symbols: none"
  fi

  local defined_symbols
  defined_symbols=$(
    rg -o '^define .*@(__instrumentor_[A-Za-z0-9_]+)' "$ll" 2>/dev/null | \
      sed -E 's/.*@(__instrumentor_[A-Za-z0-9_]+).*/\1/' | \
      sort -u || true
  )
  if [ -n "$defined_symbols" ]; then
    echo "bitcode debug: defined runtime symbols:"
    printf '  %s\n' $defined_symbols
  else
    echo "bitcode debug: defined runtime symbols: none"
  fi

  local buffer_symbols
  buffer_symbols=$(
    rg -o '^@(__instrumentor_gpu_log_(buffer|offset|records|dropped))' "$ll" 2>/dev/null | \
      sed -E 's/^@//' | sort -u || true
  )
  if [ -n "$buffer_symbols" ]; then
    echo "bitcode debug: buffer globals present:"
    printf '  %s\n' $buffer_symbols
  else
    echo "bitcode debug: buffer globals present: none"
  fi

  if [ "$call_count" = "0" ]; then
    echo "bitcode debug: warning: no __instrumentor_* call sites were found in $ll"
  fi
}

MAKE_ARGS=(-C "$RUNTIME_DIR" device-bc)
if [ -n "${GPU_ARCH:-}" ]; then
  MAKE_ARGS+=("GPU_ARCH=$GPU_ARCH")
fi
run "$MAKE" "${MAKE_ARGS[@]}"

if [ ! -f "$RUNTIME_BC" ]; then
  echo "error: runtime bitcode '$RUNTIME_BC' does not exist" >&2
  echo "hint: run 'make -C $RUNTIME_DIR device-bc${GPU_ARCH:+ GPU_ARCH=$GPU_ARCH}'" >&2
  exit 2
fi

OPT_HELP=$("$OPT" --help 2>&1 || true)
if printf '%s\n' "$OPT_HELP" | grep -q -- "-instrumentor-read-config-files"; then
  INSTRUMENTOR_CONFIG_FLAG="-instrumentor-read-config-files=$CONFIG"
elif printf '%s\n' "$OPT_HELP" | grep -q -- "-instrumentor-read-config-file"; then
  INSTRUMENTOR_CONFIG_FLAG="-instrumentor-read-config-file=$CONFIG"
else
  echo "error: '$OPT' does not appear to support the LLVM Instrumentor options" >&2
  echo "hint: rerun with OPT=/path/to/instrumentor-enabled/opt" >&2
  exit 2
fi

run "$OPT" \
  -passes=instrumentor \
  "$INSTRUMENTOR_CONFIG_FLAG" \
  "$INPUT" \
  -o "$TMP_OPT_BC"

run "$LLVM_DIS" "$TMP_OPT_BC" -o "$TMP_LL"
print_bitcode_debug_report "$TMP_LL"

run mv "$TMP_OPT_BC" "$OUTPUT_BC"
run mv "$TMP_LL" "$OUTPUT_LL"
