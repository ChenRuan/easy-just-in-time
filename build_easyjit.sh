#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
LLVM_PROJECT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)

BUILD_DIR_DEFAULT="$LLVM_PROJECT_DIR/build-easyjit"
LLVM_BUILD_DIR_DEFAULT="$LLVM_PROJECT_DIR/build"
LLVM_DIR_DEFAULT="$LLVM_BUILD_DIR_DEFAULT/lib/cmake/llvm"

BUILD_DIR="${EASY_JIT_BUILD_DIR:-$BUILD_DIR_DEFAULT}"
LLVM_BUILD_DIR="${LLVM_BUILD_DIR:-$LLVM_BUILD_DIR_DEFAULT}"
LLVM_DIR="${LLVM_DIR:-$LLVM_DIR_DEFAULT}"
CMAKE_BIN="${CMAKE_BIN:-$(command -v cmake)}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
TARGET="${TARGET:-easy-jit-core}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
GENERATOR="${CMAKE_GENERATOR:-}"
CONFIGURE_ONLY=0
BUILD_ONLY=0

pick_first_existing() {
  local path
  for path in "$@"; do
    if [[ -n "$path" && -e "$path" ]]; then
      printf '%s\n' "$path"
      return 0
    fi
  done
  return 1
}

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] [-- <extra cmake configure args>]

Options:
  --configure-only        Run CMake configure only.
  --build-only            Skip configure and build the existing tree.
  --build-dir <dir>       Override the easy-jit build directory.
  --llvm-build-dir <dir>  Override the LLVM build directory used to find clang.
  --llvm-dir <dir>        Override LLVM_DIR passed to CMake.
  --build-type <type>     CMake build type. Default: $BUILD_TYPE
  --target <name>         Build target. Default: $TARGET
  --jobs <n>              Parallel build jobs. Default: $JOBS
  -h, --help              Show this help.

Environment overrides:
  EASY_JIT_BUILD_DIR, LLVM_BUILD_DIR, LLVM_DIR, BUILD_TYPE, TARGET, JOBS,
  CC, CXX, CMAKE_BIN, CMAKE_GENERATOR
EOF
}

EXTRA_CMAKE_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --configure-only)
      CONFIGURE_ONLY=1
      shift
      ;;
    --build-only)
      BUILD_ONLY=1
      shift
      ;;
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --llvm-build-dir)
      LLVM_BUILD_DIR="$2"
      shift 2
      ;;
    --llvm-dir)
      LLVM_DIR="$2"
      shift 2
      ;;
    --build-type)
      BUILD_TYPE="$2"
      shift 2
      ;;
    --target)
      TARGET="$2"
      shift 2
      ;;
    --jobs)
      JOBS="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      EXTRA_CMAKE_ARGS+=("$@")
      break
      ;;
    *)
      EXTRA_CMAKE_ARGS+=("$1")
      shift
      ;;
  esac
done

if [[ "$CONFIGURE_ONLY" -eq 1 && "$BUILD_ONLY" -eq 1 ]]; then
  echo "error: --configure-only and --build-only cannot be used together." >&2
  exit 1
fi

if [[ ! -x "$CMAKE_BIN" ]]; then
  echo "error: cmake not found. Set CMAKE_BIN or install cmake." >&2
  exit 1
fi

if [[ ! -d "$LLVM_DIR" ]]; then
  echo "error: LLVM_DIR does not exist: $LLVM_DIR" >&2
  echo "hint: set LLVM_DIR or build llvm-project first." >&2
  exit 1
fi

if [[ -z "${CC:-}" ]]; then
  CC=$(pick_first_existing \
    "$LLVM_BUILD_DIR/bin/clang" \
    "$LLVM_BUILD_DIR/bin/clang-19" \
    "$(command -v clang 2>/dev/null || true)" \
    "$(command -v clang-19 2>/dev/null || true)" || true)
fi

if [[ -z "${CXX:-}" ]]; then
  CXX=$(pick_first_existing \
    "$LLVM_BUILD_DIR/bin/clang++" \
    "$LLVM_BUILD_DIR/bin/clang++-19" \
    "$(command -v clang++ 2>/dev/null || true)" \
    "$(command -v clang++-19 2>/dev/null || true)" || true)
fi

if [[ -n "${CC:-}" ]]; then
  export CC
fi

if [[ -n "${CXX:-}" ]]; then
  export CXX
fi

if [[ -z "$GENERATOR" ]]; then
  if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    GENERATOR=$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt" | head -n 1)
  fi
  if [[ -z "$GENERATOR" ]]; then
    if command -v ninja >/dev/null 2>&1; then
      GENERATOR=Ninja
    else
      GENERATOR="Unix Makefiles"
    fi
  fi
fi

mkdir -p "$BUILD_DIR"

if [[ "$BUILD_ONLY" -eq 0 ]]; then
  echo "==> Configuring easy-jit"
  echo "    source: $SCRIPT_DIR"
  echo "    build:  $BUILD_DIR"
  echo "    llvm:   $LLVM_DIR"
  echo "    cc:     ${CC:-<default>}"
  echo "    cxx:    ${CXX:-<default>}"
  "$CMAKE_BIN" -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -G "$GENERATOR" \
    -DLLVM_DIR="$LLVM_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    "${EXTRA_CMAKE_ARGS[@]}"
fi

if [[ "$CONFIGURE_ONLY" -eq 0 ]]; then
  echo "==> Building target $TARGET"
  "$CMAKE_BIN" --build "$BUILD_DIR" --parallel "$JOBS" --target "$TARGET"
fi
