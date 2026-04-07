#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

TARGET_PRESET="aarch64"
TARGET_TRIPLE=""
TARGET_CPU=""
SYSROOT=""
LLVM_DIR=""
HOST_LLVM_BUILD=""
HOST_EASYJIT_BUILD=""
RUNTIME_BUILD_DIR=""
SOURCE_FILE=""
OUTPUT_FILE=""
CMAKE_BUILD_TYPE="Release"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"
MODE="all"
GCC_TOOLCHAIN=""
GCC_BIN_DIR=""
GCC_LIB_DIR=""
EXTRA_CFLAGS=""
EXTRA_CXXFLAGS=""
EXTRA_LDFLAGS=""

usage() {
  cat <<'EOF'
Usage:
  ./build_cross_example.sh --source <file.cpp> --sysroot <sysroot> [options]

What it does:
  1. Cross-builds libEasyJitRuntime.so for the target platform
  2. Uses the host EasyJitPass.so to cross-compile a target binary

Important:
  - EasyJitPass.so is a host-side clang plugin and should NOT be cross-compiled
  - libEasyJitRuntime.so is a target-side runtime library and MUST match the target arch

Options:
  --preset <name>            Target preset: aarch64, x86_64
  --target <triple>          Explicit clang target triple, e.g. aarch64-linux-gnu
  --target-cpu <cpu>         Optional -mcpu value for target compilation
  --sysroot <path>           Target sysroot, required
  --source <path>            Source file to build, required
  --output <path>            Output binary path, default: <source-dir>/out/<source-base>-<preset>
  --llvm-dir <path>          LLVM CMake package dir, default: <host-llvm-build>/lib/cmake/llvm
  --host-llvm-build <path>   Host LLVM build dir containing clang/clang++, required
  --host-easyjit-build <path>
                             Host EasyJIT build dir containing EasyJitPass.so, required
  --runtime-build-dir <path> Target EasyJIT runtime build dir, default: <repo>/build-cross-<preset>
  --gcc-toolchain <path>     GCC toolchain root; script derives bin/lib paths
  --gcc-bin-dir <path>       Explicit GCC bin dir for -B
  --gcc-lib-dir <path>       Explicit GCC libgcc dir for -L/-B
  --extra-cflags <flags>     Extra target C compiler flags
  --extra-cxxflags <flags>   Extra target C++ compiler flags
  --extra-ldflags <flags>    Extra target linker flags
  --mode <all|runtime|binary>
                             Build both, only runtime, or only binary
  --build-type <type>        CMake build type, default: Release
  --jobs <n>                 Parallel build jobs
  -h, --help                 Show this help

Examples:
  ./build_cross_example.sh \
    --preset aarch64 \
    --sysroot /opt/sysroots/aarch64-linux-gnu \
    --host-llvm-build /path/to/llvm/build-host \
    --host-easyjit-build /path/to/easy-jit/build-llvm15 \
    --source ./wireless-test/example1/example1-jit.cpp

  ./build_cross_example.sh \
    --target aarch64-linux-gnu \
    --sysroot /opt/sysroots/aarch64-linux-gnu \
    --host-llvm-build /path/to/llvm/build-host \
    --host-easyjit-build /path/to/easy-jit/build-llvm15 \
    --source ./wireless-test/example1/example1-c-api-snapshot-rawptr.c \
    --output ./out/example1-c-api-snapshot-rawptr.aarch64
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

find_readelf() {
  local candidate
  for candidate in "${READELF:-}" "$(command -v readelf 2>/dev/null || true)" "$(command -v llvm-readelf 2>/dev/null || true)"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

assert_runtime_is_not_linked_against_llvm_shared() {
  local runtime_so="$1"
  local readelf_bin
  local needed

  readelf_bin="$(find_readelf || true)"
  [[ -n "$readelf_bin" ]] || die "readelf not found; install binutils or set READELF=/path/to/readelf"
  [[ -f "$runtime_so" ]] || die "runtime not found for verification: $runtime_so"

  needed="$("$readelf_bin" -d "$runtime_so" 2>/dev/null | grep 'Shared library:' || true)"
  if grep -Eq 'libLLVM[^]]*\.so|libRemarks\.so|libLTO\.so' <<<"$needed"; then
    echo "error: runtime still depends on LLVM shared libraries:" >&2
    echo "$needed" >&2
    echo "hint: use an LLVM build/install with static archives for the target libraries." >&2
    exit 1
  fi
}

pick_preset() {
  case "$1" in
    aarch64)
      TARGET_PRESET="aarch64"
      TARGET_TRIPLE="aarch64-linux-gnu"
      ;;
    x86_64)
      TARGET_PRESET="x86_64"
      TARGET_TRIPLE="x86_64-linux-gnu"
      ;;
    *)
      die "unsupported preset '$1' (expected: aarch64, x86_64)"
      ;;
  esac
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset) pick_preset "$2"; shift 2 ;;
    --target) TARGET_TRIPLE="$2"; shift 2 ;;
    --target-cpu) TARGET_CPU="$2"; shift 2 ;;
    --sysroot) SYSROOT="$2"; shift 2 ;;
    --source) SOURCE_FILE="$2"; shift 2 ;;
    --output) OUTPUT_FILE="$2"; shift 2 ;;
    --llvm-dir) LLVM_DIR="$2"; shift 2 ;;
    --host-llvm-build) HOST_LLVM_BUILD="$2"; shift 2 ;;
    --host-easyjit-build) HOST_EASYJIT_BUILD="$2"; shift 2 ;;
    --runtime-build-dir) RUNTIME_BUILD_DIR="$2"; shift 2 ;;
    --gcc-toolchain) GCC_TOOLCHAIN="$2"; shift 2 ;;
    --gcc-bin-dir) GCC_BIN_DIR="$2"; shift 2 ;;
    --gcc-lib-dir) GCC_LIB_DIR="$2"; shift 2 ;;
    --extra-cflags) EXTRA_CFLAGS="$2"; shift 2 ;;
    --extra-cxxflags) EXTRA_CXXFLAGS="$2"; shift 2 ;;
    --extra-ldflags) EXTRA_LDFLAGS="$2"; shift 2 ;;
    --mode) MODE="$2"; shift 2 ;;
    --build-type) CMAKE_BUILD_TYPE="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *)
      die "unknown option '$1'"
      ;;
  esac
done

[[ -n "$TARGET_TRIPLE" ]] || pick_preset "$TARGET_PRESET"
[[ -n "$SYSROOT" ]] || die "--sysroot is required"
[[ -n "$SOURCE_FILE" || "$MODE" == "runtime" ]] || die "--source is required unless --mode runtime"
[[ -n "$HOST_LLVM_BUILD" ]] || die "--host-llvm-build is required"
[[ -n "$HOST_EASYJIT_BUILD" ]] || die "--host-easyjit-build is required"

HOST_CLANG="$HOST_LLVM_BUILD/bin/clang"
HOST_CLANGXX="$HOST_LLVM_BUILD/bin/clang++"
PASS_SO="$HOST_EASYJIT_BUILD/bin/EasyJitPass.so"

[[ -x "$HOST_CLANG" ]] || die "host clang not found: $HOST_CLANG"
[[ -x "$HOST_CLANGXX" ]] || die "host clang++ not found: $HOST_CLANGXX"
[[ -f "$PASS_SO" ]] || die "host pass plugin not found: $PASS_SO"

if [[ -z "$LLVM_DIR" ]]; then
  LLVM_DIR="$HOST_LLVM_BUILD/lib/cmake/llvm"
fi
[[ -d "$LLVM_DIR" ]] || die "LLVM_DIR not found: $LLVM_DIR"

if [[ -z "$RUNTIME_BUILD_DIR" ]]; then
  RUNTIME_BUILD_DIR="$SCRIPT_DIR/build-cross-$TARGET_PRESET"
fi

if [[ -n "$SOURCE_FILE" ]]; then
  SOURCE_FILE=$(realpath "$SOURCE_FILE")
  [[ -f "$SOURCE_FILE" ]] || die "source file not found: $SOURCE_FILE"
  SRC_BASE=$(basename "$SOURCE_FILE")
  SRC_STEM="${SRC_BASE%.*}"
  SRC_EXT="${SRC_BASE##*.}"
  if [[ -z "$OUTPUT_FILE" ]]; then
    mkdir -p "$(dirname "$SOURCE_FILE")/out"
    OUTPUT_FILE="$(dirname "$SOURCE_FILE")/out/${SRC_STEM}-${TARGET_PRESET}"
  fi
fi

mkdir -p "$RUNTIME_BUILD_DIR"
mkdir -p "$(dirname "${OUTPUT_FILE:-$RUNTIME_BUILD_DIR/dummy}")"

echo "==> Configuration"
echo "  preset            = $TARGET_PRESET"
echo "  target            = $TARGET_TRIPLE"
echo "  target_cpu        = ${TARGET_CPU:-<default>}"
echo "  sysroot           = $SYSROOT"
echo "  host_llvm_build   = $HOST_LLVM_BUILD"
echo "  host_easyjit_build= $HOST_EASYJIT_BUILD"
echo "  llvm_dir          = $LLVM_DIR"
echo "  runtime_build_dir = $RUNTIME_BUILD_DIR"
if [[ -n "$GCC_TOOLCHAIN" ]]; then
  echo "  gcc_toolchain     = $GCC_TOOLCHAIN"
fi
if [[ -n "$GCC_BIN_DIR" ]]; then
  echo "  gcc_bin_dir       = $GCC_BIN_DIR"
fi
if [[ -n "$GCC_LIB_DIR" ]]; then
  echo "  gcc_lib_dir       = $GCC_LIB_DIR"
fi
if [[ -n "$SOURCE_FILE" ]]; then
  echo "  source            = $SOURCE_FILE"
  echo "  output            = $OUTPUT_FILE"
fi
echo "  mode              = $MODE"
echo

RUNTIME_C_FLAGS=""
RUNTIME_CXX_FLAGS=""
RUNTIME_EXE_LINKER_FLAGS=""
RUNTIME_SHARED_LINKER_FLAGS=""

if [[ -n "$TARGET_CPU" ]]; then
  RUNTIME_C_FLAGS="-mcpu=$TARGET_CPU"
  RUNTIME_CXX_FLAGS="-mcpu=$TARGET_CPU"
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
  RUNTIME_EXE_LINKER_FLAGS="${RUNTIME_EXE_LINKER_FLAGS:+$RUNTIME_EXE_LINKER_FLAGS }-B$GCC_BIN_DIR"
  RUNTIME_SHARED_LINKER_FLAGS="${RUNTIME_SHARED_LINKER_FLAGS:+$RUNTIME_SHARED_LINKER_FLAGS }-B$GCC_BIN_DIR"
fi
if [[ -n "$GCC_LIB_DIR" ]]; then
  RUNTIME_C_FLAGS="${RUNTIME_C_FLAGS:+$RUNTIME_C_FLAGS }-B$GCC_LIB_DIR"
  RUNTIME_CXX_FLAGS="${RUNTIME_CXX_FLAGS:+$RUNTIME_CXX_FLAGS }-B$GCC_LIB_DIR"
  RUNTIME_EXE_LINKER_FLAGS="${RUNTIME_EXE_LINKER_FLAGS:+$RUNTIME_EXE_LINKER_FLAGS }-B$GCC_LIB_DIR -L$GCC_LIB_DIR"
  RUNTIME_SHARED_LINKER_FLAGS="${RUNTIME_SHARED_LINKER_FLAGS:+$RUNTIME_SHARED_LINKER_FLAGS }-B$GCC_LIB_DIR -L$GCC_LIB_DIR"
fi
if [[ -n "$EXTRA_CFLAGS" ]]; then
  RUNTIME_C_FLAGS="${RUNTIME_C_FLAGS:+$RUNTIME_C_FLAGS }$EXTRA_CFLAGS"
fi
if [[ -n "$EXTRA_CXXFLAGS" ]]; then
  RUNTIME_CXX_FLAGS="${RUNTIME_CXX_FLAGS:+$RUNTIME_CXX_FLAGS }$EXTRA_CXXFLAGS"
fi
if [[ -n "$EXTRA_LDFLAGS" ]]; then
  RUNTIME_EXE_LINKER_FLAGS="${RUNTIME_EXE_LINKER_FLAGS:+$RUNTIME_EXE_LINKER_FLAGS }$EXTRA_LDFLAGS"
  RUNTIME_SHARED_LINKER_FLAGS="${RUNTIME_SHARED_LINKER_FLAGS:+$RUNTIME_SHARED_LINKER_FLAGS }$EXTRA_LDFLAGS"
fi

if [[ "$MODE" == "all" || "$MODE" == "runtime" ]]; then
  echo "==> Configuring target runtime build"
  cmake -S "$SCRIPT_DIR" \
    -B "$RUNTIME_BUILD_DIR" \
    -G Ninja \
    -DLLVM_DIR="$LLVM_DIR" \
    -DBUILD_SHARED_LIBS=OFF \
    -DLLVM_BUILD_LLVM_DYLIB=OFF \
    -DLLVM_LINK_LLVM_DYLIB=OFF \
    -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR="${TARGET_TRIPLE%%-*}" \
    -DCMAKE_C_COMPILER="$HOST_CLANG" \
    -DCMAKE_CXX_COMPILER="$HOST_CLANGXX" \
    -DCMAKE_C_COMPILER_TARGET="$TARGET_TRIPLE" \
    -DCMAKE_CXX_COMPILER_TARGET="$TARGET_TRIPLE" \
    -DCMAKE_SYSROOT="$SYSROOT" \
    -DCMAKE_SKIP_RPATH=ON \
    -DCMAKE_C_FLAGS="$RUNTIME_C_FLAGS" \
    -DCMAKE_CXX_FLAGS="$RUNTIME_CXX_FLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="$RUNTIME_EXE_LINKER_FLAGS" \
    -DCMAKE_SHARED_LINKER_FLAGS="$RUNTIME_SHARED_LINKER_FLAGS"

  echo "==> Building target runtime"
  cmake --build "$RUNTIME_BUILD_DIR" --target EasyJitRuntime --parallel "$JOBS"
  assert_runtime_is_not_linked_against_llvm_shared "$RUNTIME_BUILD_DIR/bin/libEasyJitRuntime.so"
fi

if [[ "$MODE" == "all" || "$MODE" == "binary" ]]; then
  [[ -n "$SOURCE_FILE" ]] || die "--source is required for --mode binary"

  RUNTIME_SO="$RUNTIME_BUILD_DIR/bin/libEasyJitRuntime.so"
  [[ -f "$RUNTIME_SO" ]] || die "target runtime not found: $RUNTIME_SO"

  COMMON_FLAGS=(
    "--target=$TARGET_TRIPLE"
    "--sysroot=$SYSROOT"
    "-O3"
    "-Wall"
    "-I$SCRIPT_DIR/include"
    "-L$RUNTIME_BUILD_DIR/bin"
    "-Wl,-rpath,\$ORIGIN"
    "-lEasyJitRuntime"
    "-lpthread"
  )

  if [[ -n "$TARGET_CPU" ]]; then
    COMMON_FLAGS+=("-mcpu=$TARGET_CPU")
  fi
  if [[ -n "$GCC_BIN_DIR" ]]; then
    COMMON_FLAGS+=("-B$GCC_BIN_DIR")
  fi
  if [[ -n "$GCC_LIB_DIR" ]]; then
    COMMON_FLAGS+=("-B$GCC_LIB_DIR" "-L$GCC_LIB_DIR")
  fi
  if [[ -n "$EXTRA_CFLAGS" ]]; then
    # shellcheck disable=SC2206
    EXTRA_CFLAG_ARR=($EXTRA_CFLAGS)
    COMMON_FLAGS+=("${EXTRA_CFLAG_ARR[@]}")
  fi
  if [[ -n "$EXTRA_LDFLAGS" ]]; then
    # shellcheck disable=SC2206
    EXTRA_LDFLAG_ARR=($EXTRA_LDFLAGS)
    COMMON_FLAGS+=("${EXTRA_LDFLAG_ARR[@]}")
  fi

  case "$SRC_EXT" in
    c)
      echo "==> Cross-compiling C binary"
      "$HOST_CLANG" \
        "${COMMON_FLAGS[@]}" \
        -std=c11 \
        -g \
        -Xclang -disable-O0-optnone \
        -Xclang -fpass-plugin="$PASS_SO" \
        "$SOURCE_FILE" \
        -lstdc++ \
        -o "$OUTPUT_FILE"
      ;;
    cc|cp|cxx|cpp|CPP)
      CXX_COMMON_FLAGS=("${COMMON_FLAGS[@]}")
      if [[ -n "$EXTRA_CXXFLAGS" ]]; then
        # shellcheck disable=SC2206
        EXTRA_CXXFLAG_ARR=($EXTRA_CXXFLAGS)
        CXX_COMMON_FLAGS+=("${EXTRA_CXXFLAG_ARR[@]}")
      fi
      echo "==> Cross-compiling C++ binary"
      "$HOST_CLANGXX" \
        "${CXX_COMMON_FLAGS[@]}" \
        -std=c++17 \
        -g \
        -Xclang -load \
        -Xclang "$PASS_SO" \
        -Xclang -fpass-plugin="$PASS_SO" \
        "$SOURCE_FILE" \
        -o "$OUTPUT_FILE"
      ;;
    *)
      die "unsupported source extension: .$SRC_EXT"
      ;;
  esac

  echo "==> Done"
  echo "  runtime : $RUNTIME_SO"
  echo "  binary  : $OUTPUT_FILE"
fi
