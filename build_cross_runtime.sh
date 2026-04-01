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
GCC_TOOLCHAIN=""
EXTRA_CFLAGS=""
EXTRA_CXXFLAGS=""
EXTRA_LDFLAGS=""
GCC_BIN_DIR=""
GCC_LIB_DIR=""

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
  --target-llvm-dir <path>   Target LLVM CMake dir, or an LLVM install/build root
  --host-llvm-build <path>   Host LLVM build dir containing clang/clang++

Optional:
  --build-dir <path>         Output build dir, default: ./build-cross-runtime-<sanitized-target>
  --target-cpu <cpu>         Optional -mcpu
  --gcc-toolchain <path>     GCC toolchain root; script derives bin/lib paths
  --gcc-bin-dir <path>       Explicit GCC bin dir for -B
  --gcc-lib-dir <path>       Explicit GCC libgcc dir for -L/-B
  --extra-cflags <flags>     Extra target C compiler flags
  --extra-cxxflags <flags>   Extra target C++ compiler flags
  --extra-ldflags <flags>    Extra target linker flags
  --build-type <type>        CMake build type, default: Release
  --jobs <n>                 Parallel jobs
  -h, --help                 Show this help

Example:
  ./build_cross_runtime.sh \
    --target aarch64_be-linux-gnu \
    --sysroot /opt/sdk/sysroot \
    --target-llvm-dir /opt/llvm15-aarch64be \
    --host-llvm-build /opt/llvm15-host/build-host \
    --gcc-toolchain /opt/gcc-aarch64be
EOF
}

die() {
  echo "error: $*" >&2
  exit 1
}

resolve_gcc_bin_dir() {
  local root="$1"
  local target="$2"
  local candidate
  for candidate in "$root/bin" "$root/usr/bin"; do
    if [[ -x "$candidate/${target}-gcc" ]] || [[ -x "$candidate/${target}-ld" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

resolve_gcc_lib_dir() {
  local root="$1"
  local target="$2"
  local candidate
  while IFS= read -r candidate; do
    if [[ -f "$candidate/libgcc.a" ]] || [[ -f "$candidate/libgcc_s.so" ]] || [[ -f "$candidate/crtbeginS.o" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done < <(find "$root" -type d \( -path "*/lib/gcc/$target/*" -o -path "*/lib64/gcc/$target/*" \) 2>/dev/null | sort)
  return 1
}

resolve_llvm_dir() {
  local candidate="$1"
  if [[ -z "$candidate" ]]; then
    return 1
  fi

  if [[ -f "$candidate/LLVMConfig.cmake" ]]; then
    printf '%s\n' "$candidate"
    return 0
  fi

  if [[ -f "$candidate/lib/cmake/llvm/LLVMConfig.cmake" ]]; then
    printf '%s\n' "$candidate/lib/cmake/llvm"
    return 0
  fi

  if [[ -f "$candidate/lib64/cmake/llvm/LLVMConfig.cmake" ]]; then
    printf '%s\n' "$candidate/lib64/cmake/llvm"
    return 0
  fi

  return 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target) TARGET_TRIPLE="$2"; shift 2 ;;
    --sysroot) SYSROOT="$2"; shift 2 ;;
    --target-llvm-dir) TARGET_LLVM_DIR="$2"; shift 2 ;;
    --host-llvm-build) HOST_LLVM_BUILD="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --target-cpu) TARGET_CPU="$2"; shift 2 ;;
    --gcc-toolchain) GCC_TOOLCHAIN="$2"; shift 2 ;;
    --gcc-bin-dir) GCC_BIN_DIR="$2"; shift 2 ;;
    --gcc-lib-dir) GCC_LIB_DIR="$2"; shift 2 ;;
    --extra-cflags) EXTRA_CFLAGS="$2"; shift 2 ;;
    --extra-cxxflags) EXTRA_CXXFLAGS="$2"; shift 2 ;;
    --extra-ldflags) EXTRA_LDFLAGS="$2"; shift 2 ;;
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
[[ -d "$SYSROOT" ]] || die "sysroot not found: $SYSROOT"

TARGET_LLVM_DIR=$(resolve_llvm_dir "$TARGET_LLVM_DIR" || true)
[[ -n "$TARGET_LLVM_DIR" ]] || die "could not resolve target LLVM dir; pass a directory containing LLVMConfig.cmake, or an LLVM root with lib/cmake/llvm or lib64/cmake/llvm"

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
if [[ -n "$GCC_TOOLCHAIN" ]]; then
  echo "  gcc_toolchain   = $GCC_TOOLCHAIN"
fi
if [[ -n "$GCC_BIN_DIR" ]]; then
  echo "  gcc_bin_dir     = $GCC_BIN_DIR"
fi
if [[ -n "$GCC_LIB_DIR" ]]; then
  echo "  gcc_lib_dir     = $GCC_LIB_DIR"
fi
echo "  build_dir       = $BUILD_DIR"
echo

C_FLAGS=""
CXX_FLAGS=""
EXE_LINKER_FLAGS=""
SHARED_LINKER_FLAGS=""

if [[ -n "$TARGET_CPU" ]]; then
  C_FLAGS="-mcpu=$TARGET_CPU"
  CXX_FLAGS="-mcpu=$TARGET_CPU"
fi
if [[ -n "$GCC_TOOLCHAIN" ]]; then
  if [[ -z "$GCC_BIN_DIR" ]]; then
    GCC_BIN_DIR="$(resolve_gcc_bin_dir "$GCC_TOOLCHAIN" "$TARGET_TRIPLE" || true)"
  fi
  if [[ -z "$GCC_LIB_DIR" ]]; then
    GCC_LIB_DIR="$(resolve_gcc_lib_dir "$GCC_TOOLCHAIN" "$TARGET_TRIPLE" || true)"
  fi
fi
if [[ -n "$GCC_BIN_DIR" ]]; then
  EXE_LINKER_FLAGS="${EXE_LINKER_FLAGS:+$EXE_LINKER_FLAGS }-B$GCC_BIN_DIR"
  SHARED_LINKER_FLAGS="${SHARED_LINKER_FLAGS:+$SHARED_LINKER_FLAGS }-B$GCC_BIN_DIR"
fi
if [[ -n "$GCC_LIB_DIR" ]]; then
  C_FLAGS="${C_FLAGS:+$C_FLAGS }-B$GCC_LIB_DIR"
  CXX_FLAGS="${CXX_FLAGS:+$CXX_FLAGS }-B$GCC_LIB_DIR"
  EXE_LINKER_FLAGS="${EXE_LINKER_FLAGS:+$EXE_LINKER_FLAGS }-B$GCC_LIB_DIR -L$GCC_LIB_DIR"
  SHARED_LINKER_FLAGS="${SHARED_LINKER_FLAGS:+$SHARED_LINKER_FLAGS }-B$GCC_LIB_DIR -L$GCC_LIB_DIR"
fi
if [[ -n "$EXTRA_CFLAGS" ]]; then
  C_FLAGS="${C_FLAGS:+$C_FLAGS }$EXTRA_CFLAGS"
fi
if [[ -n "$EXTRA_CXXFLAGS" ]]; then
  CXX_FLAGS="${CXX_FLAGS:+$CXX_FLAGS }$EXTRA_CXXFLAGS"
fi
if [[ -n "$EXTRA_LDFLAGS" ]]; then
  EXE_LINKER_FLAGS="${EXE_LINKER_FLAGS:+$EXE_LINKER_FLAGS }$EXTRA_LDFLAGS"
  SHARED_LINKER_FLAGS="${SHARED_LINKER_FLAGS:+$SHARED_LINKER_FLAGS }$EXTRA_LDFLAGS"
fi

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
  -DCMAKE_C_FLAGS="$C_FLAGS"
  -DCMAKE_CXX_FLAGS="$CXX_FLAGS"
  -DCMAKE_EXE_LINKER_FLAGS="$EXE_LINKER_FLAGS"
  -DCMAKE_SHARED_LINKER_FLAGS="$SHARED_LINKER_FLAGS"
)

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G Ninja "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" --target EasyJitRuntime --parallel "$JOBS"

echo "==> Done"
echo "  runtime : $BUILD_DIR/bin/libEasyJitRuntime.so"
