cmake_minimum_required(VERSION 3.12)
set(LLVM_SUBPROJECT_TITLE "EasyJIT")

# Check for a standalone build and configure as appropriate from
# there.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  message("Building EasyJIT as a standalone project.")
  project(EasyJIT)
  set(EASY_JIT_STANDALONE_BUILD ON)
  option(EASY_JIT_INCLUDE_TESTS "Generate build targets for the EasyJIT tests." ON)
  option(EASY_JIT_INCLUDE_DOCS "Generate build targets for the EasyJIT docs." OFF)
  option(EASY_JIT_EXAMPLE "Generate build targets for the EasyJIT examples." OFF)
  option(EASY_JIT_INCLUDE_BENCHMARKS "Generate build targets for the EasyJIT benchmarks." OFF)
  option(EASY_JIT_BENCHMARK "Build the EasyJIT benchmarks." OFF)
else()
  set(EASY_JIT_STANDALONE_BUILD OFF)
  option(EASY_JIT_INCLUDE_TESTS "Generate build targets for the EasyJIT tests." ${LLVM_INCLUDE_TESTS})
  option(EASY_JIT_INCLUDE_DOCS "Generate build targets for the EasyJIT docs." ${LLVM_INCLUDE_DOCS})
  option(EASY_JIT_EXAMPLE "Generate build targets for the EasyJIT examples." ${LLVM_BUILD_EXAMPLES})
  option(EASY_JIT_INCLUDE_BENCHMARKS "Generate build targets for the EasyJIT benchmarks." ${LLVM_INCLUDE_BENCHMARKS})
  option(EASY_JIT_BENCHMARK "Build the EasyJIT benchmarks." ${LLVM_BUILD_BENCHMARKS})
endif()

# set(CMAKE_SHARED_LIBRARY_PREFIX_CXX "")

if("${CMAKE_CURRENT_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_BINARY_DIR}")
  message(FATAL_ERROR "Do not set the build directory equal to the source directory!")
endif()

message(STATUS "CPU Architecture: ${CMAKE_HOST_SYSTEM_PROCESSOR}")
if (${CMAKE_HOST_SYSTEM_PROCESSOR} MATCHES "x86_64")
	add_definitions(-D_X86)
elseif (${CMAKE_HOST_SYSTEM_PROCESSOR} MATCHES "aarch64")
	add_definitions(-D_AARCH64)
endif()

if(EASY_JIT_STANDALONE_BUILD)
  # We need a pre-built/installed version of LLVM.
  find_package(LLVM 19.1 REQUIRED HINTS ${LLVM_DIR})
  message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
  message(STATUS "Using LLVMConfig.cmake in ${LLVM_DIR}")
  message(STATUS "LLVM Root: ${LLVM_TOOLS_BINARY_DIR}")
  message(STATUS "LLVM Include dirs: ${LLVM_INCLUDE_DIRS}")
  message(STATUS "LLVM Definitions: ${LLVM_DEFINITIONS}")
  # Find llvm-lit if LLVM_DIR did not provide it
  if(NOT LLVM_EXTERNAL_LIT AND EASY_JIT_INCLUDE_TESTS)
    find_program(LIT_EXECUTABLE NAMES llvm-lit lit
                  HINTS ${LLVM_TOOLS_BINARY_DIR})
    if(LIT_EXECUTABLE)
      set(LLVM_EXTERNAL_LIT ${LIT_EXECUTABLE} CACHE FILEPATH "Path to lit executable")
      message(STATUS "Found lit executable: ${LLVM_EXTERNAL_LIT}")
    else()
      message(FATAL_ERROR "lit or llvm-lit not found. Please set LLVM_EXTERNAL_LIT to the path of lit.")
    endif()
  endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")
include(AddLLVM)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIRS})
include_directories(include)
add_compile_options(-fno-rtti)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(EASY_JIT_STANDALONE_BUILD)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
endif()

set(EASY_JIT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# Suppress superfluous randlib warnings about "*.a" having no symbols on MacOSX.
if (APPLE)
	set(CMAKE_C_ARCHIVE_CREATE   "<CMAKE_AR> Scr <TARGET> <LINK_FLAGS> <OBJECTS>")
	set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> Scr <TARGET> <LINK_FLAGS> <OBJECTS>")
	set(CMAKE_C_ARCHIVE_FINISH   "<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>")
	set(CMAKE_CXX_ARCHIVE_FINISH "<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>")
endif()

add_subdirectory(cmake)
add_subdirectory(include)
add_subdirectory(pass)
add_subdirectory(runtime)

add_custom_target(easy-jit-core DEPENDS EasyJitPass EasyJitRuntime)

if(EASY_JIT_INCLUDE_DOCS)
  add_subdirectory(doc)
endif()
  
if(EASY_JIT_INCLUDE_BENCHMARKS)
  add_subdirectory(benchmark)
endif()

if(EASY_JIT_INCLUDE_TESTS)
  include(CMakeTests.txt)
endif()
