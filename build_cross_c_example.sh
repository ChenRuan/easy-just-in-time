#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

TARGET_TRIPLE=""
SYSROOT=""
HOST_LLVM_BUILD=""
HOST_EASYJIT_DIR=""
SOURCE_FILE=""
OUTPUT_FILE=""
RUNTIME_SO=""
TARGET_CPU=""
GCC_TOOLCHAIN=""
EXTRA_CFLAGS=""
EXTRA_LDFLAGS=""

usage() {
  cat <<'EOF'
Usage:
  ./build_cross_c_example.sh \
    --target <triple> \
    --sysroot <path> \
    --host-llvm-build <path> \
    --host-easyjit-dir <path> \
    --runtime-so <path> \
    --source <file.c> \
    --output <binary>

Cross-compiles a target C binary with the host-side EasyJitPass.so.

Required:
  --target <triple>          Target triple, e.g. aarch64_be-linux-gnu
  --sysroot <path>           Target sysroot
  --host-llvm-build <path>   Host LLVM build dir containing clang
  --host-easyjit-dir <path>  Dir containing host EasyJitPass.so
  --runtime-so <path>        Target libEasyJitRuntime.so path
  --source <file.c>          Input C source
  --output <binary>          Output binary

Optional:
  --target-cpu <cpu>         Optional -mcpu
  --gcc-toolchain <path>     GCC toolchain root for crt objects and libgcc
  --extra-cflags <flags>     Extra target C compiler flags
  --extra-ldflags <flags>    Extra target linker flags
  -h, --help                 Show this help

Example:
  ./build_cross_c_example.sh \
    --target aarch64_be-linux-gnu \
    --sysroot /opt/sdk/sysroot \
    --host-llvm-build /opt/llvm15-host/build-host \
    --host-easyjit-dir ./prebuilt/llvm15.0.4 \
    --gcc-toolchain /opt/gcc-aarch64be \
    --runtime-so /tmp/aarch64be/libEasyJitRuntime.so \
    --source ./tests/c_api/config_process_easyjit.c \
    --output ./tests/c_api/output/config_process_easyjit.aarch64be
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
    --host-llvm-build) HOST_LLVM_BUILD="$2"; shift 2 ;;
    --host-easyjit-dir) HOST_EASYJIT_DIR="$2"; shift 2 ;;
    --runtime-so) RUNTIME_SO="$2"; shift 2 ;;
    --source) SOURCE_FILE="$2"; shift 2 ;;
    --output) OUTPUT_FILE="$2"; shift 2 ;;
    --target-cpu) TARGET_CPU="$2"; shift 2 ;;
    --gcc-toolchain) GCC_TOOLCHAIN="$2"; shift 2 ;;
    --extra-cflags) EXTRA_CFLAGS="$2"; shift 2 ;;
    --extra-ldflags) EXTRA_LDFLAGS="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option '$1'" ;;
  esac
done

[[ -n "$TARGET_TRIPLE" ]] || die "--target is required"
[[ -n "$SYSROOT" ]] || die "--sysroot is required"
[[ -n "$HOST_LLVM_BUILD" ]] || die "--host-llvm-build is required"
[[ -n "$HOST_EASYJIT_DIR" ]] || die "--host-easyjit-dir is required"
[[ -n "$RUNTIME_SO" ]] || die "--runtime-so is required"
[[ -n "$SOURCE_FILE" ]] || die "--source is required"
[[ -n "$OUTPUT_FILE" ]] || die "--output is required"

HOST_CLANG="$HOST_LLVM_BUILD/bin/clang"
PASS_SO="$HOST_EASYJIT_DIR/EasyJitPass.so"
RUNTIME_DIR=$(cd -- "$(dirname -- "$RUNTIME_SO")" && pwd)

[[ -x "$HOST_CLANG" ]] || die "host clang not found: $HOST_CLANG"
[[ -f "$PASS_SO" ]] || die "host pass plugin not found: $PASS_SO"
[[ -f "$RUNTIME_SO" ]] || die "target runtime not found: $RUNTIME_SO"
[[ -f "$SOURCE_FILE" ]] || die "source file not found: $SOURCE_FILE"
[[ -d "$SYSROOT" ]] || die "sysroot not found: $SYSROOT"

mkdir -p "$(dirname "$OUTPUT_FILE")"

COMMON_FLAGS=(
  "--target=$TARGET_TRIPLE"
  "--sysroot=$SYSROOT"
  "-O3"
  "-std=c11"
  "-g"
  "-Wall"
  "-I$SCRIPT_DIR/include"
  "-L$RUNTIME_DIR"
  "-Wl,-rpath,\$ORIGIN"
  "-lEasyJitRuntime"
  "-lpthread"
  "-lstdc++"
  "-Xclang" "-load"
  "-Xclang" "$PASS_SO"
  "-Xclang" "-fpass-plugin=$PASS_SO"
)

if [[ -n "$TARGET_CPU" ]]; then
  COMMON_FLAGS+=("-mcpu=$TARGET_CPU")
fi
if [[ -n "$GCC_TOOLCHAIN" ]]; then
  COMMON_FLAGS+=("--gcc-toolchain=$GCC_TOOLCHAIN")
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

echo "==> Configuration"
echo "  target          = $TARGET_TRIPLE"
echo "  target_cpu      = ${TARGET_CPU:-<default>}"
echo "  sysroot         = $SYSROOT"
echo "  host_llvm_build = $HOST_LLVM_BUILD"
echo "  host_easyjit    = $HOST_EASYJIT_DIR"
if [[ -n "$GCC_TOOLCHAIN" ]]; then
  echo "  gcc_toolchain   = $GCC_TOOLCHAIN"
fi
echo "  runtime_so      = $RUNTIME_SO"
echo "  source          = $SOURCE_FILE"
echo "  output          = $OUTPUT_FILE"
echo

"$HOST_CLANG" \
  "${COMMON_FLAGS[@]}" \
  "$SOURCE_FILE" \
  -o "$OUTPUT_FILE"

echo "==> Done"
echo "  binary  : $OUTPUT_FILE"
echo "  runtime : $RUNTIME_SO"
