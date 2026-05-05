option(EASYJIT_USE_CUSTOM_NEW_DELETE
       "Route global C++ new/delete through the platform allocation hooks"
       OFF)
set(EASY_JIT_RUNTIME_TYPE "SHARED" CACHE STRING
    "Build EasyJitRuntime as SHARED or STATIC")
set_property(CACHE EASY_JIT_RUNTIME_TYPE PROPERTY STRINGS SHARED STATIC)
if (NOT EASY_JIT_RUNTIME_TYPE MATCHES "^(SHARED|STATIC)$")
  message(FATAL_ERROR "EASY_JIT_RUNTIME_TYPE must be SHARED or STATIC")
endif()

# Detect target processor (host or cross). Default LightBackend ON only on
# AArch64-family targets where the lightweight emitter actually supports
# the instruction set.
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _easyjit_lower_proc)
if (_easyjit_lower_proc MATCHES "^(aarch64|aarch64_be|arm64)$")
  set(_easyjit_light_default ON)
else()
  set(_easyjit_light_default OFF)
endif()
option(EASYJIT_ENABLE_LIGHT_BACKEND
       "Build the lightweight AArch64 runtime backend (LightBackend)"
       ${_easyjit_light_default})

# Light-only mode: completely drop MinimalOrcJIT/ORC/CodeGen/MC from the
# runtime. Only the lightweight AArch64 emitter is compiled in. Intended
# for embedded / strictly controlled workloads where unsupported IR must
# fail loudly instead of falling back through ORC.
option(EASYJIT_LIGHT_BACKEND_ONLY
       "Light-only runtime build: no ORC fallback, no CodeGen/MC"
       OFF)
if (EASYJIT_LIGHT_BACKEND_ONLY)
  if (NOT _easyjit_lower_proc MATCHES "^(aarch64|aarch64_be|arm64)$")
    message(FATAL_ERROR
      "EASYJIT_LIGHT_BACKEND_ONLY=ON requires an AArch64 target "
      "(CMAKE_SYSTEM_PROCESSOR='${CMAKE_SYSTEM_PROCESSOR}')")
  endif()
  set(EASYJIT_ENABLE_LIGHT_BACKEND ON CACHE BOOL
      "Forced ON by EASYJIT_LIGHT_BACKEND_ONLY" FORCE)
endif()

set(EASYJIT_RUNTIME_SOURCES
  BitcodeTracker.cpp
  Context.cpp
  Function.cpp
  Utils.cpp
  custom_new_delete.cpp
  easyjit_c.cpp
  pass/ContextAnalysis.cpp
  pass/DevirtualizeConstant.cpp
  pass/InlineParameters.cpp
  pass/InlineParametersHelper.cpp
  pass/ConstStructPropagate.cpp
)
if (NOT EASYJIT_LIGHT_BACKEND_ONLY)
  # Full mode keeps the ORC backend and native-target initialization.
  list(APPEND EASYJIT_RUNTIME_SOURCES
    InitNativeTarget.cpp
    MinimalOrcJIT.cpp)
endif()
if (EASYJIT_ENABLE_LIGHT_BACKEND)
  list(APPEND EASYJIT_RUNTIME_SOURCES
    LightBackend.cpp
    ${CMAKE_SOURCE_DIR}/light_codegen/light_aarch64.cpp)
endif()

add_library(EasyJitRuntime ${EASY_JIT_RUNTIME_TYPE} ${EASYJIT_RUNTIME_SOURCES})
target_compile_definitions(EasyJitRuntime PRIVATE
  EASYJIT_USE_CUSTOM_NEW_DELETE=$<IF:$<BOOL:${EASYJIT_USE_CUSTOM_NEW_DELETE}>,1,0>
  EASYJIT_LIGHT_BACKEND_ENABLED=$<IF:$<BOOL:${EASYJIT_ENABLE_LIGHT_BACKEND}>,1,0>
  EASYJIT_LIGHT_BACKEND_ONLY=$<IF:$<BOOL:${EASYJIT_LIGHT_BACKEND_ONLY}>,1,0>)

message(STATUS "EasyJIT LightBackend: ${EASYJIT_ENABLE_LIGHT_BACKEND} (target processor: ${CMAKE_SYSTEM_PROCESSOR})")
message(STATUS "EasyJIT LightBackendOnly: ${EASYJIT_LIGHT_BACKEND_ONLY}")

if (EASYJIT_LIGHT_BACKEND_ONLY)
  # Minimal LLVM component closure for light-only mode: no codegen, no
  # MC, no ORC, no native target. Only enough to load bitcode and run
  # the IR-level pass pipeline.
  llvm_config(EasyJitRuntime
    analysis
    bitreader
    bitwriter
    core
    ipo
    linker
    scalaropts
    support
    transformutils)
else()
  llvm_config(EasyJitRuntime
    analysis
    bitreader
    bitwriter
    core
    codegen
    instcombine
    ipo
    linker
    native
    nativecodegen
    orcjit
    scalaropts
    support
    transformutils)
endif()


if (EASY_JIT_RUNTIME_TYPE STREQUAL "STATIC")
  set(EASY_JIT_RUNTIME ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}/${CMAKE_STATIC_LIBRARY_PREFIX}EasyJitRuntime${CMAKE_STATIC_LIBRARY_SUFFIX} PARENT_SCOPE)
else()
  set(EASY_JIT_RUNTIME ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/${CMAKE_SHARED_LIBRARY_PREFIX}EasyJitRuntime${CMAKE_SHARED_LIBRARY_SUFFIX} PARENT_SCOPE)
endif()

install(TARGETS EasyJitRuntime 
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib)

# Optional: a tiny standalone unit test that calls light::emit directly
# under several triples (aarch64-*, aarch64_be-*, arm64-*, plus the
# rejected ILP32 / non-aarch64 triples). It does not depend on
# EasyJitRuntime, only on light_aarch64.cpp + LLVM IR; lets us check the
# round-8 BE-enablement claim without building the whole pass plugin.
# Built only when the light backend itself is enabled.
if (EASYJIT_ENABLE_LIGHT_BACKEND)
  add_executable(light_endian_parity_test
    ${CMAKE_SOURCE_DIR}/light_codegen/test_endian_parity.cpp
    ${CMAKE_SOURCE_DIR}/light_codegen/light_aarch64.cpp)
  target_include_directories(light_endian_parity_test PRIVATE
    ${CMAKE_SOURCE_DIR}/light_codegen)
  llvm_config(light_endian_parity_test core support)
  set_target_properties(light_endian_parity_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
