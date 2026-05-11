# examples/easyjit_cpp_minimal/CMakeLists.txt
#
# Minimal standalone CMake template for linking an application against
# EasyJIT in C++ mode. This file is intentionally self-contained: it
# does NOT include or `add_subdirectory()` anything from the EasyJIT
# tree. Copy this directory into your own project, point the three
# cache variables below at your EasyJIT install, and you're done.
#
# Required cache variables:
#   EASYJIT_ROOT   Path to the easy-jit-main source tree (for headers).
#   EASYJIT_LIB    Path to the EasyJIT runtime library that you intend
#                  to link against. This is usually a fat static archive
#                  (libEasyJitRuntimeWithNeededLLVM.a) on a target board
#                  or libEasyJitRuntime.so during host development.
#   EASYJIT_PASS   Path to EasyJitPass.so. The plugin must be loaded by
#                  clang at compile time so that the bitcode of every
#                  EASY_JIT_EXPOSE function is embedded in the object.
#
# Required toolchain:
#   * The C++ compiler MUST be the clang that ships with the same LLVM
#     version EasyJitPass.so was built against (LLVM 15 by default).
#     Using a different clang -> plugin ABI mismatch -> the plugin will
#     fail to load.

cmake_minimum_required(VERSION 3.13)
project(easyjit_cpp_minimal CXX)

set(EASYJIT_ROOT "" CACHE PATH
    "Path to easy-jit-main source tree (contains include/easy/*.h)")
set(EASYJIT_LIB  "" CACHE FILEPATH
    "Path to EasyJIT runtime library (.a or .so)")
set(EASYJIT_PASS "" CACHE FILEPATH
    "Path to EasyJitPass.so (loaded by clang at compile time)")

foreach(_var EASYJIT_ROOT EASYJIT_LIB EASYJIT_PASS)
    if(NOT ${_var})
        message(FATAL_ERROR
            "${_var} is not set. Configure with:\n"
            "  cmake -S . -B build \\\n"
            "    -DEASYJIT_ROOT=/path/to/easy-jit-main \\\n"
            "    -DEASYJIT_LIB=/path/to/libEasyJitRuntime{.a|.so} \\\n"
            "    -DEASYJIT_PASS=/path/to/EasyJitPass.so \\\n"
            "    -DCMAKE_CXX_COMPILER=/path/to/llvm15/bin/clang++")
    endif()
endforeach()

if(NOT EXISTS "${EASYJIT_ROOT}/include/easy/jit.h")
    message(FATAL_ERROR
        "EASYJIT_ROOT does not look like an easy-jit-main checkout: "
        "${EASYJIT_ROOT}/include/easy/jit.h is missing.")
endif()

add_executable(easyjit_cpp_minimal main.cpp)

# 1. Headers.
target_include_directories(easyjit_cpp_minimal PRIVATE
    ${EASYJIT_ROOT}/include)

# 2. EasyJitPass.so MUST be loaded by clang at compile time. This is
#    what embeds the bitcode of EASY_JIT_EXPOSE-marked functions into
#    the resulting object file. Without this flag, easy::jit() at
#    runtime will fail to find any bitcode and abort.
target_compile_options(easyjit_cpp_minimal PRIVATE
    -Xclang -fpass-plugin=${EASYJIT_PASS}
    -Xclang -disable-O0-optnone)

# 3. Link against the EasyJIT runtime + the system libs it needs.
#    Order matters when EASYJIT_LIB is a static archive: pthread and dl
#    must come AFTER it so the linker can resolve symbols.
target_link_libraries(easyjit_cpp_minimal PRIVATE
    ${EASYJIT_LIB}
    pthread
    dl)

# 4. C++17 + force C++ link (matters when EASYJIT_LIB is a .a containing
#    C++ globals: a plain `cc` driver would not pull in libstdc++).
set_target_properties(easyjit_cpp_minimal PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    LINKER_LANGUAGE CXX)
