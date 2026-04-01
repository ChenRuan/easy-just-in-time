#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

LLVM_DIR=""
LLVM_CONFIG="${LLVM_CONFIG:-$(command -v llvm-config || true)}"
BUILD_DIR="$SCRIPT_DIR/build-host-easyjit"
CMAKE_BUILD_TYPE="Release"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"
PYTHON_EXEC="${PYTHON_EXEC:-$(command -v python3 || true)}"

usage() {
  cat <<'EOF'
Usage:
  ./build_host_easyjit.sh [options]

Builds the host-side EasyJIT artifacts that are needed before cross-building:
  - EasyJitPass.so
  - libEasyJitRuntime.so

Options:
  --llvm-dir <path>          LLVM CMake package dir
  --llvm-config <path>       llvm-config to query when --llvm-dir is not given
  --build-dir <path>         Host EasyJIT build dir, default: ./build-host-easyjit
  --build-type <type>        CMake build type, default: Release
  --python <path>            Python executable for EasyJIT CMake, default: python3
  --jobs <n>                 Parallel build jobs
  -h, --help                 Show this help

Example:
  ./build_host_easyjit.sh \
    --llvm-dir /path/to/llvm/lib/cmake/llvm \
    --build-dir /path/to/easy-jit/build-llvm15
EOF
}

die() {
  echo "error: $*" >&2
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --llvm-dir) LLVM_DIR="$2"; shift 2 ;;
    --llvm-config) LLVM_CONFIG="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --build-type) CMAKE_BUILD_TYPE="$2"; shift 2 ;;
    --python) PYTHON_EXEC="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option '$1'" ;;
  esac
done

[[ -n "$PYTHON_EXEC" && -x "$PYTHON_EXEC" ]] || die "python executable not found; pass --python /path/to/python3"

if [[ -z "$LLVM_DIR" ]]; then
  [[ -n "$LLVM_CONFIG" && -x "$LLVM_CONFIG" ]] || die "--llvm-dir not set and llvm-config not found; pass --llvm-dir or --llvm-config"
  LLVM_CMAKE_DIR="$("$LLVM_CONFIG" --cmakedir)"
  [[ -n "$LLVM_CMAKE_DIR" ]] || die "failed to query llvm-config --cmakedir"
  LLVM_DIR="$LLVM_CMAKE_DIR"
fi
[[ -d "$LLVM_DIR" ]] || die "LLVM_DIR not found: $LLVM_DIR"

mkdir -p "$BUILD_DIR"

echo "==> Configuration"
echo "  llvm_dir        = $LLVM_DIR"
if [[ -n "$LLVM_CONFIG" ]]; then
  echo "  llvm_config     = $LLVM_CONFIG"
fi
echo "  build_dir       = $BUILD_DIR"
echo "  build_type      = $CMAKE_BUILD_TYPE"
echo "  python_exec     = $PYTHON_EXEC"
echo

cmake -S "$SCRIPT_DIR" \
  -B "$BUILD_DIR" \
  -G Ninja \
  -DLLVM_DIR="$LLVM_DIR" \
  -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
  -DPYTHON_EXEC="$PYTHON_EXEC"

cmake --build "$BUILD_DIR" --target easy-jit-core --parallel "$JOBS"

echo "==> Done"
echo "  pass    : $BUILD_DIR/bin/EasyJitPass.so"
echo "  runtime : $BUILD_DIR/bin/libEasyJitRuntime.so"
