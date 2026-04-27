option(EASYJIT_USE_CUSTOM_NEW_DELETE
       "Route global C++ new/delete through the platform allocation hooks"
       OFF)

add_library(EasyJitRuntime SHARED
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
)
target_compile_definitions(EasyJitRuntime PRIVATE
  EASYJIT_USE_CUSTOM_NEW_DELETE=$<IF:$<BOOL:${EASYJIT_USE_CUSTOM_NEW_DELETE}>,1,0>)

llvm_config(EasyJitRuntime
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
  orctargetprocess)


set(EASY_JIT_RUNTIME ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/${CMAKE_SHARED_LIBRARY_PREFIX}EasyJitRuntime${CMAKE_SHARED_LIBRARY_SUFFIX} PARENT_SCOPE)

install(TARGETS EasyJitRuntime 
        LIBRARY DESTINATION lib)
