#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

find_default_llvm_config() {
  local candidate
  for candidate in \
    "$(command -v llvm-config 2>/dev/null || true)" \
    "$(command -v llvm-config-21 2>/dev/null || true)" \
    "$(command -v llvm-config-20 2>/dev/null || true)" \
    "$(command -v llvm-config-19 2>/dev/null || true)" \
    "$(command -v llvm-config-18 2>/dev/null || true)" \
    "$(command -v llvm-config-17 2>/dev/null || true)" \
    "$(command -v llvm-config-16 2>/dev/null || true)" \
    "$(command -v llvm-config-15 2>/dev/null || true)" \
    /usr/lib/llvm-21/bin/llvm-config \
    /usr/lib/llvm-20/bin/llvm-config \
    /usr/lib/llvm-19/bin/llvm-config \
    /usr/lib/llvm-18/bin/llvm-config \
    /usr/lib/llvm-17/bin/llvm-config \
    /usr/lib/llvm-16/bin/llvm-config \
    /usr/lib/llvm-15/bin/llvm-config
  do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

DEFAULT_LOCAL_LLVM_DIR=""
for candidate in \
  "$SCRIPT_DIR/../build-host/lib/cmake/llvm" \
  "$SCRIPT_DIR/../build/lib/cmake/llvm"
do
  if [[ -d "$candidate" ]]; then
    DEFAULT_LOCAL_LLVM_DIR="$candidate"
    break
  fi
done

LLVM_DIR=""
LLVM_CONFIG="${LLVM_CONFIG:-$(find_default_llvm_config || true)}"
BUILD_DIR="$SCRIPT_DIR/build-host-easyjit"
CMAKE_BUILD_TYPE="Release"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"
PYTHON_EXEC="${PYTHON_EXEC:-$(command -v python3 || true)}"
CMAKE_PREFIX_PATH_VALUE="${CMAKE_PREFIX_PATH:-}"
ZLIB_ROOT=""
ZLIB_LIBRARY=""
ZLIB_INCLUDE_DIR=""
TERMINFO_LIBRARY=""
LIBXML2_ROOT=""

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
  --cmake-prefix-path <path> Extra CMake prefix path, e.g. /usr/local
  --build-dir <path>         Host EasyJIT build dir, default: ./build-host-easyjit
  --build-type <type>        CMake build type, default: Release
  --python <path>            Python executable for EasyJIT CMake, default: python3
  --zlib-root <path>         Optional ZLIB root
  --zlib-library <path>      Optional explicit libz path
  --zlib-include-dir <path>  Optional explicit zlib include dir
  --terminfo-library <path>  Optional explicit terminfo/tinfo library path
  --libxml2-root <path>      Optional LibXml2 root
  --jobs <n>                 Parallel build jobs
  -h, --help                 Show this help

Example:
  ./build_host_easyjit.sh \
    --llvm-dir /path/to/llvm/lib/cmake/llvm \
    --build-dir /path/to/easy-jit/build-llvm15

  ./build_host_easyjit.sh \
    --llvm-config /usr/bin/llvm-config-18 \
    --build-dir /path/to/easy-jit/build-host-easyjit

  ./build_host_easyjit.sh \
    --llvm-config /usr/local/bin/llvm-config \
    --cmake-prefix-path /usr/local \
    --terminfo-library /usr/local/lib/libtinfo.so \
    --zlib-library /usr/local/lib/libz.so
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
    --cmake-prefix-path) CMAKE_PREFIX_PATH_VALUE="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --build-type) CMAKE_BUILD_TYPE="$2"; shift 2 ;;
    --python) PYTHON_EXEC="$2"; shift 2 ;;
    --zlib-root) ZLIB_ROOT="$2"; shift 2 ;;
    --zlib-library) ZLIB_LIBRARY="$2"; shift 2 ;;
    --zlib-include-dir) ZLIB_INCLUDE_DIR="$2"; shift 2 ;;
    --terminfo-library) TERMINFO_LIBRARY="$2"; shift 2 ;;
    --libxml2-root) LIBXML2_ROOT="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option '$1'" ;;
  esac
done

[[ -n "$PYTHON_EXEC" && -x "$PYTHON_EXEC" ]] || die "python executable not found; pass --python /path/to/python3"

if [[ -z "$LLVM_DIR" ]]; then
  if [[ -n "$DEFAULT_LOCAL_LLVM_DIR" ]]; then
    LLVM_DIR="$DEFAULT_LOCAL_LLVM_DIR"
  else
    [[ -n "$LLVM_CONFIG" && -x "$LLVM_CONFIG" ]] || die "--llvm-dir not set and llvm-config not found; pass --llvm-dir or --llvm-config"
    LLVM_CMAKE_DIR="$("$LLVM_CONFIG" --cmakedir)"
    [[ -n "$LLVM_CMAKE_DIR" ]] || die "failed to query llvm-config --cmakedir"
    LLVM_DIR="$LLVM_CMAKE_DIR"
  fi
fi
[[ -d "$LLVM_DIR" ]] || die "LLVM_DIR not found: $LLVM_DIR"

if [[ -z "$CMAKE_PREFIX_PATH_VALUE" && -n "$LLVM_CONFIG" && -x "$LLVM_CONFIG" ]]; then
  LLVM_PREFIX="$("$LLVM_CONFIG" --prefix 2>/dev/null || true)"
  if [[ -n "$LLVM_PREFIX" && -d "$LLVM_PREFIX" ]]; then
    CMAKE_PREFIX_PATH_VALUE="$LLVM_PREFIX"
  fi
fi

mkdir -p "$BUILD_DIR"

echo "==> Configuration"
echo "  llvm_dir        = $LLVM_DIR"
if [[ -n "$LLVM_CONFIG" ]]; then
  echo "  llvm_config     = $LLVM_CONFIG"
fi
if [[ -n "$DEFAULT_LOCAL_LLVM_DIR" ]]; then
  echo "  local_llvm_dir  = $DEFAULT_LOCAL_LLVM_DIR"
fi
if [[ -n "$CMAKE_PREFIX_PATH_VALUE" ]]; then
  echo "  cmake_prefix    = $CMAKE_PREFIX_PATH_VALUE"
fi
if [[ -n "$ZLIB_ROOT" ]]; then
  echo "  zlib_root       = $ZLIB_ROOT"
fi
if [[ -n "$ZLIB_LIBRARY" ]]; then
  echo "  zlib_library    = $ZLIB_LIBRARY"
fi
if [[ -n "$ZLIB_INCLUDE_DIR" ]]; then
  echo "  zlib_include    = $ZLIB_INCLUDE_DIR"
fi
if [[ -n "$TERMINFO_LIBRARY" ]]; then
  echo "  terminfo_lib    = $TERMINFO_LIBRARY"
fi
if [[ -n "$LIBXML2_ROOT" ]]; then
  echo "  libxml2_root    = $LIBXML2_ROOT"
fi
echo "  build_dir       = $BUILD_DIR"
echo "  build_type      = $CMAKE_BUILD_TYPE"
echo "  python_exec     = $PYTHON_EXEC"
echo

CMAKE_ARGS=(
  -DLLVM_DIR="$LLVM_DIR"
  -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"
  -DPYTHON_EXEC="$PYTHON_EXEC"
)

if [[ -n "$CMAKE_PREFIX_PATH_VALUE" ]]; then
  CMAKE_ARGS+=(-DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH_VALUE")
fi
if [[ -n "$ZLIB_ROOT" ]]; then
  CMAKE_ARGS+=(-DZLIB_ROOT="$ZLIB_ROOT")
fi
if [[ -n "$ZLIB_LIBRARY" ]]; then
  CMAKE_ARGS+=(-DZLIB_LIBRARY="$ZLIB_LIBRARY")
fi
if [[ -n "$ZLIB_INCLUDE_DIR" ]]; then
  CMAKE_ARGS+=(-DZLIB_INCLUDE_DIR="$ZLIB_INCLUDE_DIR")
fi
if [[ -n "$TERMINFO_LIBRARY" ]]; then
  CMAKE_ARGS+=(-DTerminfo_LIBRARIES="$TERMINFO_LIBRARY" -DTerminfo_LINKABLE=TRUE)
fi
if [[ -n "$LIBXML2_ROOT" ]]; then
  CMAKE_ARGS+=(-DLibXml2_ROOT="$LIBXML2_ROOT")
fi

cmake -S "$SCRIPT_DIR" \
  -B "$BUILD_DIR" \
  -G Ninja \
  "${CMAKE_ARGS[@]}"

cmake --build "$BUILD_DIR" --target easy-jit-core --parallel "$JOBS"

echo "==> Done"
echo "  pass    : $BUILD_DIR/bin/EasyJitPass.so"
echo "  runtime : $BUILD_DIR/bin/libEasyJitRuntime.so"
