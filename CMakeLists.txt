cmake_minimum_required(VERSION 3.4.3)

project(easyjit)

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

find_package(LLVM 15.0 REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in ${LLVM_DIR}")
message(STATUS "LLVM Root: ${LLVM_TOOLS_BINARY_DIR}")
message(STATUS "LLVM Include dirs: ${LLVM_INCLUDE_DIRS}")
message(STATUS "LLVM Definitions: ${LLVM_DEFINITIONS}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")
include(AddLLVM)
include(Python)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIRS})
include_directories(include)
add_compile_options(-fno-rtti -gdwarf-4)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)

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

add_subdirectory(doc)
add_subdirectory(benchmark)

include(CMakeTests.txt)
