#!/usr/bin/env bash
# ------------------------------------------------------------------
#  build-lightweight-easyjit.sh
#
#  Build a minimal LLVM + Clang that is just enough for EasyJIT,
#  then build EasyJIT standalone against that LLVM.
#
#  Usage:
#    ./easy-jit/build-lightweight-easyjit.sh                # full build (LLVM + EasyJIT)
#    ./easy-jit/build-lightweight-easyjit.sh --llvm-only    # build LLVM only
#    ./easy-jit/build-lightweight-easyjit.sh --easyjit-only # build EasyJIT only (LLVM must exist)
#    ./easy-jit/build-lightweight-easyjit.sh --clean        # rm build dirs and rebuild
# ------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
LLVM_SRC="$PROJECT_DIR/llvm"
EASYJIT_SRC="$SCRIPT_DIR"

LLVM_BUILD_DIR="${LLVM_LIGHTWEIGHT_BUILD_DIR:-$PROJECT_DIR/build-lightweight-llvm}"
EASYJIT_BUILD_DIR="${EASYJIT_LIGHTWEIGHT_BUILD_DIR:-$PROJECT_DIR/build-lightweight-easyjit}"
INSTALL_DIR="${LIGHTWEIGHT_INSTALL_DIR:-$PROJECT_DIR/install-lightweight}"

BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
CMAKE_BIN="${CMAKE_BIN:-$(command -v cmake)}"

# ---- Detect host architecture -----------------------------------
HOST_ARCH="$(uname -m)"
case "$HOST_ARCH" in
  x86_64)  LLVM_TARGET="X86"   ;;
  aarch64) LLVM_TARGET="AArch64" ;;
  arm*)    LLVM_TARGET="ARM"   ;;
  riscv64) LLVM_TARGET="RISCV" ;;
  *)       LLVM_TARGET="$HOST_ARCH"
           echo "warning: unknown arch $HOST_ARCH – using '$LLVM_TARGET' as target" ;;
esac

# ---- Parse arguments --------------------------------------------
BUILD_LLVM=1
BUILD_EASYJIT=1
CLEAN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --llvm-only)    BUILD_EASYJIT=0; shift ;;
    --easyjit-only) BUILD_LLVM=0;    shift ;;
    --clean)        CLEAN=1;         shift ;;
    -j)             JOBS="$2";       shift 2 ;;
    --build-type)   BUILD_TYPE="$2"; shift 2 ;;
    --install-dir)  INSTALL_DIR="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,/^# ---/{ /^# ---/d; s/^#  \?//; p }' "$0"
      exit 0 ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done

if [[ "$CLEAN" -eq 1 ]]; then
  echo "==> Cleaning build directories"
  rm -rf "$LLVM_BUILD_DIR" "$EASYJIT_BUILD_DIR" "$INSTALL_DIR"
fi

# ---- Generator --------------------------------------------------
if command -v ninja >/dev/null 2>&1; then
  GENERATOR=Ninja
else
  GENERATOR="Unix Makefiles"
fi

reset_easyjit_cache_if_needed() {
  local cache_file="$EASYJIT_BUILD_DIR/CMakeCache.txt"
  [[ -f "$cache_file" ]] || return 0

  local cached_cc=""
  local cached_cxx=""
  cached_cc=$(sed -n 's|^CMAKE_C_COMPILER:FILEPATH=||p' "$cache_file" | head -n 1)
  cached_cxx=$(sed -n 's|^CMAKE_CXX_COMPILER:FILEPATH=||p' "$cache_file" | head -n 1)

  if [[ -n "${CC:-}" && -n "$cached_cc" && "$cached_cc" != "$CC" ]] || \
     [[ -n "${CXX:-}" && -n "$cached_cxx" && "$cached_cxx" != "$CXX" ]]; then
    echo "==> Compiler changed, resetting $EASYJIT_BUILD_DIR CMake cache"
    rm -f "$EASYJIT_BUILD_DIR/CMakeCache.txt"
    rm -rf "$EASYJIT_BUILD_DIR/CMakeFiles"
  fi
}

# ==================================================================
#  Phase 1 – Minimal LLVM + Clang
# ==================================================================
if [[ "$BUILD_LLVM" -eq 1 ]]; then
  echo "============================================================"
  echo " Phase 1: Building minimal LLVM + Clang"
  echo "   target backend : $LLVM_TARGET"
  echo "   build type     : $BUILD_TYPE"
  echo "   build dir      : $LLVM_BUILD_DIR"
  echo "   install dir    : $INSTALL_DIR"
  echo "   parallel jobs  : $JOBS"
  echo "============================================================"

  mkdir -p "$LLVM_BUILD_DIR"

  "$CMAKE_BIN" -S "$LLVM_SRC" -B "$LLVM_BUILD_DIR" \
    -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DLLVM_TARGETS_TO_BUILD="$LLVM_TARGET" \
    -DLLVM_ENABLE_PROJECTS="clang" \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_ENABLE_BINDINGS=OFF \
    -DLLVM_ENABLE_OCAMLDOC=OFF \
    -DLLVM_ENABLE_Z3_SOLVER=OFF \
    -DLLVM_BUILD_TOOLS=ON \
    -DLLVM_INSTALL_TOOLCHAIN_ONLY=OFF \
    -DLLVM_ENABLE_RTTI=ON \
    -DLLVM_ENABLE_EH=ON \
    -DCLANG_INCLUDE_TESTS=OFF \
    -DCLANG_INCLUDE_DOCS=OFF \
    -DCLANG_ENABLE_ARCMT=OFF \
    -DCLANG_ENABLE_STATIC_ANALYZER=OFF

  "$CMAKE_BIN" --build "$LLVM_BUILD_DIR" --parallel "$JOBS"
  "$CMAKE_BIN" --install "$LLVM_BUILD_DIR"

  echo "==> Minimal LLVM + Clang installed to $INSTALL_DIR"
fi

# ==================================================================
#  Phase 2 – EasyJIT (standalone)
# ==================================================================
if [[ "$BUILD_EASYJIT" -eq 1 ]]; then
  # Determine LLVM_DIR
  LLVM_DIR=""
  for candidate in \
    "$INSTALL_DIR/lib/cmake/llvm" \
    "$LLVM_BUILD_DIR/lib/cmake/llvm" \
    ; do
    if [[ -d "$candidate" ]]; then
      LLVM_DIR="$candidate"
      break
    fi
  done

  if [[ -z "$LLVM_DIR" ]]; then
    echo "error: cannot locate LLVMConfig.cmake.  Build LLVM first (--llvm-only)." >&2
    exit 1
  fi

  # Locate clang/clang++ in the lightweight build or install
  CC=""
  CXX=""
  for dir in "$INSTALL_DIR/bin" "$LLVM_BUILD_DIR/bin"; do
    if [[ -x "$dir/clang" ]]; then
      CC="$dir/clang"
      CXX="$dir/clang++"
      break
    fi
  done

  echo "============================================================"
  echo " Phase 2: Building EasyJIT (standalone)"
  echo "   source      : $EASYJIT_SRC"
  echo "   build dir   : $EASYJIT_BUILD_DIR"
  echo "   LLVM_DIR    : $LLVM_DIR"
  echo "   CC          : ${CC:-<system default>}"
  echo "   CXX         : ${CXX:-<system default>}"
  echo "============================================================"

  mkdir -p "$EASYJIT_BUILD_DIR"
  reset_easyjit_cache_if_needed

  cmake_extra_args=()
  [[ -n "$CC" ]]  && cmake_extra_args+=(-DCMAKE_C_COMPILER="$CC")
  [[ -n "$CXX" ]] && cmake_extra_args+=(-DCMAKE_CXX_COMPILER="$CXX")

  "$CMAKE_BIN" -S "$EASYJIT_SRC" -B "$EASYJIT_BUILD_DIR" \
    -G "$GENERATOR" \
    -DLLVM_DIR="$LLVM_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
    -DEASY_JIT_INCLUDE_TESTS=OFF \
    -DEASY_JIT_INCLUDE_DOCS=OFF \
    "${cmake_extra_args[@]}"

  "$CMAKE_BIN" --build "$EASYJIT_BUILD_DIR" --parallel "$JOBS" --target easy-jit-core

  echo "==> EasyJIT built successfully"
  echo "    Pass plugin : $EASYJIT_BUILD_DIR/bin/EasyJitPass.so"
  echo "    Runtime lib : $EASYJIT_BUILD_DIR/bin/libEasyJitRuntime.so"
fi

echo ""
echo "============================================================"
echo " Build complete!"
echo "============================================================"
