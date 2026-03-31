# ── Collect LLVM static libraries needed by EasyJIT runtime ──────
# We statically link LLVM into libeasyjit.so so users never need LLVM.

# Map LLVM component names to library targets
llvm_map_components_to_libnames(LLVM_LIBS
  core codegen interpreter support mcjit native
  executionengine passes objcarcopts
  linker bitreader bitwriter ipo
  asmparser asmprinter
  selectiondag
  target
)

# Build the single user-facing library
add_library(EasyJitRuntime SHARED
  BitcodeTracker.cpp
  Context.cpp
  Function.cpp
  InitNativeTarget.cpp
  Utils.cpp
  easyjit_c.cpp
  pass/ContextAnalysis.cpp
  pass/DevirtualizeConstant.cpp
  pass/InlineParameters.cpp
  pass/InlineParametersHelper.cpp
)

# Include the internal headers directory
target_include_directories(EasyJitRuntime PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# LLVM is a PRIVATE dependency — not exposed to users
target_link_libraries(EasyJitRuntime PRIVATE ${LLVM_LIBS})

# Hide all LLVM symbols from the .so's dynamic symbol table
# We use a version script instead of compiler visibility settings
# so that easy::* symbols remain visible while LLVM symbols are hidden

# However, we need easy::* and easy_register* symbols to be visible
# We'll use a version script to export only what we need
set(VERSION_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/easyjit.map")
file(WRITE ${VERSION_SCRIPT} "{
  global:
    easy_register;
    easy_register_layout;
    easyjit_*;
    extern \"C++\" {
      easy::Function::*;
      easy::Context::*;
      easy::FunctionWrapper*;
      easy::FunctionWrapperBase*;
      easy::Cache*;
      easy::BitcodeTracker::*;
      easy::LLVMHolder*;
      easy::exception*;
      easy::ForwardArgument*;
      easy::IntArgument*;
      easy::FloatArgument*;
      easy::PtrArgument*;
      easy::StructArgument*;
      easy::ArrayArgument*;
      easy::ModuleArgument*;
      easy::ArgumentBase*;
      easy::serialized_arg*;
      *hash*easy*Function*;
      *hash*easy*Context*;
      *hash*easy*ArgumentBase*;
    };
  local:
    *;
};
")
target_link_options(EasyJitRuntime PRIVATE
  "-Wl,--version-script=${VERSION_SCRIPT}"
)

set(EASY_JIT_RUNTIME ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/${CMAKE_SHARED_LIBRARY_PREFIX}EasyJitRuntime${CMAKE_SHARED_LIBRARY_SUFFIX} PARENT_SCOPE)

install(TARGETS EasyJitRuntime 
        LIBRARY DESTINATION lib)
