if(EASY_JIT_BENCHMARK)
    set(CMAKE_CXX_COMPILER ${LLVM_TOOLS_BINARY_DIR}/clang++)
    set(CMAKE_CXX_FLAGS "")
    set(EASYJIT_PLUGIN_FLAGS -Xclang -disable-O0-optnone -Xclang -load -Xclang ${EASY_JIT_PASS} -Xclang -fpass-plugin=${EASY_JIT_PASS})

    if(EASY_JIT_STANDALONE_BUILD)
        add_executable(easyjit-benchmark benchmark.cpp)
        target_compile_options(easyjit-benchmark PRIVATE ${EASYJIT_PLUGIN_FLAGS})
        find_package(benchmark CONFIG REQUIRED)
        link_directories(${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
        target_link_libraries (easyjit-benchmark benchmark::benchmark)
        target_link_libraries (easyjit-benchmark EasyJitRuntime pthread)
        target_link_options (easyjit-benchmark PRIVATE "-Wl,-rpath,$<TARGET_FILE_DIR:EasyJitRuntime>")
        set_output_directory(easyjit-benchmark BINARY_DIR ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY})
    else()
        add_benchmark(easyjit-benchmark benchmark.cpp)
        target_compile_options(easyjit-benchmark PRIVATE ${EASYJIT_PLUGIN_FLAGS})
        target_link_libraries (easyjit-benchmark PRIVATE EasyJitRuntime pthread)
        target_link_options (easyjit-benchmark PRIVATE "-Wl,-rpath,$<TARGET_FILE_DIR:EasyJitRuntime>")
        set_output_directory(easyjit-benchmark BINARY_DIR ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY})
    endif()

    add_dependencies(easyjit-benchmark easy-jit-core)

    # Put the benchmark executable alongside libraries in monorepo builds.
    if(NOT EASY_JIT_STANDALONE_BUILD)
      set_target_properties(easyjit-benchmark PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${LLVM_LIBRARY_OUTPUT_INTDIR}")
    endif()

endif()
