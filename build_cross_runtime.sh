#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

TARGET_TRIPLE=""
SYSROOT=""
TARGET_LLVM_DIR=""
HOST_LLVM_BUILD=""
BUILD_DIR=""
CMAKE_BUILD_TYPE="Release"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"
TARGET_CPU=""

usage() {
  cat <<'EOF'
Usage:
  ./build_cross_runtime.sh \
    --target <triple> \
    --sysroot <path> \
    --target-llvm-dir <path> \
    --host-llvm-build <path> \
    [--build-dir <path>] [--target-cpu <cpu>]

Builds target-side libEasyJitRuntime.so.

Required:
  --target <triple>          Target triple, e.g. aarch64_be-linux-gnu
  --sysroot <path>           Target sysroot
  --target-llvm-dir <path>   Target LLVM CMake dir
  --host-llvm-build <path>   Host LLVM build dir containing clang/clang++

Optional:
  --build-dir <path>         Output build dir, default: ./build-cross-runtime-<sanitized-target>
  --target-cpu <cpu>         Optional -mcpu
  --build-type <type>        CMake build type, default: Release
  --jobs <n>                 Parallel jobs
  -h, --help                 Show this help

Example:
  ./build_cross_runtime.sh \
    --target aarch64_be-linux-gnu \
    --sysroot /opt/sdk/sysroot \
    --target-llvm-dir /opt/llvm15-aarch64be/lib/cmake/llvm \
    --host-llvm-build /opt/llvm15-host/build-host
EOF
}

die() {
  echo "error: $*" >&2
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target) TARGET_TRIPLE="$2"; shift 2 ;;
    --sysroot) SYSROOT="$2"; shift 2 ;;
    --target-llvm-dir) TARGET_LLVM_DIR="$2"; shift 2 ;;
    --host-llvm-build) HOST_LLVM_BUILD="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --target-cpu) TARGET_CPU="$2"; shift 2 ;;
    --build-type) CMAKE_BUILD_TYPE="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option '$1'" ;;
  esac
done

[[ -n "$TARGET_TRIPLE" ]] || die "--target is required"
[[ -n "$SYSROOT" ]] || die "--sysroot is required"
[[ -n "$TARGET_LLVM_DIR" ]] || die "--target-llvm-dir is required"
[[ -n "$HOST_LLVM_BUILD" ]] || die "--host-llvm-build is required"

HOST_CLANG="$HOST_LLVM_BUILD/bin/clang"
HOST_CLANGXX="$HOST_LLVM_BUILD/bin/clang++"
[[ -x "$HOST_CLANG" ]] || die "host clang not found: $HOST_CLANG"
[[ -x "$HOST_CLANGXX" ]] || die "host clang++ not found: $HOST_CLANGXX"
[[ -d "$TARGET_LLVM_DIR" ]] || die "target LLVM_DIR not found: $TARGET_LLVM_DIR"
[[ -d "$SYSROOT" ]] || die "sysroot not found: $SYSROOT"

if [[ -z "$BUILD_DIR" ]]; then
  SANITIZED_TARGET=${TARGET_TRIPLE//[^A-Za-z0-9._-]/_}
  BUILD_DIR="$SCRIPT_DIR/build-cross-runtime-$SANITIZED_TARGET"
fi

mkdir -p "$BUILD_DIR"

echo "==> Configuration"
echo "  target          = $TARGET_TRIPLE"
echo "  target_cpu      = ${TARGET_CPU:-<default>}"
echo "  sysroot         = $SYSROOT"
echo "  target_llvm_dir = $TARGET_LLVM_DIR"
echo "  host_llvm_build = $HOST_LLVM_BUILD"
echo "  build_dir       = $BUILD_DIR"
echo

CMAKE_ARGS=(
  -DLLVM_DIR="$TARGET_LLVM_DIR"
  -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"
  -DCMAKE_SYSTEM_NAME=Linux
  -DCMAKE_SYSTEM_PROCESSOR="${TARGET_TRIPLE%%-*}"
  -DCMAKE_C_COMPILER="$HOST_CLANG"
  -DCMAKE_CXX_COMPILER="$HOST_CLANGXX"
  -DCMAKE_C_COMPILER_TARGET="$TARGET_TRIPLE"
  -DCMAKE_CXX_COMPILER_TARGET="$TARGET_TRIPLE"
  -DCMAKE_SYSROOT="$SYSROOT"
)

if [[ -n "$TARGET_CPU" ]]; then
  CMAKE_ARGS+=(-DCMAKE_C_FLAGS_INIT="-mcpu=$TARGET_CPU" -DCMAKE_CXX_FLAGS_INIT="-mcpu=$TARGET_CPU")
fi

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G Ninja "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" --target EasyJitRuntime --parallel "$JOBS"

echo "==> Done"
echo "  runtime : $BUILD_DIR/bin/libEasyJitRuntime.so"
