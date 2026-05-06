#!/bin/bash
# run_c_api_tests.sh — build and run the C API test examples
#
# Usage:
#   ./run_c_api_tests.sh [LLVM_BUILD_DIR] [EASYJIT_BUILD_DIR]
#
# Defaults assume the standard workspace layout.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EASYJIT_SRC="${SCRIPT_DIR}/../.."
INCLUDE_BENCHMARKS="${INCLUDE_BENCHMARKS:-0}"

# Default paths — adjust if your layout differs
LLVM_BUILD="${1:-${EASYJIT_SRC}/../build}"
EASYJIT_BUILD="${2:-${EASYJIT_SRC}/../build-easyjit-minimal}"

CLANG="${LLVM_BUILD}/bin/clang"
PASS="${EASYJIT_BUILD}/bin/EasyJitPass.so"
RUNTIME_DIR="${EASYJIT_BUILD}/bin"
INCLUDE_DIR="${EASYJIT_SRC}/include"

echo "=== EasyJIT C API Test Runner ==="
echo "Clang:       ${CLANG}"
echo "Pass:        ${PASS}"
echo "Runtime:     ${RUNTIME_DIR}"
echo "Include:     ${INCLUDE_DIR}"
echo ""

if [ ! -x "${CLANG}" ]; then
    echo "ERROR: clang not found at ${CLANG}" >&2
    exit 1
fi
if [ ! -f "${PASS}" ]; then
    echo "ERROR: EasyJitPass.so not found at ${PASS}" >&2
    exit 1
fi
if [ ! -f "${RUNTIME_DIR}/libEasyJitRuntime.so" ]; then
    echo "ERROR: libEasyJitRuntime.so not found in ${RUNTIME_DIR}" >&2
    exit 1
fi

OUTDIR="${SCRIPT_DIR}/output"
mkdir -p "${OUTDIR}"

should_skip() {
    local src="$1"
    local base
    base="$(basename "$src")"
    case "$base" in
        config_process_base.c|config_process_easyjit.c|wireless_pointer_perf.c)
            [ "$INCLUDE_BENCHMARKS" = "1" ] && return 1 || return 0
            ;;
        *)
            return 1
            ;;
    esac
}

run_test() {
    local src="$1"
    local name="$(basename "$src" .c)"
    echo "--- Building ${name} ---"

    # Compile with EasyJIT pass (embeds bitcode into the binary)
    "${CLANG}" -g -Xclang -disable-O0-optnone \
        -I"${INCLUDE_DIR}" \
        -Xclang -fpass-plugin="${PASS}" \
        -L"${RUNTIME_DIR}" -Wl,-rpath,"${RUNTIME_DIR}" \
        -lEasyJitRuntime -lm \
        "${src}" -o "${OUTDIR}/${name}"

    echo "--- Running ${name} ---"
    "${OUTDIR}/${name}"
    echo ""
}

for src in "${SCRIPT_DIR}"/*.c; do
    if [ -f "$src" ]; then
        if should_skip "$src"; then
            echo "--- Skipping $(basename "$src") (set INCLUDE_BENCHMARKS=1 to include) ---"
            continue
        fi
        run_test "$src"
    fi
done

echo "=== All C API tests completed ==="
