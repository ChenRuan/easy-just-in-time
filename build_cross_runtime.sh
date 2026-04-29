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
CXX_STDLIB=""
USE_LLD=0
LIBCXX_INCLUDE_DIR=""
LIBCXX_LIB_DIR=""
LIBCXXABI_LIB_DIR=""
LIBUNWIND_LIB_DIR=""
RUNTIME_TYPE="shared"
USE_CUSTOM_NEW_DELETE=0
BUNDLE_LLVM_STATIC=0
BUNDLE_LLVM_NEEDED_STATIC=0
STATIC_LIBUNWIND=0
STRIP_DEBUG=0
LLVM_COMPONENTS=(
  core
  codegen
  interpreter
  support
  mcjit
  native
  nativecodegen
  executionengine
  passes
  objcarcopts
  jitlink
  orcjit
  orcshared
  orctargetprocess
)

usage() {
  cat <<'EOF'
Usage:
  ./build_cross_runtime.sh \
    --target <triple> \
    --sysroot <path> \
    --target-llvm-dir <path> \
    --host-llvm-build <path> \
    [--build-dir <path>] [--target-cpu <cpu>]

Builds target-side libEasyJitRuntime.so or libEasyJitRuntime.a.

Required:
  --target <triple>          Target triple, e.g. aarch64_be-linux-gnu
  --sysroot <path>           Target sysroot
  --target-llvm-dir <path>   Target LLVM CMake dir, or an LLVM install/build root
  --host-llvm-build <path>   Host LLVM build dir containing clang/clang++

Optional:
  --build-dir <path>         Output build dir, default: ./build-cross-runtime-<sanitized-target>
  --target-cpu <cpu>         Optional -mcpu
  --runtime-type <type>      Runtime output: shared or static, default: shared
  --use-custom-new-delete    Route EasyJIT's global new/delete through
                             the platform XXX_MemAlloc/XXX_MemFree hooks
  --bundle-llvm-static       With --runtime-type static, also emit
                             libEasyJitRuntimeWithLLVM.a containing EasyJIT
                             runtime objects plus LLVM static archive members.
                             C++ runtime libraries are intentionally excluded.
  --bundle-llvm-needed-static
                             With --runtime-type static, also emit
                             libEasyJitRuntimeWithNeededLLVM.a by doing a
                             relocatable link that pulls only LLVM archive
                             members needed by EasyJIT. C++ runtime libraries
                             are intentionally excluded.
  --gcc-toolchain <path>     GCC toolchain root; script derives bin/lib paths
  --gcc-bin-dir <path>       Explicit GCC bin dir for -B
  --gcc-lib-dir <path>       Explicit GCC libgcc dir for -L/-B
  --stdlib <name>            C++ runtime: libstdc++, libc++, or none.
                             Default keeps clang's normal target default.
  --use-lld                  Add -fuse-ld=lld to target link flags
  --libcxx-include-dir <dir> Extra libc++ header dir, e.g. sysroot/usr/include/c++/v1
  --libcxx-lib-dir <dir>     Extra dir containing libc++.a/.so
  --libcxxabi-lib-dir <dir>  Extra dir containing libc++abi.a/.so
  --libunwind-lib-dir <dir>  Extra dir containing libunwind.a/.so
  --static-libunwind         Link libunwind by full path to libunwind.a from
                             --libunwind-lib-dir, avoiding accidental .so use
  --strip-debug              Compile EasyJIT runtime with -g0 and strip debug
                             sections from generated static bundle artifacts
  --extra-cflags <flags>     Extra target C compiler flags
  --extra-cxxflags <flags>   Extra target C++ compiler flags
  --extra-ldflags <flags>    Extra target linker flags
                             Use this for explicit ABI/unwind libs, e.g.
                             '-lc++abi -lunwind', if the toolchain needs them.
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

Static runtime example:
  ./build_cross_runtime.sh \
    --target aarch64_be-linux-gnu \
    --sysroot /opt/sdk/sysroot \
    --target-llvm-dir /opt/llvm15-aarch64be \
    --host-llvm-build /opt/llvm15-host/build-host \
    --runtime-type static \
    --use-custom-new-delete \
    --bundle-llvm-static \
    --bundle-llvm-needed-static \
    --strip-debug \
    --gcc-toolchain /opt/gcc-aarch64be

Pure clang + libc++ example:
  ./build_cross_runtime.sh \
    --target aarch64_be-linux-gnu \
    --sysroot /opt/sdk/sysroot \
    --target-llvm-dir /opt/llvm15-aarch64be \
    --host-llvm-build /opt/llvm15-host/build-host \
    --runtime-type static \
    --stdlib libc++ \
    --use-lld \
    --libcxx-include-dir /opt/sdk/sysroot/usr/include/c++/v1 \
    --libcxx-lib-dir /opt/sdk/lib64
EOF
}

die() {
  echo "error: $*" >&2
  exit 1
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

resolve_static_lib() {
  local lib_name="$1"
  shift
  local dir
  for dir in "$@"; do
    if [[ -n "$dir" && -f "$dir/$lib_name" ]]; then
      printf '%s\n' "$dir/$lib_name"
      return 0
    fi
  done
  return 1
}

find_llvm_ar() {
  local candidate
  for candidate in \
      "$HOST_LLVM_BUILD/bin/llvm-ar" \
      "$(command -v llvm-ar 2>/dev/null || true)" \
      "$(command -v ar 2>/dev/null || true)"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

find_llvm_ranlib() {
  local candidate
  for candidate in \
      "$HOST_LLVM_BUILD/bin/llvm-ranlib" \
      "$(command -v llvm-ranlib 2>/dev/null || true)" \
      "$(command -v ranlib 2>/dev/null || true)"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

find_strip_tool() {
  local candidate
  for candidate in \
      "$HOST_LLVM_BUILD/bin/llvm-strip" \
      "$(command -v llvm-strip 2>/dev/null || true)" \
      "$(command -v strip 2>/dev/null || true)"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

strip_debug_file() {
  local file="$1"
  local strip_bin

  [[ "$STRIP_DEBUG" -eq 1 ]] || return 0
  [[ -f "$file" ]] || return 0

  strip_bin="$(find_strip_tool || true)"
  [[ -n "$strip_bin" ]] || die "--strip-debug requested, but llvm-strip/strip was not found"
  "$strip_bin" --strip-debug "$file"
}

write_llvm_static_libs() {
  local out_file="$1"
  local probe_src="$BUILD_DIR/llvm-lib-probe-src"
  local probe_build="$BUILD_DIR/llvm-lib-probe-build"
  local components

  components="${LLVM_COMPONENTS[*]}"
  rm -rf "$probe_src" "$probe_build"
  mkdir -p "$probe_src"
  cat > "$probe_src/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.13)
project(easyjit_llvm_lib_probe NONE)
find_package(LLVM REQUIRED CONFIG)
include("\${LLVM_CMAKE_DIR}/LLVM-Config.cmake")
function(append_llvm_target target)
  if(NOT TARGET "\${target}")
    return()
  endif()
  get_property(visited GLOBAL PROPERTY EASYJIT_VISITED_LLVM_TARGETS)
  if(";\${visited};" MATCHES ";\${target};")
    return()
  endif()
  set_property(GLOBAL APPEND PROPERTY EASYJIT_VISITED_LLVM_TARGETS "\${target}")
  set_property(GLOBAL APPEND PROPERTY EASYJIT_LLVM_TARGETS "\${target}")
  get_target_property(deps "\${target}" INTERFACE_LINK_LIBRARIES)
  foreach(dep IN LISTS deps)
    if("\${dep}" MATCHES "^\\\$<LINK_ONLY:([^>]+)>$")
      set(dep "\${CMAKE_MATCH_1}")
    endif()
    if("\${dep}" MATCHES "^LLVM")
      append_llvm_target("\${dep}")
    endif()
  endforeach()
endfunction()

llvm_map_components_to_libnames(EASYJIT_DIRECT_LLVM_LIBS ${components})
foreach(lib IN LISTS EASYJIT_DIRECT_LLVM_LIBS)
  append_llvm_target("\${lib}")
endforeach()
get_property(EASYJIT_LLVM_LIBS GLOBAL PROPERTY EASYJIT_LLVM_TARGETS)
file(WRITE "\${CMAKE_BINARY_DIR}/llvm-static-libs.txt" "")
foreach(lib IN LISTS EASYJIT_LLVM_LIBS)
  set(loc "")
  if(TARGET "\${lib}")
    get_target_property(loc "\${lib}" IMPORTED_LOCATION_RELEASE)
    if(NOT loc)
      get_target_property(loc "\${lib}" IMPORTED_LOCATION)
    endif()
  endif()
  if(NOT loc)
    find_library(loc NAMES "\${lib}" "lib\${lib}.a" PATHS \${LLVM_LIBRARY_DIRS} NO_DEFAULT_PATH)
  endif()
  if(NOT loc)
    message(FATAL_ERROR "Could not resolve LLVM static library for \${lib}")
  endif()
  file(APPEND "\${CMAKE_BINARY_DIR}/llvm-static-libs.txt" "\${loc}\\n")
endforeach()
EOF

  cmake -S "$probe_src" -B "$probe_build" -DLLVM_DIR="$TARGET_LLVM_DIR" >/dev/null
  sort -u "$probe_build/llvm-static-libs.txt" > "$out_file"
}

bundle_static_runtime_with_llvm() {
  local runtime_archive="$1"
  local bundled_archive="$BUILD_DIR/bin/libEasyJitRuntimeWithLLVM.a"
  local llvm_libs_file="$BUILD_DIR/easyjit-llvm-static-libs.txt"
  local mri_script="$BUILD_DIR/easyjit-bundle-llvm.mri"
  local ar_bin
  local ranlib_bin
  local lib

  [[ "$RUNTIME_TYPE" == "static" ]] || die "--bundle-llvm-static requires --runtime-type static"
  [[ -f "$runtime_archive" ]] || die "runtime archive not found: $runtime_archive"

  ar_bin="$(find_llvm_ar || true)"
  [[ -n "$ar_bin" ]] || die "llvm-ar/ar not found"

  write_llvm_static_libs "$llvm_libs_file"

  {
    printf 'CREATE %s\n' "$bundled_archive"
    printf 'ADDLIB %s\n' "$runtime_archive"
    while IFS= read -r lib; do
      [[ -n "$lib" ]] || continue
      [[ -f "$lib" ]] || die "LLVM static library not found: $lib"
      printf 'ADDLIB %s\n' "$lib"
    done < "$llvm_libs_file"
    printf 'SAVE\n'
    printf 'END\n'
  } > "$mri_script"

  rm -f "$bundled_archive"
  "$ar_bin" -M < "$mri_script"
  strip_debug_file "$bundled_archive"

  ranlib_bin="$(find_llvm_ranlib || true)"
  if [[ -n "$ranlib_bin" ]]; then
    "$ranlib_bin" "$bundled_archive"
  fi

  echo "  bundled runtime : $bundled_archive"
  echo "  bundled LLVM libs list : $llvm_libs_file"
}

append_shell_words() {
  local var_name="$1"
  local words="$2"
  local -n out_array="$var_name"

  if [[ -n "$words" ]]; then
    local parsed=()
    # shellcheck disable=SC2206
    parsed=( $words )
    out_array+=( "${parsed[@]}" )
  fi
}

bundle_static_runtime_with_needed_llvm() {
  local runtime_archive="$1"
  local needed_object="$BUILD_DIR/bin/EasyJitRuntimeWithNeededLLVM.o"
  local bundled_archive="$BUILD_DIR/bin/libEasyJitRuntimeWithNeededLLVM.a"
  local llvm_libs_file="$BUILD_DIR/easyjit-llvm-static-libs.txt"
  local link_log="$BUILD_DIR/easyjit-bundle-needed-llvm-link.log"
  local ar_bin
  local ranlib_bin
  local lib
  local link_cmd

  [[ "$RUNTIME_TYPE" == "static" ]] || die "--bundle-llvm-needed-static requires --runtime-type static"
  [[ -f "$runtime_archive" ]] || die "runtime archive not found: $runtime_archive"

  ar_bin="$(find_llvm_ar || true)"
  [[ -n "$ar_bin" ]] || die "llvm-ar/ar not found"

  write_llvm_static_libs "$llvm_libs_file"

  link_cmd=( "$HOST_CLANGXX" "--target=$TARGET_TRIPLE" "--sysroot=$SYSROOT" )
  append_shell_words link_cmd "$BUNDLE_LINKER_FLAGS"
  link_cmd+=( -r -nostdlib -o "$needed_object" )
  link_cmd+=( -Wl,--whole-archive "$runtime_archive" -Wl,--no-whole-archive )
  link_cmd+=( -Wl,--start-group )
  while IFS= read -r lib; do
    [[ -n "$lib" ]] || continue
    [[ -f "$lib" ]] || die "LLVM static library not found: $lib"
    link_cmd+=( "$lib" )
  done < "$llvm_libs_file"
  link_cmd+=( -Wl,--end-group )

  rm -f "$needed_object" "$bundled_archive" "$link_log"
  if ! "${link_cmd[@]}" >"$link_log" 2>&1; then
    echo "error: failed to create needed LLVM relocatable object" >&2
    echo "link log: $link_log" >&2
    tail -80 "$link_log" >&2 || true
    exit 1
  fi

  strip_debug_file "$needed_object"
  "$ar_bin" rcs "$bundled_archive" "$needed_object"
  strip_debug_file "$bundled_archive"

  ranlib_bin="$(find_llvm_ranlib || true)"
  if [[ -n "$ranlib_bin" ]]; then
    "$ranlib_bin" "$bundled_archive"
  fi

  echo "  needed LLVM object : $needed_object"
  echo "  needed LLVM bundled runtime : $bundled_archive"
  echo "  needed LLVM libs search list : $llvm_libs_file"
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
    echo "hint: point --target-llvm-dir to an LLVM build/install that includes static archives and not only shared LLVM libraries." >&2
    exit 1
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target) TARGET_TRIPLE="$2"; shift 2 ;;
    --sysroot) SYSROOT="$2"; shift 2 ;;
    --target-llvm-dir) TARGET_LLVM_DIR="$2"; shift 2 ;;
    --host-llvm-build) HOST_LLVM_BUILD="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --target-cpu) TARGET_CPU="$2"; shift 2 ;;
    --runtime-type) RUNTIME_TYPE="$2"; shift 2 ;;
    --use-custom-new-delete) USE_CUSTOM_NEW_DELETE=1; shift ;;
    --bundle-llvm-static) BUNDLE_LLVM_STATIC=1; shift ;;
    --bundle-llvm-needed-static) BUNDLE_LLVM_NEEDED_STATIC=1; shift ;;
    --gcc-toolchain) GCC_TOOLCHAIN="$2"; shift 2 ;;
    --gcc-bin-dir) GCC_BIN_DIR="$2"; shift 2 ;;
    --gcc-lib-dir) GCC_LIB_DIR="$2"; shift 2 ;;
    --stdlib) CXX_STDLIB="$2"; shift 2 ;;
    --use-lld) USE_LLD=1; shift ;;
    --libcxx-include-dir) LIBCXX_INCLUDE_DIR="$2"; shift 2 ;;
    --libcxx-lib-dir) LIBCXX_LIB_DIR="$2"; shift 2 ;;
    --libcxxabi-lib-dir) LIBCXXABI_LIB_DIR="$2"; shift 2 ;;
    --libunwind-lib-dir) LIBUNWIND_LIB_DIR="$2"; shift 2 ;;
    --static-libunwind) STATIC_LIBUNWIND=1; shift ;;
    --strip-debug) STRIP_DEBUG=1; shift ;;
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
case "$CXX_STDLIB" in
  ""|libstdc++|libc++|none) ;;
  *) die "--stdlib must be one of: libstdc++, libc++, none" ;;
esac
case "$RUNTIME_TYPE" in
  shared|static) ;;
  *) die "--runtime-type must be one of: shared, static" ;;
esac
if [[ "$BUNDLE_LLVM_STATIC" -eq 1 && "$RUNTIME_TYPE" != "static" ]]; then
  die "--bundle-llvm-static requires --runtime-type static"
fi
if [[ "$BUNDLE_LLVM_NEEDED_STATIC" -eq 1 && "$RUNTIME_TYPE" != "static" ]]; then
  die "--bundle-llvm-needed-static requires --runtime-type static"
fi
if [[ "$STATIC_LIBUNWIND" -eq 1 && -z "$LIBUNWIND_LIB_DIR" ]]; then
  die "--static-libunwind requires --libunwind-lib-dir"
fi
if [[ "$STATIC_LIBUNWIND" -eq 1 && "$EXTRA_LDFLAGS" =~ (^|[[:space:]])-lunwind($|[[:space:]]) ]]; then
  die "--static-libunwind already links libunwind.a; remove '-lunwind' from --extra-ldflags"
fi
if [[ "$STATIC_LIBUNWIND" -eq 1 && "$EXTRA_LDFLAGS" == *libunwind.so* ]]; then
  die "--static-libunwind cannot be combined with a libunwind.so path in --extra-ldflags"
fi

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
echo "  runtime_type    = $RUNTIME_TYPE"
echo "  custom_new_delete = $USE_CUSTOM_NEW_DELETE"
echo "  bundle_llvm_static = $BUNDLE_LLVM_STATIC"
echo "  bundle_llvm_needed_static = $BUNDLE_LLVM_NEEDED_STATIC"
echo "  static_libunwind = $STATIC_LIBUNWIND"
echo "  strip_debug = $STRIP_DEBUG"
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
if [[ -n "$LIBUNWIND_LIB_DIR" ]]; then
  echo "  libunwind_lib_dir = $LIBUNWIND_LIB_DIR"
fi
echo "  build_dir       = $BUILD_DIR"
echo

C_FLAGS=""
CXX_FLAGS=""
EXE_LINKER_FLAGS=""
SHARED_LINKER_FLAGS=""
BUNDLE_LINKER_FLAGS=""

if [[ -n "$TARGET_CPU" ]]; then
  C_FLAGS="-mcpu=$TARGET_CPU"
  CXX_FLAGS="-mcpu=$TARGET_CPU"
fi
if [[ "$STRIP_DEBUG" -eq 1 ]]; then
  C_FLAGS="${C_FLAGS:+$C_FLAGS }-g0"
  CXX_FLAGS="${CXX_FLAGS:+$CXX_FLAGS }-g0"
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
  BUNDLE_LINKER_FLAGS="${BUNDLE_LINKER_FLAGS:+$BUNDLE_LINKER_FLAGS }-B$GCC_BIN_DIR"
fi
if [[ -n "$GCC_LIB_DIR" ]]; then
  C_FLAGS="${C_FLAGS:+$C_FLAGS }-B$GCC_LIB_DIR"
  CXX_FLAGS="${CXX_FLAGS:+$CXX_FLAGS }-B$GCC_LIB_DIR"
  EXE_LINKER_FLAGS="${EXE_LINKER_FLAGS:+$EXE_LINKER_FLAGS }-B$GCC_LIB_DIR -L$GCC_LIB_DIR"
  SHARED_LINKER_FLAGS="${SHARED_LINKER_FLAGS:+$SHARED_LINKER_FLAGS }-B$GCC_LIB_DIR -L$GCC_LIB_DIR"
  BUNDLE_LINKER_FLAGS="${BUNDLE_LINKER_FLAGS:+$BUNDLE_LINKER_FLAGS }-B$GCC_LIB_DIR -L$GCC_LIB_DIR"
fi
if [[ "$USE_LLD" -eq 1 ]]; then
  EXE_LINKER_FLAGS="${EXE_LINKER_FLAGS:+$EXE_LINKER_FLAGS }-fuse-ld=lld"
  SHARED_LINKER_FLAGS="${SHARED_LINKER_FLAGS:+$SHARED_LINKER_FLAGS }-fuse-ld=lld"
  BUNDLE_LINKER_FLAGS="${BUNDLE_LINKER_FLAGS:+$BUNDLE_LINKER_FLAGS }-fuse-ld=lld"
fi
if [[ -n "$CXX_STDLIB" && "$CXX_STDLIB" != "none" ]]; then
  CXX_FLAGS="${CXX_FLAGS:+$CXX_FLAGS }-stdlib=$CXX_STDLIB"
  EXE_LINKER_FLAGS="${EXE_LINKER_FLAGS:+$EXE_LINKER_FLAGS }-stdlib=$CXX_STDLIB"
  SHARED_LINKER_FLAGS="${SHARED_LINKER_FLAGS:+$SHARED_LINKER_FLAGS }-stdlib=$CXX_STDLIB"
fi
if [[ -n "$LIBCXX_INCLUDE_DIR" ]]; then
  [[ -d "$LIBCXX_INCLUDE_DIR" ]] || die "libc++ include dir not found: $LIBCXX_INCLUDE_DIR"
  CXX_FLAGS="${CXX_FLAGS:+$CXX_FLAGS }-isystem $LIBCXX_INCLUDE_DIR"
fi
for dir in "$LIBCXX_LIB_DIR" "$LIBCXXABI_LIB_DIR" "$LIBUNWIND_LIB_DIR"; do
  if [[ -n "$dir" ]]; then
    [[ -d "$dir" ]] || die "C++ runtime library dir not found: $dir"
    EXE_LINKER_FLAGS="${EXE_LINKER_FLAGS:+$EXE_LINKER_FLAGS }-L$dir"
    SHARED_LINKER_FLAGS="${SHARED_LINKER_FLAGS:+$SHARED_LINKER_FLAGS }-L$dir"
  fi
done
if [[ "$STATIC_LIBUNWIND" -eq 1 ]]; then
  STATIC_LIBUNWIND_PATH="$(resolve_static_lib libunwind.a "$LIBUNWIND_LIB_DIR" || true)"
  [[ -n "$STATIC_LIBUNWIND_PATH" ]] || die "libunwind.a not found in --libunwind-lib-dir: $LIBUNWIND_LIB_DIR"
  EXE_LINKER_FLAGS="${EXE_LINKER_FLAGS:+$EXE_LINKER_FLAGS }$STATIC_LIBUNWIND_PATH"
  SHARED_LINKER_FLAGS="${SHARED_LINKER_FLAGS:+$SHARED_LINKER_FLAGS }$STATIC_LIBUNWIND_PATH"
  echo "  static_libunwind_path = $STATIC_LIBUNWIND_PATH"
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
  -DBUILD_SHARED_LIBS=OFF
  -DLLVM_BUILD_LLVM_DYLIB=OFF
  -DLLVM_LINK_LLVM_DYLIB=OFF
  -DEASY_JIT_BUILD_PASS=OFF
  -DEASY_JIT_RUNTIME_TYPE="${RUNTIME_TYPE^^}"
  -DEASYJIT_USE_CUSTOM_NEW_DELETE="$USE_CUSTOM_NEW_DELETE"
  -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"
  -DCMAKE_SYSTEM_NAME=Linux
  -DCMAKE_SYSTEM_PROCESSOR="${TARGET_TRIPLE%%-*}"
  -DCMAKE_C_COMPILER="$HOST_CLANG"
  -DCMAKE_CXX_COMPILER="$HOST_CLANGXX"
  -DCMAKE_C_COMPILER_TARGET="$TARGET_TRIPLE"
  -DCMAKE_CXX_COMPILER_TARGET="$TARGET_TRIPLE"
  -DCMAKE_SYSROOT="$SYSROOT"
  -DCMAKE_SKIP_RPATH=ON
  -DCMAKE_C_FLAGS="$C_FLAGS"
  -DCMAKE_CXX_FLAGS="$CXX_FLAGS"
  -DCMAKE_EXE_LINKER_FLAGS="$EXE_LINKER_FLAGS"
  -DCMAKE_SHARED_LINKER_FLAGS="$SHARED_LINKER_FLAGS"
)

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G Ninja "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" --target EasyJitRuntime --parallel "$JOBS"

if [[ "$RUNTIME_TYPE" == "static" ]]; then
  RUNTIME_OUTPUT="$BUILD_DIR/bin/libEasyJitRuntime.a"
  [[ -f "$RUNTIME_OUTPUT" ]] || die "static runtime not found: $RUNTIME_OUTPUT"
  strip_debug_file "$RUNTIME_OUTPUT"
  if [[ "$BUNDLE_LLVM_STATIC" -eq 1 ]]; then
    bundle_static_runtime_with_llvm "$RUNTIME_OUTPUT"
  fi
  if [[ "$BUNDLE_LLVM_NEEDED_STATIC" -eq 1 ]]; then
    bundle_static_runtime_with_needed_llvm "$RUNTIME_OUTPUT"
  fi
else
  RUNTIME_OUTPUT="$BUILD_DIR/bin/libEasyJitRuntime.so"
  assert_runtime_is_not_linked_against_llvm_shared "$RUNTIME_OUTPUT"
fi

echo "==> Done"
echo "  runtime : $RUNTIME_OUTPUT"
