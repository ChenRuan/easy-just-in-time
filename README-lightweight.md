# Lightweight EasyJIT# Lightweight EasyJIT



> 从 LLVM monorepo 中抽离 EasyJIT 的轻量化构建方案A minimal extraction of **EasyJIT** from the LLVM monorepo, designed to build

and run with only the essential LLVM components.

## 概述

## Quick Start

本分支 (`lightweight-easyjit`) 将 EasyJIT 从完整的 LLVM monorepo 中抽离，裁剪掉不相关的项目、测试、文档和目标后端，使其成为可独立构建的轻量化版本。

### Option 1: Full build (LLVM + EasyJIT)

**关键数据：**

```bash

| 指标 | 原始 monorepo | 轻量化后 | 缩减比 |

|------|--------------|---------|--------|```

| 源码体积（不含 .git/build） | ~2.1 GB | ~257 MB | **~88%** |

| llvm/ 目录 | ~1.1 GB | ~118 MB | ~89% |### Option 2: Two-phase build

| clang/ 目录 | ~435 MB | ~72 MB | ~83% |

| 目标后端数量 | 24+ | 3（AArch64 + ARM + RISCV） | ~88% |```bash

| 顶层项目数量 | 21 | 4（llvm, clang, easy-jit, cmake） | ~81% |# Phase 1: Build minimal LLVM + Clang

./easy-jit/build-lightweight-easyjit.sh --llvm-only

## 做了哪些改动

# Phase 2: Build EasyJIT standalone

### 1. 裁剪的顶层项目（21 → 4）

```

**完全移除：**

- `bolt`, `clang-tools-extra`, `compiler-rt`, `cross-project-tests`### Option 3: Use existing LLVM build

- `flang`, `libc`, `libclc`, `libcxx`, `libcxxabi`, `libunwind`

- `lld`, `lldb`, `llvm-libgcc`, `mlir`, `offload`, `openmp`If you already have a compatible LLVM 19.1 build:

- `polly`, `pstl`, `runtimes`, `third-party`, `utils`

```bash

**保留：**cd easy-jit

- `llvm/` — LLVM 核心库（IR, CodeGen, Passes, MCJIT, ExecutionEngine 等）./build_easyjit.sh --llvm-dir /path/to/llvm/lib/cmake/llvm

- `clang/` — Clang 编译器（EasyJitPass 作为 clang 插件加载必须）```

- `easy-jit/` — EasyJIT 本身

- `cmake/` — 共享 CMake 模块## What's Included



### 2. 裁剪的 LLVM 子目录| Directory     | Purpose                                    |

|---------------|--------------------------------------------|

- `llvm/test/` (~848 MB) — LLVM 测试套件| `llvm/`       | Core LLVM (IR, codegen, passes, MCJIT)     |

- `llvm/unittests/` (~13 MB) — 单元测试| `clang/`      | Clang compiler (for `-fplugin` support)     |

- `llvm/examples/` (~1.5 MB) — 示例代码| `easy-jit/`   | EasyJIT pass plugin + runtime library      |

- `llvm/docs/` (~22 MB) — 文档| `cmake/`      | Shared CMake modules                       |

- `llvm/benchmarks/` — 基准测试

- `llvm/bindings/` (~652 KB) — 语言绑定## What's Removed



### 3. 裁剪的 LLVM 工具The following LLVM monorepo components have been removed:



移除了 26 个不相关的 llvm/tools 子目录，包括：- **Projects**: bolt, clang-tools-extra, compiler-rt, cross-project-tests,

bugpoint, dsymutil, gold, llvm-exegesis, llvm-mca, llvm-reduce, llvm-xray,  flang, libc, libclc, libcxx, libcxxabi, libunwind, lld, lldb, llvm-libgcc,

llvm-gsymutil, llvm-dwarfutil, llvm-dwp, llvm-rc, llvm-pdbutil, llvm-profgen,  mlir, offload, openmp, polly, pstl

llvm-profdata, llvm-cov, llvm-c-test, llvm-diff, llvm-jitlink, llvm-lto,- **Runtimes**: runtimes, third-party

llvm-lto2, llvm-ml, llvm-mt, remarks-shlib, sancov, sanstats,- **LLVM internals**: test suites, unit tests, examples, docs, benchmarks,

verify-uselistorder, bugpoint-passes  bindings, most tools (bugpoint, dsymutil, exegesis, mca, etc.)

- **Target backends**: All backends except the native host target

### 4. 裁剪的目标后端（24 → 3）  (AArch64/ARM/RISCV tablegen files are kept for TargetParser)

- **Clang internals**: tests, docs, examples, bindings, www

**保留（构建所需）：**

- `AArch64` — 当前宿主架构## EasyJIT Dependencies

- `ARM` — TargetParser tablegen 依赖（不能移除）

- `RISCV` — TargetParser tablegen 依赖（不能移除）EasyJIT requires these LLVM components:



**移除：**- `core` – LLVM IR

X86, AMDGPU, NVPTX, PowerPC, SystemZ, Hexagon, Mips, WebAssembly,- `support` – Basic utilities

Sparc, Lanai, BPF, AVR, MSP430, XCore, VE, CSKY, LoongArch, M68k,- `passes` – PassBuilder, O3 pipeline

DirectX, SPIRV, Xtensa, ARC- `codegen` – Code generation

- `executionengine` – JIT execution

### 5. 裁剪的 Clang 子目录- `mcjit` – MCJIT engine

- `interpreter` – LLVM Interpreter

- `clang/test/` (~350 MB)- `native` – Native target backend

- `clang/unittests/` (~6.8 MB)- `objcarcopts` – ObjC ARC optimizations

- `clang/docs/` (~4.5 MB)- `linker` – Module linker (for devirtualization)

- `clang/examples/` (~92 KB)- `bitreader` / `bitwriter` – Bitcode I/O

- `clang/bindings/` (~368 KB)

- `clang/www/` (~1.8 MB)## Build Artifacts



### 6. CMake 修改After building:



- **`llvm/CMakeLists.txt`**: `LLVM_ALL_PROJECTS` 仅保留 `"clang"`, `LLVM_EXTRA_PROJECTS` 仅保留 `"easy-jit"`- `EasyJitPass.so` – Clang compiler plugin (loaded via `-fplugin=...`)

- **`clang/CMakeLists.txt`**: `add_subdirectory(examples)` 加了 `if(EXISTS ...)` 保护- `libEasyJitRuntime.so` – Runtime library (linked with user programs)



### 7. 构建配置要点## Limitations



必须启用以下 CMake 选项（EasyJIT 使用异常和 RTTI）：- Only builds for the host architecture

- `LLVM_ENABLE_RTTI=ON`- Requires Clang from the same LLVM version (19.1) for the pass plugin

- `LLVM_ENABLE_EH=ON`- The O3 optimization pipeline is preserved as-is (no pass trimming)

- ARM and RISCV target `.td` files are kept (required by TargetParser tablegen)

## 新增的关键文件

| 文件 | 用途 |
|------|------|
| `easy-jit/build-lightweight-easyjit.sh` | 一键构建脚本：Phase 1 编译最小 LLVM+Clang，Phase 2 编译 EasyJIT |
| `trim-repo.sh` | 仓库裁剪脚本，支持 `--dry-run` 预览 |
| `LIGHTWEIGHT-MANIFEST.md` | 详细的裁剪清单 |
| `README-lightweight.md` | 本文件 |

## 构建方法

### 方式一：一键构建（推荐）

```bash
./easy-jit/build-lightweight-easyjit.sh
```

选项：
- `--llvm-only` — 仅构建 LLVM+Clang
- `--easyjit-only` — 仅构建 EasyJIT（需先有 LLVM）
- `--clean` — 清除构建目录重建
- `-j N` — 并行数
- `--build-type Release|Debug|RelWithDebInfo` — 构建类型

### 方式二：分步构建

```bash
# Phase 1: 构建最小 LLVM
cmake -S llvm -B build-lightweight-llvm -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLLVM_TARGETS_TO_BUILD="AArch64" \
  -DLLVM_ENABLE_PROJECTS="clang" \
  -DLLVM_ENABLE_RTTI=ON \
  -DLLVM_ENABLE_EH=ON \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_DOCS=OFF \
  -DLLVM_ENABLE_BINDINGS=OFF
ninja -C build-lightweight-llvm

# Phase 2: 构建 EasyJIT
cmake -S easy-jit -B build-lightweight-easyjit -G Ninja \
  -DLLVM_DIR=$(pwd)/build-lightweight-llvm/lib/cmake/llvm \
  -DCMAKE_C_COMPILER=$(pwd)/build-lightweight-llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=$(pwd)/build-lightweight-llvm/bin/clang++ \
  -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build-lightweight-easyjit easy-jit-core
```

### 使用 EasyJIT

```bash
clang++ -std=c++17 -O3 \
  -I easy-jit/include \
  -Xclang -load -Xclang build-lightweight-easyjit/bin/EasyJitPass.so \
  -Xclang -fpass-plugin=build-lightweight-easyjit/bin/EasyJitPass.so \
  your_code.cpp \
  -Lbuild-lightweight-easyjit/bin -lEasyJitRuntime -lpthread \
  -o your_binary
```

## EasyJIT 依赖分析

### 必需的 LLVM 组件

EasyJitRuntime 通过 `llvm_config()` 声明的依赖：
```
core codegen interpreter support mcjit native executionengine passes objcarcopts
```

展开后的完整传递依赖（~50 个库）：
```
LLVMCore, LLVMCodeGen, LLVMInterpreter, LLVMSupport, LLVMMCJIT,
LLVMExecutionEngine, LLVMPasses, LLVMObjCARCOpts,
LLVMAnalysis, LLVMAsmParser, LLVMAsmPrinter, LLVMBinaryFormat,
LLVMBitReader, LLVMBitWriter, LLVMCodeGenTypes, LLVMCoroutines,
LLVMDemangle, LLVMGlobalISel, LLVMInstCombine, LLVMInstrumentation,
LLVMipo, LLVMIRPrinter, LLVMIRReader, LLVMLinker, LLVMMC,
LLVMMCDisassembler, LLVMMCParser, LLVMObject, LLVMOrcShared,
LLVMOrcTargetProcess, LLVMProfileData, LLVMRemarks,
LLVMRuntimeDyld, LLVMScalarOpts, LLVMSelectionDAG, LLVMSymbolize,
LLVMTarget, LLVMTargetParser, LLVMTextAPI, LLVMTransformUtils,
LLVMVectorize, LLVMAArch64CodeGen, LLVMAArch64Desc, ...
```

### 关键耦合点

1. **PassBuilder + O3 Pipeline** — `Function.cpp` 中 `PB.buildPerModuleDefaultPipeline(OptLevel)` 拉入完整 O3 pass 管线
2. **ExecutionEngine / MCJIT** — `EngineBuilder` + `getFunctionAddress` 依赖 MCJIT 和全部 native codegen
3. **LinkAllPasses / LinkAllIR** — `InitNativeTarget.cpp` 中的 `#include <llvm/LinkAllPasses.h>` 拉入所有 pass
4. **Linker** — `DevirtualizeConstant.cpp` 使用 `llvm::Linker::linkModules`

### 可裁剪但暂未移除的依赖

| 组件 | 状态 | 原因 |
|------|------|------|
| `LLVMInterpreter` | 可能可移除 | 如果纯用 MCJIT 可不需要 |
| `LLVMObjCARCOpts` | 可能可移除 | ObjC ARC 优化，可能非必需 |
| `LLVMSymbolize` | 可能可移除 | 符号化，间接依赖 |
| `LinkAllPasses.h` | 可优化 | 可改为按需链接，减少二进制体积 |

## 验证结果

以下测试均在完全轻量化的构建链（lightweight LLVM + lightweight EasyJIT）下通过：

| 测试 | 结果 |
|------|------|
| `int_a` | ✅ PASS |
| `float_a` | ✅ PASS |
| `long_a` | ✅ PASS |
| `ptr_a` | ✅ PASS |
| `small_struct` | ✅ PASS |
| `struct_arg` | ✅ PASS |
| `struct_return` | ✅ PASS |
| `cache` | ✅ PASS |
| `opt_level` | ✅ PASS |
| `snapshot_struct` | ✅ PASS |
| `snapshot_array` | ✅ PASS |
| `example1-jit-struct-snapshot` (wireless) | ✅ PASS |

已知预存的失败（与轻量化无关，原始 build 也失败）：
- `devirtualization` — 抛出 `basic_string: construction from null` 异常
- `unroll` — 同上

## 当前方案限制

1. **仍需完整 LLVM 构建** — EasyJIT 依赖 LLVM 的静态库（~50 个），LLVM 构建步骤不可省略
2. **ARM/RISCV 后端无法移除** — `TargetParser` tablegen 硬依赖这两个目录的 `.td` 文件
3. **Clang 仍然完整保留** — 作为 EasyJitPass 的宿主编译器，无法进一步裁剪
4. **`LinkAllPasses.h`** — 静态链接所有 pass，增加二进制体积
5. **仅限 AArch64** — 当前裁剪针对 aarch64，换架构需重新运行 `trim-repo.sh`

## 后续进一步轻量化方向

1. **移除 `LinkAllPasses.h` / `LinkAllIR.h`** — 改为按需注册 pass，可显著减小 `libEasyJitRuntime.so` 体积
2. **探索移除 Interpreter** — 确认 EasyJIT 是否真正使用了 LLVM 解释器
3. **探索移除 ObjCARCOpts** — 非 Apple 平台可能不需要
4. **MCJIT → OrcJIT 迁移** — OrcJIT 更现代，可能带来更好的模块化
5. **预编译 LLVM 为共享库** — 使用 `LLVM_BUILD_LLVM_DYLIB=ON`，减少 EasyJIT 链接时间
6. **Clang 进一步精简** — 移除 StaticAnalyzer, 更多 tools 等
7. **支持多架构** — `trim-repo.sh` 已支持按 `uname -m` 自动选择目标架构
