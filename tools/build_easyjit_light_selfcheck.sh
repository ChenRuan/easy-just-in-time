#!/usr/bin/env bash
# tools/build_easyjit_light_selfcheck.sh
#
# One-shot build helper for tools/easyjit_light_selfcheck.cpp.
#
# This script ONLY builds the selfcheck program. It is not part of the
# normal EasyJIT build system and is not used to build downstream
# business projects — those should use the CMake snippets in
# docs/easyjit_integration_zh/ and examples/easyjit_cpp_minimal/.
#
# Typical native usage:
#   tools/build_easyjit_light_selfcheck.sh \
#     --easyjit-root . \
#     --easyjit-lib  build-llvm15-light-only/bin/libEasyJitRuntime.a \
#     --easyjit-pass build-llvm15-global/bin/EasyJitPass.so \
#     --output       tools/output/easyjit_light_selfcheck
#
# Typical cross usage:
#   tools/build_easyjit_light_selfcheck.sh \
#     --clangxx      /opt/llvm15-cross/bin/clang++ \
#     --target       aarch64-linux-gnu \
#     --sysroot      /opt/sysroots/aarch64 \
#     --easyjit-root . \
#     --easyjit-lib  /deliverables/libEasyJitRuntimeWithNeededLLVM.a \
#     --easyjit-pass build-llvm15-global/bin/EasyJitPass.so \
#     --output       tools/output/easyjit_light_selfcheck.aarch64
#
# After build:
#   EASYJIT_LIGHT=force EASYJIT_LIGHT_VERBOSE=1 \
#     <output> --iters 10 --verbose

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)

EASYJIT_ROOT=""
EASYJIT_LIB=""
EASYJIT_PASS=""
OUTPUT=""
CLANGXX=""
TARGET_TRIPLE=""
SYSROOT=""
EXTRA_CXXFLAGS=""
EXTRA_LDFLAGS=""
STDLIB=""

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Builds tools/easyjit_light_selfcheck.cpp into a single executable.

Required (with sensible defaults if omitted):
  --easyjit-root <path>   EasyJIT source root (contains include/easy/*.h).
                          Default: ${REPO_ROOT}
  --easyjit-lib  <path>   EasyJIT runtime library to link. .a (preferred,
                          static) or .so. Default: first existing of
                            build-llvm15-light-only/bin/libEasyJitRuntime.a
                            build-llvm15-global/bin/libEasyJitRuntime.a
                            build-llvm15-global/bin/libEasyJitRuntime.so
  --easyjit-pass <path>   EasyJitPass.so (loaded by clang at compile time).
                          Default: build-llvm15-global/bin/EasyJitPass.so
  --output       <path>   Output executable path.
                          Default: tools/output/easyjit_light_selfcheck

Compiler:
  --clangxx <path>        C++ compiler to use. Default: clang++ from PATH.
                          NOTE: must match the LLVM version EasyJitPass.so
                          was built against (LLVM 15 in this repo).

Cross compilation (optional):
  --target  <triple>      Adds --target=<triple>.
  --sysroot <path>        Adds --sysroot=<path>.
  --stdlib  <name>        libstdc++ | libc++ | none (default: not passed)

Extras:
  --extra-cxxflags "..."  Appended to the compile flags.
  --extra-ldflags  "..."  Appended after the runtime library on the cmdline.
  -h, --help              This help.

After a successful build, run:
  EASYJIT_LIGHT=force EASYJIT_LIGHT_VERBOSE=1 <output> --iters 10 --verbose
EOF
}

die() {
  echo "error: $*" >&2
  exit 1
}

while [ $# -gt 0 ]; do
  case "$1" in
    --easyjit-root)   EASYJIT_ROOT="$2"; shift 2 ;;
    --easyjit-lib)    EASYJIT_LIB="$2"; shift 2 ;;
    --easyjit-pass)   EASYJIT_PASS="$2"; shift 2 ;;
    --output)         OUTPUT="$2"; shift 2 ;;
    --clangxx)        CLANGXX="$2"; shift 2 ;;
    --target)         TARGET_TRIPLE="$2"; shift 2 ;;
    --sysroot)        SYSROOT="$2"; shift 2 ;;
    --stdlib)         STDLIB="$2"; shift 2 ;;
    --extra-cxxflags) EXTRA_CXXFLAGS="$2"; shift 2 ;;
    --extra-ldflags)  EXTRA_LDFLAGS="$2"; shift 2 ;;
    -h|--help)        usage; exit 0 ;;
    *) die "unknown argument: $1 (try --help)" ;;
  esac
done

# ---- defaults --------------------------------------------------------

[ -n "$EASYJIT_ROOT" ] || EASYJIT_ROOT="$REPO_ROOT"
EASYJIT_ROOT=$(cd -- "$EASYJIT_ROOT" && pwd) \
  || die "--easyjit-root does not exist: $EASYJIT_ROOT"

if [ -z "$EASYJIT_PASS" ]; then
  EASYJIT_PASS="$EASYJIT_ROOT/build-llvm15-global/bin/EasyJitPass.so"
fi

if [ -z "$EASYJIT_LIB" ]; then
  for cand in \
      "$EASYJIT_ROOT/build-llvm15-light-only/bin/libEasyJitRuntime.a" \
      "$EASYJIT_ROOT/build-llvm15-global/bin/libEasyJitRuntime.a" \
      "$EASYJIT_ROOT/build-llvm15-global/bin/libEasyJitRuntime.so" ; do
    if [ -f "$cand" ]; then
      EASYJIT_LIB="$cand"
      break
    fi
  done
fi

if [ -z "$OUTPUT" ]; then
  OUTPUT="$EASYJIT_ROOT/tools/output/easyjit_light_selfcheck"
fi

if [ -z "$CLANGXX" ]; then
  CLANGXX=$(command -v clang++ || true)
  [ -n "$CLANGXX" ] || die "no clang++ found in PATH (pass --clangxx)"
fi

# ---- validate --------------------------------------------------------

SRC="$EASYJIT_ROOT/tools/easyjit_light_selfcheck.cpp"
[ -f "$SRC" ] || die "selfcheck source not found: $SRC"
[ -f "$EASYJIT_ROOT/include/easy/jit.h" ] \
  || die "EASYJIT_ROOT does not look right (missing include/easy/jit.h): $EASYJIT_ROOT"

[ -n "$EASYJIT_PASS" ] || die "--easyjit-pass not set and no default could be located"
[ -f "$EASYJIT_PASS" ] \
  || die "EasyJitPass.so not found: $EASYJIT_PASS
  (build the repo first, or pass --easyjit-pass explicitly)"

[ -n "$EASYJIT_LIB" ] || die "--easyjit-lib not set and no default could be located
  (looked for libEasyJitRuntime.{a,so} under build-llvm15-light-only/ and build-llvm15-global/)"
[ -f "$EASYJIT_LIB" ] || die "EasyJIT runtime library not found: $EASYJIT_LIB"

[ -x "$CLANGXX" ] || die "clang++ is not executable: $CLANGXX"

OUTDIR=$(dirname -- "$OUTPUT")
mkdir -p -- "$OUTDIR"

# ---- assemble command -----------------------------------------------

CMD=( "$CLANGXX" -std=c++17 -O2 -g
      -Xclang -disable-O0-optnone
      -I"$EASYJIT_ROOT/include"
      -Xclang -fpass-plugin="$EASYJIT_PASS" )

if [ -n "$TARGET_TRIPLE" ]; then
  CMD+=( --target="$TARGET_TRIPLE" )
fi
if [ -n "$SYSROOT" ]; then
  CMD+=( --sysroot="$SYSROOT" )
fi
if [ -n "$STDLIB" ] && [ "$STDLIB" != "none" ]; then
  CMD+=( -stdlib="$STDLIB" )
fi

# Append user-supplied extra compile flags before the source / link section.
if [ -n "$EXTRA_CXXFLAGS" ]; then
  # shellcheck disable=SC2206
  EXTRA_CXX_ARR=( $EXTRA_CXXFLAGS )
  CMD+=( "${EXTRA_CXX_ARR[@]}" )
fi

CMD+=( "$SRC" "$EASYJIT_LIB" -lstdc++ -lm -pthread -ldl )

if [ -n "$EXTRA_LDFLAGS" ]; then
  # shellcheck disable=SC2206
  EXTRA_LD_ARR=( $EXTRA_LDFLAGS )
  CMD+=( "${EXTRA_LD_ARR[@]}" )
fi

# If we're linking the .so, embed an rpath so the user does not need to
# set LD_LIBRARY_PATH for a quick smoke test.
case "$EASYJIT_LIB" in
  *.so|*.so.*)
    LIBDIR=$(dirname -- "$EASYJIT_LIB")
    CMD+=( -Wl,-rpath,"$LIBDIR" )
    ;;
esac

CMD+=( -o "$OUTPUT" )

# ---- run -------------------------------------------------------------

echo "=== build_easyjit_light_selfcheck.sh ==="
echo "  EASYJIT_ROOT : $EASYJIT_ROOT"
echo "  EASYJIT_LIB  : $EASYJIT_LIB"
echo "  EASYJIT_PASS : $EASYJIT_PASS"
echo "  CLANGXX      : $CLANGXX"
[ -n "$TARGET_TRIPLE" ] && echo "  TARGET       : $TARGET_TRIPLE"
[ -n "$SYSROOT" ]       && echo "  SYSROOT      : $SYSROOT"
[ -n "$STDLIB" ]        && echo "  STDLIB       : $STDLIB"
echo "  OUTPUT       : $OUTPUT"
echo
echo "+ ${CMD[*]}"
"${CMD[@]}"

echo
echo "OK: built $OUTPUT"
echo "Run with:"
echo "  EASYJIT_LIGHT=force EASYJIT_LIGHT_VERBOSE=1 \\"
echo "    $OUTPUT --iters 10 --verbose"
