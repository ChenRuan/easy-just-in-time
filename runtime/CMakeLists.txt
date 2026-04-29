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

set(EASYJIT_RUNTIME_SOURCES
  BitcodeTracker.cpp
  Context.cpp
  Function.cpp
  InitNativeTarget.cpp
  MinimalOrcJIT.cpp
  Utils.cpp
  custom_new_delete.cpp
  easyjit_c.cpp
  pass/ContextAnalysis.cpp
  pass/DevirtualizeConstant.cpp
  pass/InlineParameters.cpp
  pass/InlineParametersHelper.cpp
  pass/ConstStructPropagate.cpp
)
if (EASYJIT_ENABLE_LIGHT_BACKEND)
  list(APPEND EASYJIT_RUNTIME_SOURCES
    LightBackend.cpp
    ${CMAKE_SOURCE_DIR}/light_codegen/light_aarch64.cpp)
endif()

add_library(EasyJitRuntime ${EASY_JIT_RUNTIME_TYPE} ${EASYJIT_RUNTIME_SOURCES})
target_compile_definitions(EasyJitRuntime PRIVATE
  EASYJIT_USE_CUSTOM_NEW_DELETE=$<IF:$<BOOL:${EASYJIT_USE_CUSTOM_NEW_DELETE}>,1,0>
  EASYJIT_LIGHT_BACKEND_ENABLED=$<IF:$<BOOL:${EASYJIT_ENABLE_LIGHT_BACKEND}>,1,0>)

message(STATUS "EasyJIT LightBackend: ${EASYJIT_ENABLE_LIGHT_BACKEND} (target processor: ${CMAKE_SYSTEM_PROCESSOR})")

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


if (EASY_JIT_RUNTIME_TYPE STREQUAL "STATIC")
  set(EASY_JIT_RUNTIME ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}/${CMAKE_STATIC_LIBRARY_PREFIX}EasyJitRuntime${CMAKE_STATIC_LIBRARY_SUFFIX} PARENT_SCOPE)
else()
  set(EASY_JIT_RUNTIME ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/${CMAKE_SHARED_LIBRARY_PREFIX}EasyJitRuntime${CMAKE_SHARED_LIBRARY_SUFFIX} PARENT_SCOPE)
endif()

install(TARGETS EasyJitRuntime 
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib)
