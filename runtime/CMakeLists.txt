option(EASYJIT_USE_CUSTOM_NEW_DELETE
       "Route global C++ new/delete through the platform allocation hooks"
       OFF)
set(EASY_JIT_RUNTIME_TYPE "SHARED" CACHE STRING
    "Build EasyJitRuntime as SHARED or STATIC")
set_property(CACHE EASY_JIT_RUNTIME_TYPE PROPERTY STRINGS SHARED STATIC)
if (NOT EASY_JIT_RUNTIME_TYPE MATCHES "^(SHARED|STATIC)$")
  message(FATAL_ERROR "EASY_JIT_RUNTIME_TYPE must be SHARED or STATIC")
endif()

add_library(EasyJitRuntime ${EASY_JIT_RUNTIME_TYPE}
  BitcodeTracker.cpp
  Context.cpp
  Function.cpp
  InitNativeTarget.cpp
  Utils.cpp
  custom_new_delete.cpp
  easyjit_c.cpp
  pass/ContextAnalysis.cpp
  pass/DevirtualizeConstant.cpp
  pass/InlineParameters.cpp
  pass/InlineParametersHelper.cpp
  pass/ConstStructPropagate.cpp
)
target_compile_definitions(EasyJitRuntime PRIVATE
  EASYJIT_USE_CUSTOM_NEW_DELETE=$<IF:$<BOOL:${EASYJIT_USE_CUSTOM_NEW_DELETE}>,1,0>)

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
