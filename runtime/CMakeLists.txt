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

  # Convenience runner. The lit-based `check` target only knows about
  # tests under tests/, but this is a pure compile-time emitter test
  # that does not need clang/EasyJitPass — keeping it as a small
  # custom target avoids dragging Python/lit into the dependency chain
  # and lets developers run it directly:
  #     cmake --build <dir> --target check-light-endian
  add_custom_target(check-light-endian
    COMMAND $<TARGET_FILE:light_endian_parity_test>
    DEPENDS light_endian_parity_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running light backend endian/triple parity test"
    VERBATIM)

  # FP capability test — covers the round-8f f32 arithmetic / fcmp +
  # branch / fcmp + select paths AND the round-8g hardening of fcmp
  # deferred-operand lifetime (stale-S31 reproducer). On AArch64 hosts
  # the emitted code is mmap+executed; on non-AArch64 hosts only the
  # emit step + opcode-mask coverage guards run, which is still useful
  # cross-build coverage.
  add_executable(light_fp_ops_test
    ${CMAKE_SOURCE_DIR}/light_codegen/test_fp_ops.cpp
    ${CMAKE_SOURCE_DIR}/light_codegen/light_aarch64.cpp)
  target_include_directories(light_fp_ops_test PRIVATE
    ${CMAKE_SOURCE_DIR}/light_codegen)
  llvm_config(light_fp_ops_test core support)
  set_target_properties(light_fp_ops_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

  add_custom_target(check-light-fp
    COMMAND $<TARGET_FILE:light_fp_ops_test>
    DEPENDS light_fp_ops_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running light backend f32 ops test"
    VERBATIM)

  # Stack-argument test (round-8j) — covers AAPCS64 NSAA layout for
  # the 9th and later scalar args (int/ptr/float/double). On AArch64
  # hosts the C++ caller arranges the stack layout per AAPCS64; the
  # emitted code reads from `[sp, frameSize+incomingOff]` after the
  # entry prologue. On non-AArch64 hosts only the emit step is run.
  add_executable(light_stack_args_test
    ${CMAKE_SOURCE_DIR}/light_codegen/test_stack_args.cpp
    ${CMAKE_SOURCE_DIR}/light_codegen/light_aarch64.cpp)
  target_include_directories(light_stack_args_test PRIVATE
    ${CMAKE_SOURCE_DIR}/light_codegen)
  llvm_config(light_stack_args_test core support)
  set_target_properties(light_stack_args_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

  add_custom_target(check-light-stack-args
    COMMAND $<TARGET_FILE:light_stack_args_test>
    DEPENDS light_stack_args_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running light backend stack-arg test"
    VERBATIM)

  # Round 8k: dynamic GEP with up to 2 dynamic terms (`base[i][j]`).
  # Validates the extended PtrLoc::*ScaledIndex form, the per-term
  # ADD-extended-register or extend+LSL+ADD address sequence, and
  # multi-index struct GEP folding. End-to-end correctness on AArch64
  # hosts; emit-only on others.
  add_executable(light_dynamic_gep_test
    ${CMAKE_SOURCE_DIR}/light_codegen/test_dynamic_gep.cpp
    ${CMAKE_SOURCE_DIR}/light_codegen/light_aarch64.cpp)
  target_include_directories(light_dynamic_gep_test PRIVATE
    ${CMAKE_SOURCE_DIR}/light_codegen)
  llvm_config(light_dynamic_gep_test core support)
  set_target_properties(light_dynamic_gep_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

  add_custom_target(check-light-dynamic-gep
    COMMAND $<TARGET_FILE:light_dynamic_gep_test>
    DEPENDS light_dynamic_gep_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running light backend dynamic-GEP test"
    VERBATIM)
endif()
