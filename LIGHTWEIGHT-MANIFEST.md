# Lightweight EasyJIT - Removable Directories Manifest
#
# This file documents which directories/files from the LLVM monorepo
# can be safely removed when creating a lightweight EasyJIT distribution.
#
# Categories:
#   REMOVE  - Safe to delete; not needed by EasyJIT at all
#   KEEP    - Required for EasyJIT build and runtime
#   TRIM    - Keep directory but remove sub-items (tests, docs, examples)

# ============================================================
# TOP-LEVEL PROJECTS
# ============================================================

# KEEP: llvm/           - Core LLVM libraries (IR, codegen, passes, MCJIT, etc.)
# KEEP: clang/          - Clang compiler (needed to load EasyJitPass plugin)
# KEEP: easy-jit/       - EasyJIT itself
# KEEP: cmake/          - Shared CMake modules

# REMOVE: bolt/                 - Binary optimization tool (not needed)
# REMOVE: clang-tools-extra/    - Extra clang tools (clang-tidy etc.)
# REMOVE: compiler-rt/          - Compiler runtime (sanitizers etc.)
# REMOVE: cross-project-tests/  - Cross-project test suites
# REMOVE: flang/                - Fortran frontend
# REMOVE: libc/                 - LLVM C library
# REMOVE: libclc/               - OpenCL C library
# REMOVE: libcxx/               - C++ standard library
# REMOVE: libcxxabi/            - C++ ABI library
# REMOVE: libunwind/            - Unwinding library
# REMOVE: lld/                  - LLVM linker
# REMOVE: lldb/                 - LLVM debugger
# REMOVE: llvm-libgcc/          - libgcc replacement
# REMOVE: mlir/                 - Multi-Level IR framework
# REMOVE: offload/              - GPU offloading runtime
# REMOVE: openmp/               - OpenMP runtime
# REMOVE: polly/                - Polyhedral loop optimizer
# REMOVE: pstl/                 - Parallel STL
# REMOVE: runtimes/             - Runtimes umbrella
# REMOVE: third-party/          - Third-party dependencies (benchmark, etc.)
# REMOVE: utils/                - Miscellaneous utility scripts

# ============================================================
# WITHIN llvm/ - TRIMMABLE CONTENT
# ============================================================

# REMOVE: llvm/test/                    - LLVM test suite (~300MB)
# REMOVE: llvm/unittests/               - LLVM unit tests
# REMOVE: llvm/examples/                - LLVM examples
# REMOVE: llvm/docs/                    - LLVM documentation
# REMOVE: llvm/benchmarks/              - LLVM benchmarks
# REMOVE: llvm/bindings/                - Language bindings (Python, Go, OCaml)
# REMOVE: llvm/tools/bugpoint/          - Bugpoint
# REMOVE: llvm/tools/dsymutil/          - dSYM utility
# REMOVE: llvm/tools/gold/              - Gold linker plugin
# REMOVE: llvm/tools/llvm-exegesis/     - LLVM exegesis
# REMOVE: llvm/tools/llvm-mca/          - Machine code analyzer
# REMOVE: llvm/tools/llvm-reduce/       - Test case reducer
# REMOVE: llvm/tools/llvm-xray/         - XRay tools

# KEEP:   llvm/lib/                     - Core libraries
# KEEP:   llvm/include/                 - Core headers
# KEEP:   llvm/cmake/                   - CMake build infrastructure
# KEEP:   llvm/tools/llvm-config/       - llvm-config tool
# KEEP:   llvm/tools/llc/               - LLC (if needed for testing)
# KEEP:   llvm/tools/opt/               - opt tool
# KEEP:   llvm/CMakeLists.txt           - Top-level CMake

# ============================================================
# WITHIN llvm/lib/Target/ - BACKEND TRIMMING
# ============================================================
# Only the native target backend is needed.
# For AArch64 builds, remove all other backends:
#
# REMOVE: llvm/lib/Target/X86/
# REMOVE: llvm/lib/Target/ARM/
# REMOVE: llvm/lib/Target/RISCV/
# REMOVE: llvm/lib/Target/Mips/
# REMOVE: llvm/lib/Target/PowerPC/
# REMOVE: llvm/lib/Target/SystemZ/
# REMOVE: llvm/lib/Target/Hexagon/
# REMOVE: llvm/lib/Target/AMDGPU/
# REMOVE: llvm/lib/Target/NVPTX/
# REMOVE: llvm/lib/Target/WebAssembly/
# REMOVE: llvm/lib/Target/Sparc/
# REMOVE: llvm/lib/Target/Lanai/
# REMOVE: llvm/lib/Target/BPF/
# REMOVE: llvm/lib/Target/AVR/
# REMOVE: llvm/lib/Target/MSP430/
# REMOVE: llvm/lib/Target/XCore/
# REMOVE: llvm/lib/Target/VE/
# REMOVE: llvm/lib/Target/CSKY/
# REMOVE: llvm/lib/Target/LoongArch/
# REMOVE: llvm/lib/Target/M68k/
# REMOVE: llvm/lib/Target/DirectX/
# REMOVE: llvm/lib/Target/SPIRV/
# REMOVE: llvm/lib/Target/Xtensa/
# REMOVE: llvm/lib/Target/ARC/

# ============================================================
# WITHIN clang/ - TRIMMABLE CONTENT
# ============================================================

# REMOVE: clang/test/              - Clang test suite
# REMOVE: clang/unittests/         - Clang unit tests
# REMOVE: clang/docs/              - Clang documentation
# REMOVE: clang/examples/          - Clang examples
# REMOVE: clang/bindings/          - Clang bindings
# REMOVE: clang/tools/scan-build/  - Scan-build tool
# REMOVE: clang/tools/scan-view/   - Scan-view tool
# REMOVE: clang/www/               - Clang website content

# ============================================================
# EASYJIT REQUIRED LLVM COMPONENTS (from llvm_config call)
# ============================================================
# core, codegen, interpreter, support, mcjit, native,
# executionengine, passes, objcarcopts
#
# Transitive dependencies (resolved by llvm-config):
# analysis, asmparser, asmprinter, binaryformat, bitreader,
# bitstreamreader, bitwriter, cfguard, codegentypes,
# coroutines, debuginfobtf, debuginfocodeview, debuginfodwarf,
# debuginfomsf, debuginfopdb, demangle, frontendoffloading,
# frontendopenmp, globalisel, hipstdpar, instcombine,
# instrumentation, ipo, irprinter, irreader, linker, mc,
# mcdisassembler, mcparser, object, orcshared, orctargetprocess,
# profiledata, remarks, runtimedyld, scalaropts, selectiondag,
# symbolize, target, targetparser, textapi, transformutils, vectorize
