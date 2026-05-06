#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

TARGET_TRIPLE=""
SYSROOT=""
HOST_LLVM_BUILD=""
HOST_EASYJIT_DIR=""
HOST_EASYJIT_BUILD=""
SOURCE_FILE=""
OUTPUT_FILE=""
RUNTIME_SO=""
RUNTIME_STATIC=""
STATIC_BINARY=0
RUNTIME_BUILD_DIR=""
LLVM_DIR=""
TARGET_CPU=""
GCC_TOOLCHAIN=""
EXTRA_CFLAGS=""
EXTRA_CXXFLAGS=""
EXTRA_LDFLAGS=""
GCC_BIN_DIR=""
GCC_LIB_DIR=""
CXX_STDLIB=""
USE_LLD=0
LIBCXX_INCLUDE_DIR=""
LIBCXX_LIB_DIR=""
LIBCXXABI_LIB_DIR=""
LIBUNWIND_LIB_DIR=""
LIBCXX_MERGED_ABI=0
CMAKE_BUILD_TYPE="Release"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"

usage() {
  cat <<'EOF'
Usage:
  ./build_cross_c_example.sh \
    --target <triple> \
    --sysroot <path> \
    --host-llvm-build <path> \
    (--host-easyjit-build <path> | --host-easyjit-dir <path>) \
    [--runtime-so <path> | --runtime-static <path> | --llvm-dir <path>] \
    --source <file.c> \
    --output <binary>

Cross-compiles a target C binary with the host-side EasyJitPass.so.

Required:
  --target <triple>          Target triple, e.g. aarch64_be-linux-gnu
  --sysroot <path>           Target sysroot
  --host-llvm-build <path>   Host LLVM build dir containing clang
  --source <file.c>          Input C source
  --output <binary>          Output binary

Optional:
  --host-easyjit-build <path>
                             Host EasyJIT build dir containing bin/EasyJitPass.so
  --host-easyjit-dir <path>  Dir containing host EasyJitPass.so
  --runtime-so <path>        Prebuilt target libEasyJitRuntime.so path
  --runtime-static <path>    Prebuilt target static EasyJIT runtime archive,
                             usually libEasyJitRuntimeWithNeededLLVM.a from
                             build_cross_runtime.sh --runtime-type static
                             --bundle-llvm-needed-static
  --static-binary            Link the final target binary with -static.
                             Requires --runtime-static.
  --llvm-dir <path>          Target LLVM CMake package dir; required when --runtime-so is omitted
  --runtime-build-dir <path> Output dir when building target runtime in-script
  --target-cpu <cpu>         Optional -mcpu
  --gcc-toolchain <path>     GCC toolchain root; script derives bin/lib paths
  --gcc-bin-dir <path>       Explicit GCC bin dir for -B
  --gcc-lib-dir <path>       Explicit GCC libgcc dir for -L/-B
  --stdlib <name>            C++ runtime: libstdc++, libc++, or none.
                             Default is libstdc++ for backwards compatibility.
  --use-lld                  Add -fuse-ld=lld to target link flags
  --libcxx-include-dir <dir> Extra libc++ header dir for runtime build
  --libcxx-lib-dir <dir>     Extra dir containing libc++.a/.so
  --libcxxabi-lib-dir <dir>  Extra dir containing libc++abi.a/.so
  --libcxx-merged-abi        libc++.a already contains libc++abi objects;
                             do not add -lc++abi when linking a static binary
  --libunwind-lib-dir <dir>  Extra dir containing libunwind.a/.so
  --extra-cflags <flags>     Extra target C compiler flags
  --extra-cxxflags <flags>   Extra target C++ compiler flags for runtime build
  --extra-ldflags <flags>    Extra target linker flags
                             Use this for explicit ABI/unwind libs, e.g.
                             '-lc++abi -lunwind', if the toolchain needs them.
  --build-type <type>        CMake build type, default: Release
  --jobs <n>                 Parallel build jobs
  -h, --help                 Show this help

Example:
  ./build_cross_c_example.sh \
    --target aarch64_be-linux-gnu \
    --sysroot /opt/sdk/sysroot \
    --host-llvm-build /opt/llvm15-host/build-host \
    --host-easyjit-build /path/to/easy-jit/build-llvm15 \
    --gcc-toolchain /opt/gcc-aarch64be \
    --runtime-so /tmp/aarch64be/libEasyJitRuntime.so \
    --source ./tests/c_api/config_process_easyjit.c \
    --output ./tests/c_api/output/config_process_easyjit.aarch64be

  ./build_cross_c_example.sh \
    --target aarch64-linux-gnu \
    --sysroot /opt/sysroots/aarch64-linux-gnu \
    --host-llvm-build /path/to/llvm/build-host \
    --host-easyjit-build /path/to/easy-jit/build-llvm15 \
    --llvm-dir /path/to/target-llvm/lib/cmake/llvm \
    --gcc-toolchain /opt/gcc-aarch64 \
    --source ./wireless-test/example1/example1-c-api-snapshot-rawptr.c \
    --output ./wireless-test/example1/out/example1-c-api-snapshot-rawptr.aarch64

Pure clang + libc++ example:
  ./build_cross_c_example.sh \
    --target aarch64_be-linux-gnu \
    --sysroot /opt/sdk/sysroot \
    --host-llvm-build /opt/llvm15-host/build-host \
    --host-easyjit-build /path/to/easy-jit/build-llvm15 \
    --llvm-dir /opt/llvm15-aarch64be \
    --stdlib libc++ \
    --use-lld \
    --libcxx-include-dir /opt/sdk/sysroot/usr/include/c++/v1 \
    --libcxx-lib-dir /opt/sdk/lib64 \
    --source ./tests/c_api/add_int.c \
    --output ./tests/c_api/output/add_int.aarch64be

Static board-probe example:
  # First build a light-only bundled static runtime archive:
  ./build_cross_runtime.sh \
    --target aarch64_be-linux-gnu \
    --sysroot /opt/sdk/sysroot \
    --target-llvm-dir /opt/llvm15-aarch64be \
    --host-llvm-build /opt/llvm15-host/build-host \
    --runtime-type static \
    --light-backend-only \
    --bundle-llvm-needed-static \
    --strip-debug \
    --use-lld \
    --stdlib libstdc++ \
    --gcc-toolchain /opt/gcc-aarch64be \
    --extra-cflags "-mno-outline-atomics" \
    --extra-cxxflags "-mno-outline-atomics" \
    --build-dir /tmp/easyjit-be-light-static

  # Then build one self-contained target executable:
  ./build_cross_c_example.sh \
    --target aarch64_be-linux-gnu \
    --sysroot /opt/sdk/sysroot \
    --host-llvm-build /opt/llvm15-host/build-host \
    --host-easyjit-build /path/to/easy-jit/build-llvm15 \
    --runtime-static /tmp/easyjit-be-light-static/bin/libEasyJitRuntimeWithNeededLLVM.a \
    --static-binary \
    --use-lld \
    --stdlib libstdc++ \
    --gcc-toolchain /opt/gcc-aarch64be \
    --extra-cflags "-mno-outline-atomics -fno-vectorize -fno-slp-vectorize" \
    --source ./tests/c_api/be_backend_probe.c \
    --output ./tests/c_api/output/be_backend_probe.aarch64be.static

Static libc++ example when libc++abi is merged into libc++.a:
  ./build_cross_c_example.sh \
    --target aarch64_be-linux-gnu \
    --sysroot /opt/sdk/sysroot \
    --host-llvm-build /opt/llvm15-host/build-host \
    --host-easyjit-build /path/to/easy-jit/build-llvm15 \
    --runtime-static /tmp/easyjit-be-light-static/bin/libEasyJitRuntimeWithNeededLLVM.a \
    --static-binary \
    --use-lld \
    --stdlib libc++ \
    --libcxx-lib-dir /opt/sdk/lib64 \
    --libunwind-lib-dir /opt/sdk/lib64 \
    --libcxx-merged-abi \
    --extra-cflags "-mno-outline-atomics -fno-vectorize -fno-slp-vectorize" \
    --source ./tests/c_api/be_backend_probe.c \
    --output ./tests/c_api/output/be_backend_probe.aarch64be.static
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
    echo "hint: point --llvm-dir to an LLVM build/install that includes static archives and not only shared LLVM libraries." >&2
    exit 1
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target) TARGET_TRIPLE="$2"; shift 2 ;;
    --sysroot) SYSROOT="$2"; shift 2 ;;
    --host-llvm-build) HOST_LLVM_BUILD="$2"; shift 2 ;;
    --host-easyjit-build) HOST_EASYJIT_BUILD="$2"; shift 2 ;;
    --host-easyjit-dir) HOST_EASYJIT_DIR="$2"; shift 2 ;;
    --runtime-so) RUNTIME_SO="$2"; shift 2 ;;
    --runtime-static) RUNTIME_STATIC="$2"; shift 2 ;;
    --static-binary) STATIC_BINARY=1; shift ;;
    --runtime-build-dir) RUNTIME_BUILD_DIR="$2"; shift 2 ;;
    --llvm-dir|--target-llvm-dir) LLVM_DIR="$2"; shift 2 ;;
    --source) SOURCE_FILE="$2"; shift 2 ;;
    --output) OUTPUT_FILE="$2"; shift 2 ;;
    --target-cpu) TARGET_CPU="$2"; shift 2 ;;
    --gcc-toolchain) GCC_TOOLCHAIN="$2"; shift 2 ;;
    --gcc-bin-dir) GCC_BIN_DIR="$2"; shift 2 ;;
    --gcc-lib-dir) GCC_LIB_DIR="$2"; shift 2 ;;
    --stdlib) CXX_STDLIB="$2"; shift 2 ;;
    --use-lld) USE_LLD=1; shift ;;
    --libcxx-include-dir) LIBCXX_INCLUDE_DIR="$2"; shift 2 ;;
    --libcxx-lib-dir) LIBCXX_LIB_DIR="$2"; shift 2 ;;
    --libcxxabi-lib-dir) LIBCXXABI_LIB_DIR="$2"; shift 2 ;;
    --libcxx-merged-abi) LIBCXX_MERGED_ABI=1; shift ;;
    --libunwind-lib-dir) LIBUNWIND_LIB_DIR="$2"; shift 2 ;;
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
[[ -n "$HOST_LLVM_BUILD" ]] || die "--host-llvm-build is required"
[[ -n "$SOURCE_FILE" ]] || die "--source is required"
[[ -n "$OUTPUT_FILE" ]] || die "--output is required"

if [[ -n "$HOST_EASYJIT_BUILD" && -z "$HOST_EASYJIT_DIR" ]]; then
  HOST_EASYJIT_DIR="$HOST_EASYJIT_BUILD/bin"
fi
[[ -n "$HOST_EASYJIT_DIR" ]] || die "pass --host-easyjit-build <build-dir> or --host-easyjit-dir <dir>"

HOST_CLANG="$HOST_LLVM_BUILD/bin/clang"
PASS_SO="$HOST_EASYJIT_DIR/EasyJitPass.so"

[[ -x "$HOST_CLANG" ]] || die "host clang not found: $HOST_CLANG"
[[ -f "$PASS_SO" ]] || die "host pass plugin not found: $PASS_SO"
[[ -f "$SOURCE_FILE" ]] || die "source file not found: $SOURCE_FILE"
[[ -d "$SYSROOT" ]] || die "sysroot not found: $SYSROOT"
case "$CXX_STDLIB" in
  ""|libstdc++|libc++|none) ;;
  *) die "--stdlib must be one of: libstdc++, libc++, none" ;;
esac

if [[ "$STATIC_BINARY" -eq 1 && -z "$RUNTIME_STATIC" ]]; then
  die "--static-binary requires --runtime-static <archive>"
fi
if [[ -n "$RUNTIME_STATIC" && "$STATIC_BINARY" -ne 1 ]]; then
  die "--runtime-static requires --static-binary"
fi
if [[ -n "$RUNTIME_SO" && -n "$RUNTIME_STATIC" ]]; then
  die "pass only one of --runtime-so or --runtime-static"
fi
if [[ -z "$RUNTIME_SO" && -z "$RUNTIME_STATIC" ]]; then
  [[ -n "$LLVM_DIR" ]] || die "--llvm-dir is required when --runtime-so is omitted"
  LLVM_DIR="$(resolve_llvm_dir "$LLVM_DIR" || true)"
  [[ -n "$LLVM_DIR" ]] || die "could not resolve LLVM dir; pass a directory containing LLVMConfig.cmake, or an LLVM root with lib/cmake/llvm or lib64/cmake/llvm"
  if [[ -z "$RUNTIME_BUILD_DIR" ]]; then
    SANITIZED_TARGET=${TARGET_TRIPLE//[^A-Za-z0-9._-]/_}
    RUNTIME_BUILD_DIR="$SCRIPT_DIR/build-cross-runtime-$SANITIZED_TARGET"
  fi
  mkdir -p "$RUNTIME_BUILD_DIR"
fi

mkdir -p "$(dirname "$OUTPUT_FILE")"

if [[ -n "$TARGET_CPU" ]]; then
  RUNTIME_C_FLAGS="-mcpu=$TARGET_CPU"
  RUNTIME_CXX_FLAGS="-mcpu=$TARGET_CPU"
else
  RUNTIME_C_FLAGS=""
  RUNTIME_CXX_FLAGS=""
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
else
  RUNTIME_EXE_LINKER_FLAGS=""
  RUNTIME_SHARED_LINKER_FLAGS=""
fi
if [[ -n "$GCC_LIB_DIR" ]]; then
  RUNTIME_C_FLAGS="${RUNTIME_C_FLAGS:+$RUNTIME_C_FLAGS }-B$GCC_LIB_DIR"
  RUNTIME_CXX_FLAGS="${RUNTIME_CXX_FLAGS:+$RUNTIME_CXX_FLAGS }-B$GCC_LIB_DIR"
  RUNTIME_EXE_LINKER_FLAGS="${RUNTIME_EXE_LINKER_FLAGS:+$RUNTIME_EXE_LINKER_FLAGS }-B$GCC_LIB_DIR -L$GCC_LIB_DIR"
  RUNTIME_SHARED_LINKER_FLAGS="${RUNTIME_SHARED_LINKER_FLAGS:+$RUNTIME_SHARED_LINKER_FLAGS }-B$GCC_LIB_DIR -L$GCC_LIB_DIR"
fi
if [[ "$USE_LLD" -eq 1 ]]; then
  RUNTIME_EXE_LINKER_FLAGS="${RUNTIME_EXE_LINKER_FLAGS:+$RUNTIME_EXE_LINKER_FLAGS }-fuse-ld=lld"
  RUNTIME_SHARED_LINKER_FLAGS="${RUNTIME_SHARED_LINKER_FLAGS:+$RUNTIME_SHARED_LINKER_FLAGS }-fuse-ld=lld"
fi
if [[ -n "$CXX_STDLIB" && "$CXX_STDLIB" != "none" ]]; then
  RUNTIME_CXX_FLAGS="${RUNTIME_CXX_FLAGS:+$RUNTIME_CXX_FLAGS }-stdlib=$CXX_STDLIB"
  RUNTIME_EXE_LINKER_FLAGS="${RUNTIME_EXE_LINKER_FLAGS:+$RUNTIME_EXE_LINKER_FLAGS }-stdlib=$CXX_STDLIB"
  RUNTIME_SHARED_LINKER_FLAGS="${RUNTIME_SHARED_LINKER_FLAGS:+$RUNTIME_SHARED_LINKER_FLAGS }-stdlib=$CXX_STDLIB"
fi
if [[ -n "$LIBCXX_INCLUDE_DIR" ]]; then
  [[ -d "$LIBCXX_INCLUDE_DIR" ]] || die "libc++ include dir not found: $LIBCXX_INCLUDE_DIR"
  RUNTIME_CXX_FLAGS="${RUNTIME_CXX_FLAGS:+$RUNTIME_CXX_FLAGS }-isystem $LIBCXX_INCLUDE_DIR"
fi
for dir in "$LIBCXX_LIB_DIR" "$LIBCXXABI_LIB_DIR" "$LIBUNWIND_LIB_DIR"; do
  if [[ -n "$dir" ]]; then
    [[ -d "$dir" ]] || die "C++ runtime library dir not found: $dir"
    RUNTIME_EXE_LINKER_FLAGS="${RUNTIME_EXE_LINKER_FLAGS:+$RUNTIME_EXE_LINKER_FLAGS }-L$dir"
    RUNTIME_SHARED_LINKER_FLAGS="${RUNTIME_SHARED_LINKER_FLAGS:+$RUNTIME_SHARED_LINKER_FLAGS }-L$dir"
  fi
done
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

echo "==> Configuration"
echo "  target          = $TARGET_TRIPLE"
echo "  target_cpu      = ${TARGET_CPU:-<default>}"
echo "  sysroot         = $SYSROOT"
echo "  host_llvm_build = $HOST_LLVM_BUILD"
echo "  host_easyjit    = $HOST_EASYJIT_DIR"
if [[ -n "$HOST_EASYJIT_BUILD" ]]; then
  echo "  host_easyjit_build = $HOST_EASYJIT_BUILD"
fi
if [[ -n "$GCC_TOOLCHAIN" ]]; then
  echo "  gcc_toolchain   = $GCC_TOOLCHAIN"
fi
if [[ -n "$GCC_BIN_DIR" ]]; then
  echo "  gcc_bin_dir     = $GCC_BIN_DIR"
fi
if [[ -n "$GCC_LIB_DIR" ]]; then
  echo "  gcc_lib_dir     = $GCC_LIB_DIR"
fi
if [[ -n "$CXX_STDLIB" ]]; then
  echo "  cxx_stdlib      = $CXX_STDLIB"
fi
if [[ "$USE_LLD" -eq 1 ]]; then
  echo "  linker          = lld"
fi
if [[ -n "$LIBCXX_INCLUDE_DIR" ]]; then
  echo "  libcxx_include  = $LIBCXX_INCLUDE_DIR"
fi
if [[ -n "$LIBCXX_LIB_DIR" ]]; then
  echo "  libcxx_lib_dir  = $LIBCXX_LIB_DIR"
fi
if [[ -n "$LIBCXXABI_LIB_DIR" ]]; then
  echo "  libcxxabi_lib_dir = $LIBCXXABI_LIB_DIR"
fi
if [[ "$LIBCXX_MERGED_ABI" -eq 1 ]]; then
  echo "  libcxx_merged_abi = ON"
fi
if [[ -n "$LIBUNWIND_LIB_DIR" ]]; then
  echo "  libunwind_lib_dir = $LIBUNWIND_LIB_DIR"
fi
if [[ -n "$LLVM_DIR" ]]; then
  echo "  llvm_dir        = $LLVM_DIR"
fi
if [[ -n "$RUNTIME_BUILD_DIR" ]]; then
  echo "  runtime_build   = $RUNTIME_BUILD_DIR"
fi
if [[ -n "$RUNTIME_SO" ]]; then
  echo "  runtime_so      = $RUNTIME_SO"
elif [[ -n "$RUNTIME_STATIC" ]]; then
  echo "  runtime_static  = $RUNTIME_STATIC"
  echo "  static_binary   = ON"
else
  echo "  runtime_so      = <build in-script>"
fi
echo "  source          = $SOURCE_FILE"
echo "  output          = $OUTPUT_FILE"
echo

if [[ -z "$RUNTIME_SO" && -z "$RUNTIME_STATIC" ]]; then
  echo "==> Configuring target runtime build"
  cmake -S "$SCRIPT_DIR" \
    -B "$RUNTIME_BUILD_DIR" \
    -G Ninja \
    -DLLVM_DIR="$LLVM_DIR" \
    -DBUILD_SHARED_LIBS=OFF \
    -DLLVM_BUILD_LLVM_DYLIB=OFF \
    -DLLVM_LINK_LLVM_DYLIB=OFF \
    -DEASY_JIT_BUILD_PASS=OFF \
    -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR="${TARGET_TRIPLE%%-*}" \
    -DCMAKE_C_COMPILER="$HOST_CLANG" \
    -DCMAKE_CXX_COMPILER="$HOST_LLVM_BUILD/bin/clang++" \
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
  RUNTIME_SO="$RUNTIME_BUILD_DIR/bin/libEasyJitRuntime.so"
  assert_runtime_is_not_linked_against_llvm_shared "$RUNTIME_SO"
fi

if [[ -n "$RUNTIME_STATIC" ]]; then
  [[ -f "$RUNTIME_STATIC" ]] || die "target static runtime archive not found: $RUNTIME_STATIC"
  RUNTIME_DIR=$(cd -- "$(dirname -- "$RUNTIME_STATIC")" && pwd)
else
  [[ -f "$RUNTIME_SO" ]] || die "target runtime not found: $RUNTIME_SO"
  RUNTIME_DIR=$(cd -- "$(dirname -- "$RUNTIME_SO")" && pwd)
fi

COMMON_FLAGS=(
  "--target=$TARGET_TRIPLE"
  "--sysroot=$SYSROOT"
  "-O3"
  "-std=c11"
  "-g"
  "-Wall"
  "-I$SCRIPT_DIR/include"
  "-Xclang" "-fpass-plugin=$PASS_SO"
)

LINK_FLAGS=()
if [[ "$STATIC_BINARY" -eq 1 ]]; then
  LINK_FLAGS+=("-static")
else
  LINK_FLAGS+=("-L$RUNTIME_DIR" "-Wl,-rpath,\$ORIGIN" "-lEasyJitRuntime")
fi

if [[ "$USE_LLD" -eq 1 ]]; then
  LINK_FLAGS+=("-fuse-ld=lld")
fi
if [[ -n "$LIBCXX_LIB_DIR" ]]; then
  LINK_FLAGS+=("-L$LIBCXX_LIB_DIR")
fi
if [[ -n "$LIBCXXABI_LIB_DIR" ]]; then
  LINK_FLAGS+=("-L$LIBCXXABI_LIB_DIR")
fi
if [[ -n "$LIBUNWIND_LIB_DIR" ]]; then
  LINK_FLAGS+=("-L$LIBUNWIND_LIB_DIR")
fi

if [[ -n "$TARGET_CPU" ]]; then
  COMMON_FLAGS+=("-mcpu=$TARGET_CPU")
fi
if [[ -n "$GCC_BIN_DIR" ]]; then
  COMMON_FLAGS+=("-B$GCC_BIN_DIR")
fi
if [[ -n "$GCC_LIB_DIR" ]]; then
  COMMON_FLAGS+=("-B$GCC_LIB_DIR")
  LINK_FLAGS+=("-L$GCC_LIB_DIR" "-B$GCC_LIB_DIR")
fi
if [[ -n "$EXTRA_CFLAGS" ]]; then
  # shellcheck disable=SC2206
  EXTRA_CFLAG_ARR=($EXTRA_CFLAGS)
  COMMON_FLAGS+=("${EXTRA_CFLAG_ARR[@]}")
fi
if [[ -n "$EXTRA_LDFLAGS" ]]; then
  # shellcheck disable=SC2206
  EXTRA_LDFLAG_ARR=($EXTRA_LDFLAGS)
  LINK_FLAGS+=("${EXTRA_LDFLAG_ARR[@]}")
fi

if [[ "$STATIC_BINARY" -eq 1 ]]; then
  LINK_FLAGS+=("-Wl,--start-group" "$RUNTIME_STATIC")
  case "${CXX_STDLIB:-libstdc++}" in
    libstdc++) LINK_FLAGS+=("-lstdc++") ;;
    libc++)
      LINK_FLAGS+=("-lc++")
      if [[ "$LIBCXX_MERGED_ABI" -ne 1 ]]; then
        LINK_FLAGS+=("-lc++abi")
      fi
      LINK_FLAGS+=("-lunwind")
      ;;
    none) ;;
  esac
  LINK_FLAGS+=("-lm" "-lpthread" "-Wl,--end-group")
else
  case "${CXX_STDLIB:-libstdc++}" in
    libstdc++) LINK_FLAGS+=("-lstdc++") ;;
    libc++) LINK_FLAGS+=("-lc++") ;;
    none) ;;
  esac
  LINK_FLAGS+=("-lm" "-lpthread")
fi

"$HOST_CLANG" \
  "${COMMON_FLAGS[@]}" \
  "$SOURCE_FILE" \
  "${LINK_FLAGS[@]}" \
  -o "$OUTPUT_FILE"

echo "==> Done"
echo "  binary  : $OUTPUT_FILE"
if [[ -n "$RUNTIME_STATIC" ]]; then
  echo "  runtime : $RUNTIME_STATIC"
else
  echo "  runtime : $RUNTIME_SO"
fi
