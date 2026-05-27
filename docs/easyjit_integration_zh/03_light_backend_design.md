# EasyJIT 轻量 AArch64 后端设计说明

本文档面向技术评审，描述 `llvm15_trim` 分支中 EasyJIT 轻量 AArch64 后端的设计目标、实现边界、可复刻步骤、验证方法、收益与代价。它不是用户接入教程；业务接入请先看 `01_easyjit_cpp_usage.md` 和 `02_cmake_use_cpp_and_link_easyjit.md`。

## 1. 背景与目标

EasyJIT 的原始运行路径依赖 LLVM ORC JIT、RuntimeDyld/JITLink、TargetMachine、SelectionDAG/GlobalISel、AsmPrinter、MC 等完整 LLVM 后端组件。这个方案功能完整，但在嵌入式/板端交付场景中有几个问题：

- 包体积大：完整后端会拉入大量 LLVM CodeGen、MC、目标后端与 JIT 基础设施对象。
- 启动与编译链路长：IR 优化后还要走 LLVM 通用 codegen pipeline。
- 目标平台 C++ runtime/动态加载环境不稳定时，ORC/JITLink 依赖面更大，排障困难。
- 业务侧通常只需要对“参数绑定和 snapshot 后的标量 kernel”做 JIT，不需要完整 C/C++ 语言后端能力。

轻量后端的目标是：

- 在 EasyJIT runtime 内直接把优化后的 LLVM IR 子集翻译成 AArch64 机器码。
- 移除运行时对 LLVM CodeGen/ORC/JITLink/RuntimeDyld/SelectionDAG/AsmPrinter/AArch64 后端库的强依赖。
- 支持 `aarch64-*` 与 `aarch64_be-*` LP64 目标，尤其覆盖目标板上需要的 big-endian 验证路径。
- 对不支持的 IR 形态 clean reject，不能静默生成错误代码。
- 保持原有 EasyJIT 参数绑定、snapshot、cache、C++ API/C API 的上层语义。

非目标：

- 不实现通用 LLVM 后端。
- 不引入 Machine IR。
- 不支持任意 C/C++ IR。
- 不做全局寄存器分配、指令调度、向量化或通用 loop lowering。
- 不替代 LLVM 优化器；轻量后端只负责把已优化、已特化后的 IR 子集降到机器码。

## 2. 总体架构

### 2.1 原始路径

原始 EasyJIT 路径如下：

```text
业务函数 + EASY_JIT_EXPOSE
        |
        | clang + EasyJitPass.so
        v
对象文件中携带 bitcode blob + 注册函数
        |
        | runtime easy_register / BitcodeTracker
        v
easy::jit / easyjit_compile
        |
        | parseBitcodeFile
        v
LLVM Module
        |
        | EasyJIT 自定义优化 + LLVM legacy passes
        v
优化后的 LLVM IR
        |
        | ORC / LLVM CodeGen / MC / JITLink 或 RuntimeDyld
        v
可执行机器码
```

### 2.2 轻量后端路径

轻量后端插入在优化后、ORC fallback 前：

```text
easy::jit / easyjit_compile
        |
        | Function::Compile
        v
parse bitcode -> LLVM Module
        |
        | Optimize(Context, Module, EntryName)
        v
优化后的 LLVM IR
        |
        | EASYJIT_LIGHT policy
        v
TryLightCompile
        |
        | light::emit(Function, code_buffer, globals)
        v
手写 AArch64 指令编码
        |
        | mmap RW -> emit -> mprotect RX
        v
easy::Function wrapper
```

如果是普通 runtime 构建，轻量后端失败后可以 fallback 到 ORC。若使用 `EASYJIT_LIGHT_BACKEND_ONLY=ON` 构建，runtime 不包含 ORC/LLVM CodeGen fallback；不支持的 IR 只能失败，或者在签名完全一致时回退到原始函数指针。

### 2.3 核心源码位置

| 文件 | 作用 |
| --- | --- |
| `runtime/Function.cpp` | EasyJIT compile 主流程、优化 pipeline、轻量后端调用点。 |
| `runtime/LightBackend.h` | 轻量后端 runtime 入口、policy、结果类型。 |
| `runtime/LightBackend.cpp` | runtime glue：policy、host/triple check、private globals 物化、mmap/mprotect、Function holder。 |
| `light_codegen/light_aarch64.h` | 手写 emitter 对外接口、状态码、GlobalSymbol。 |
| `light_codegen/light_aarch64.cpp` | LLVM IR 子集解析与 AArch64 指令编码核心。 |
| `runtime/LightBackend_LIMITATIONS.md` | 当前支持/不支持 IR 形态、测试目标、历史验证记录。 |
| `runtime/CMakeLists.txt` | 轻量后端测试目标和 `check-light-*` 目标。 |

## 3. 与 LLVM 后端的关系

轻量后端不是“裁剪后的 LLVM CodeGen”，而是一个专用 direct IR-to-machine-code emitter。

它不经过：

- LLVM Machine IR
- SelectionDAG
- GlobalISel
- MachineInstr
- register allocator
- instruction scheduler
- MCInst
- AsmPrinter
- ORC/JITLink
- RuntimeDyld

它直接读取 LLVM IR 的 `Function`、`BasicBlock`、`Instruction`、`Type`、`DataLayout` 等对象，然后按受支持的 IR 模式手写 AArch64 32-bit instruction word，最后按 AArch64 规范写入 little-endian 指令字节。

因此“从 LLVM IR 到机器码”这段仍然属于 codegen，但它是 EasyJIT 自己实现的窄 codegen，不是 LLVM 通用 codegen。

## 4. 构建模式与运行策略

### 4.1 CMake 开关

轻量后端相关构建通常依赖：

```bash
-DEASYJIT_ENABLE_LIGHT_BACKEND=ON
```

如果要构建不含 ORC/LLVM CodeGen fallback 的 light-only runtime，需要使用项目提供的 cross runtime 脚本或对应 CMake 选项生成 light-only/static-bundle-needed 包。实际交付中推荐交付 fat static archive：

```text
libEasyJitRuntimeWithNeededLLVM.a
```

这类包只应包含 EasyJIT runtime 所需 LLVM Core/BitReader/Analysis/ScalarOpts 等库，不应包含 LLVMCodeGen、OrcJIT、RuntimeDyld、JITLink、SelectionDAG、AsmPrinter、AArch64CodeGen 等重型后端组件。

### 4.2 运行时策略

由环境变量 `EASYJIT_LIGHT` 控制：

| 值 | 行为 |
| --- | --- |
| unset / `off` / `0` | 不尝试轻量后端。 |
| `try` / `auto` / `1` | 尝试轻量后端；不支持时 fallback 到 ORC。 |
| `force` | 强制轻量后端；不支持时直接失败。 |

辅助诊断：

| 环境变量 | 作用 |
| --- | --- |
| `EASYJIT_LIGHT_VERBOSE=1` | 打印轻量后端是否命中、失败原因、fallback 原因。 |
| `EASYJIT_DUMP_IR=<path>` | dump 优化后的 IR。 |
| `EASYJIT_LIGHT_DUMP_CODE_DIR=<dir>` | dump 轻量后端生成的原始机器码 bytes。 |
| `EASYJIT_LIGHT_DUMP_META=1` | 配合机器码 dump 输出元信息。 |

## 5. 优化 pipeline 设计

轻量后端不负责复杂 IR 优化，所以 EasyJIT 在 `Function::Optimize` 中保留一条小而明确的优化 pipeline：

```text
ContextAnalysis
InlineParameters
DevirtualizeConstant
FunctionInlining
ConstStructPropagate
mem2reg
ConstStructPropagate
InstCombine
LoopSimplify / LCSSA / LoopRotate / LoopUnroll   (OptLevel >= 2)
CFGSimplification
Internalize
GlobalDCE
StripDeadPrototypes
Verifier
```

关键设计点：

- `InlineParameters` 将 EasyJIT 绑定参数和 snapshot 信息写回 IR。
- `FunctionInlining` 将 wrapper 与原函数内联到一个更容易特化的形态。
- `ConstStructPropagate` 是 EasyJIT 自定义轻量优化，专门处理 snapshot struct/array、GEP、load/store、branch folding。
- `mem2reg` 将普通 alloca 提升成 SSA，降低后端处理栈变量的压力。
- `LoopUnroll` 只在 `OptLevel >= 2` 启用，并且不启用 LoopVectorize/SLPVectorize。目标是把特化后 trip count 已知的小 loop 展开成标量直线 IR，而不是产生 vector IR。

这条 pipeline 的原则是：尽量用已有 LLVM Core/ScalarOpts 能力，把业务 kernel 规整成轻量后端能吃的标量 IR；不要引入完整 codegen 依赖。

## 6. IR 子集与语义边界

### 6.1 支持的目标 triple

接受：

- `aarch64-*`
- `aarch64_be-*`
- `arm64-*`

拒绝：

- `aarch64_32-*`
- `arm64_32-*`
- `x86_64-*`、`riscv64-*` 等非 AArch64 目标

状态码使用 `Status::NotAarch64`。旧名字 `NotAarch64LE` 仅作为兼容别名保留；当前实现不是 little-endian-only。

### 6.2 函数与参数

支持：

- 单个入口函数。
- 多 basic block，支持前向/后向分支。
- `i32`、`i64`、pointer 参数。
- `float`、`double` 参数。
- AAPCS64 前 8 个 GPR-class 参数走 `x0..x7`。
- 前 8 个 FP-class 参数走 `s0..s7` / `d0..d7`。
- 超过 8 个后的 scalar stack-passed 参数，支持 `i32/i64/pointer/float/double`，按 AAPCS64 NSAA 8-byte slot 处理。

不支持：

- by-value aggregate ABI。
- HFA/HVA。
- varargs。
- vector 参数。
- `i8/i16` stack-passed scalar 参数。

### 6.3 内存访问

支持：

- fixed-size stack frame。
- static alloca。
- constant-offset GEP。
- `i8/i16/i32/i64` load/store。
- `float/double` load/store。
- pointer argument base load/store。
- private/internal constant globals 的 runtime 物化。
- `llvm.memcpy` constant size <= 32 bytes。
- dynamic scaled GEP 最多两个动态项：

```text
base + constOff + idx0 * pow2Scale0 [+ idx1 * pow2Scale1]
```

限制：

- scale 必须是 power-of-two。
- `log2(scale)` 必须在 `0..12`。
- 超过两个动态 index 的 GEP 不支持。
- 非 power-of-two stride，例如 5 列 `int` 数组导致 20-byte stride，不支持。
- 复杂 index 表达式必须在 IR 层规整成 nested array/struct GEP，后端不识别任意 `mul/add` 组合。

### 6.4 整数运算

支持：

- `add`
- `sub`
- `mul`
- `and`
- `or`
- `xor`
- `shl`
- `lshr`
- `ashr`
- `sext`
- `zext`
- `trunc`
- `icmp` 融合到后续 conditional branch
- `select` 的整数结果形态
- `ret i32/i64/void`

整数常量：

- `i32/i64` 任意正负常量可通过 `MOVZ + MOVK*` materialize。
- 负数使用 two's-complement bit pattern，不使用 `MOVN`。
- 12-bit 非负立即数仍走 AArch64 ADD/SUB immediate 快路径。

限制：

- `icmp` 作为普通 GPR boolean 值的泛化形态不完整，主要支持 fused branch/select 场景。
- 窄整数负常量直接用于 `i1/i8/i16` 同宽 binop 不承诺支持，前端应先 widen。

### 6.5 浮点运算

支持：

- `float` / `double` load/store。
- `fadd`
- `fsub`
- `fmul`
- `fdiv`
- `fneg`
- `ret float/double`
- ordered `fcmp` 融合到 branch/select。
- `select` 返回 `float/double`。
- `llvm.fmuladd.f32`。
- scalar FP/int 转换：
  - `fptosi`
  - `fptoui`
  - `sitofp`
  - `uitofp`
  - `fpext`
  - `fptrunc`
  - FP/int bitcast

限制：

- unordered FP predicates 不支持。
- `FCMP_ONE` 不支持，因为 AArch64 无单条 condition 表达 “ordered and not equal”，需要 CCMP 或双分支。
- FP/V register spill 不支持，高 FP 压力会 clean reject。
- vector/NEON 不支持。
- FP-class intrinsics 覆盖不完整。

### 6.6 控制流

支持：

- unconditional branch。
- conditional branch。
- integer compare + branch。
- FP compare + branch。
- PHI edge copy。
- 多 basic block。
- loop 本身不是后端特性；若 LLVM 优化 pipeline 将常量 trip count loop 展开成直线 IR，则后端可处理展开后的 IR。

限制：

- runtime-variable loop 不是轻量后端重点能力。已有 branch/PHI 可覆盖部分简单形态，但不把“通用 loop lowering”作为承诺。
- cross-block spill 不支持。
- PHI spill 不支持。

## 7. AArch64 指令生成策略

### 7.1 Writer 与指令字节序

AArch64 指令固定为 32-bit word。ARM 规范规定 AArch64 instruction stream 以 little-endian 字节序编码。即使目标是 `aarch64_be`，指令字节仍按 little-endian 写入。

因此 `Writer::emit(uint32_t word)` 总是显式写：

```text
buf[0] = word & 0xff
buf[1] = (word >> 8) & 0xff
buf[2] = (word >> 16) & 0xff
buf[3] = (word >> 24) & 0xff
```

这保证在 LE host 和 BE host 上生成的 instruction byte stream 一致。

### 7.2 常量 materialization

整数常量：

- 小立即数优先用 AArch64 immediate encoding。
- 通用 `i32/i64` 常量使用 `MOVZ` 加若干 `MOVK` halfword。
- `MOVZ/MOVK` 的 halfword position 是逻辑位位置，不依赖数据字节序。

浮点常量：

- 可编码的简单 FP immediate 使用 FMOV immediate。
- 其他 bit pattern 先 materialize 到 W/X scratch，再用 `FMOV S,W` 或 `FMOV D,X` 转入 FP register。

### 7.3 寄存器策略

不是完整 register allocator，而是保守 scratch allocator：

- 参数寄存器按 AAPCS64 映射。
- caller-saved scratch 使用 `x8..x15` 等。
- 扩展 scratch 池使用 `x19..x28`，函数 prologue 保存，epilogue 恢复。
- `x16/x17` 用作临时地址/立即数 scratch。
- FP 使用 V register 的 `s` / `d` view。

局部复用：

- 同 basic block 内最后一次使用后释放临时 SSA value 的寄存器。
- binop 可在 source 死亡时复用 source register 作为 dest。
- dynamic GEP hidden term 在地址 materialize 完成后释放。

最小 GPR spill/reload：

- 仅针对 `i32/i64/pointer` 局部 instruction 临时值。
- spill slot 在当前函数 stack frame 内。
- 最多 32 slots。
- 不 spill PHI、Argument、cross-block live value、ptrLoc owner、dynamic-GEP term、reloadable stack arg、FP/vector。

### 7.4 分支与 PHI

PHI 通过 predecessor edge copy 实现。每条 incoming edge 在跳转到目标 block 前插入必要 move。该策略简单、可验证，但不做复杂并行 copy 消解；当前支持的 IR 形态中通过保守顺序和 scratch 规避常见冲突。

### 7.5 机器码内存管理

每个 light-compiled function 分配固定大小代码页：

```text
mmap RW
emit code
clear/icache sync if needed
mprotect RX
```

函数销毁时由 `LightCodeHolder` munmap。

当前策略刻意简单：

- 不做 code cache compaction。
- 不做可增长 code buffer。
- 超过固定上限返回 `TooLarge`。

## 8. Big-endian 支持模型

`aarch64_be` 支持分成两层：

### 8.1 指令流

指令流固定 little-endian，与目标 data endian 无关。因此 LE/BE 的机器码 bytes 应完全一致。`light_endian_parity_test` 已检查：

- `aarch64-*`
- `aarch64_be-*`
- `arm64-*`

对同一 IR 生成的 instruction byte stream 一致。

### 8.2 数据访问

JIT 生成代码在当前进程执行。也就是说：

```text
target data endian == host data endian
```

对 big-endian 板子：

- C/C++ runtime 写内存是 BE。
- JIT 代码执行 `LDR/STR` 也是 BE。
- `MaterializePrivateGlobals` 使用 host-endian `memcpy` 写入数据。

因此 producer/consumer 端字节序一致。

需要真实 BE 硬件验证的项目：

- `LDR/LDRH/LDRB/STR` 读写 struct/array 字段。
- `MOVZ/MOVK -> FMOV` 的 FP bit pattern。
- `llvm.memcpy` lowering。
- private globals 物化后由 JIT 代码读取。

项目中已经提供 QEMU user-mode BE smoke 和板端 selfcheck，但最终审查应以真实 `aarch64_be` 板端执行结果为准。

## 9. 可复刻构建与测试

以下命令以仓库根目录为例：

```bash
cd /path/to/easy-jit-main
```

### 9.1 构建 light-enabled runtime

示意：

```bash
cmake -S . -B build-llvm15-global \
  -DCMAKE_BUILD_TYPE=Release \
  -DEASYJIT_ENABLE_LIGHT_BACKEND=ON

cmake --build build-llvm15-global -j
```

### 9.2 运行轻量后端单元测试

```bash
cmake --build build-llvm15-global --target check-light-endian
cmake --build build-llvm15-global --target check-light-fp
cmake --build build-llvm15-global --target check-light-stack-args
cmake --build build-llvm15-global --target check-light-dynamic-gep
cmake --build build-llvm15-global --target check-light-gpr-pressure
```

这些目标覆盖：

| 目标 | 覆盖点 |
| --- | --- |
| `check-light-endian` | triple gate、LE/BE/arm64 instruction parity、MOVZ/MOVK negative constants。 |
| `check-light-fp` | f32/f64 arithmetic、fcmp、select、ret FP、FP/int conversions、bitcast。 |
| `check-light-stack-args` | AAPCS64 stack-passed scalar args。 |
| `check-light-dynamic-gep` | 两项 dynamic scaled GEP、struct/array index。 |
| `check-light-gpr-pressure` | scratch reuse、saved scratch、minimal GPR spill/reload。 |

### 9.3 运行 C API 回归

```bash
EASYJIT_LIGHT=force tests/c_api/run_c_api_tests.sh
```

典型覆盖：

- `add_int`
- `mixed_bindings`
- `struct_snapshot`
- `partial_struct_binding`
- `pointer_field_snapshot`
- `global_snapshot`
- `global_partial_snapshot`
- `array_snapshot`
- `wireless_beamform`

### 9.4 板端 selfcheck

构建：

```bash
tools/build_easyjit_light_selfcheck.sh \
  --easyjit-root /path/to/easy-jit-main \
  --easyjit-lib /path/to/libEasyJitRuntimeWithNeededLLVM.a \
  --easyjit-pass /path/to/EasyJitPass.so \
  --clangxx /path/to/clang++
```

运行：

```bash
EASYJIT_LIGHT=force EASYJIT_LIGHT_VERBOSE=1 \
  ./easyjit_light_selfcheck --iters 10 --verbose
```

### 9.5 强制 dump IR 与机器码

```bash
EASYJIT_LIGHT=force \
EASYJIT_DUMP_IR=/tmp/easyjit_opt.ll \
EASYJIT_LIGHT_DUMP_CODE_DIR=/tmp/easyjit_code \
EASYJIT_LIGHT_DUMP_META=1 \
./your_test
```

审查时建议保存：

- 优化后 IR。
- light backend 机器码 dump。
- `EASYJIT_LIGHT_VERBOSE=1` 日志。
- 测试输入输出。

## 10. 静态包依赖审查

light-only 静态包应检查对象依赖，确认没有重型后端：

```bash
nm -A libEasyJitRuntimeWithNeededLLVM.a | grep -E 'LLVMCodeGen|Orc|JITLink|RuntimeDyld|SelectionDAG|AsmPrinter|AArch64'
```

预期：

- 不应出现 LLVMCodeGen/ORC/JITLink/RuntimeDyld/SelectionDAG/AsmPrinter/AArch64 backend 强依赖。
- 仍会保留 LLVM Core、BitReader、Analysis、ScalarOpts、TransformUtils、Support、Object、BitWriter 等 IR 解析和优化相关组件。

`strip-debug` 可显著降低包体积，但不会删除 archive 中的 DWARF 相关 object 成员；它只移除 debug sections。若要彻底去掉某些 LLVM debug object，必须从链接依赖裁剪源头处理。

## 11. 失败模型与安全性

轻量后端失败必须显式返回：

| 失败 | 行为 |
| --- | --- |
| 非 AArch64 triple | `Status::NotAarch64` |
| IR 子集不支持 | `Status::Unsupported` + reason |
| 代码 buffer 不够 | `Status::TooLarge` |
| `EASYJIT_LIGHT=try` | fallback ORC，或 light-only 下尝试原函数指针 fallback。 |
| `EASYJIT_LIGHT=force` | 直接失败。 |

light-only 原函数指针 fallback 仅在签名完全一致时允许：

- 所有原始参数原样 forward。
- 参数顺序为 `0,1,2,...`。
- EasyJIT 没有绑定/删除参数。

否则 fallback 会产生 ABI 错误，因此必须拒绝。

## 12. 优点

### 12.1 包体积和依赖面

- 移除完整 LLVM 后端、ORC/JITLink/RuntimeDyld 等重型依赖。
- 静态包更适合嵌入式交付。
- 链接符号面更小，和目标 SDK 冲突概率降低。

### 12.2 编译路径短

- 优化后直接 emit AArch64 指令。
- 不创建 TargetMachine codegen pipeline。
- 不经历 Machine IR、指令选择、寄存器分配、汇编打印。

### 12.3 行为可控

- 支持子集明确。
- 不支持即 clean reject。
- 可以通过 dump IR / dump code 精确审查。
- AArch64/BE 指令编码模型简单透明。

### 12.4 更适合 EasyJIT 特化场景

EasyJIT 的核心价值是运行时绑定参数和 snapshot，使 IR 大量常量化。轻量后端利用这个特点，只覆盖特化后常见的标量直线/小 CFG 形态，而不是完整语言后端。

## 13. 缺点与风险

### 13.1 后端优化能力缺失

不经过 LLVM CodeGen 后，以下能力没有了：

- LLVM 后端指令选择优化。
- 后端 peephole。
- 指令调度。
- 成熟寄存器分配。
- rematerialization。
- spill cost model。
- load/store 合并。
- CPU micro-architecture tuning。
- vector/NEON lowering。

因此同一份 IR，light backend 生成的机器码性能可能不如 LLVM AArch64 后端。

### 13.2 IR 覆盖有限

light backend 对 IR shape 有强约束。业务函数如果包含复杂控制流、vector、aggregate ABI、复杂 GEP、unordered FP、runtime loop 等，可能会被拒绝。

### 13.3 维护成本

每新增一类 IR，都需要：

- 明确定义语义。
- 增加 IR shape 检查。
- 增加 AArch64 encoder。
- 增加 LE/BE parity 测试。
- 增加执行测试。
- 更新限制文档。

这比“直接交给 LLVM 后端”维护成本高。

### 13.4 平台 runtime 仍是关键依赖

即使去掉 LLVM CodeGen，runtime 仍需要：

- 解析 bitcode。
- 运行 LLVM IR 优化 pass。
- 使用部分 LLVM/Core/ADT 容器。
- 使用 C++ runtime。

如果目标 SDK 的 `std::string`、`std::vector`、`operator new/delete`、static initialization、exception 支持不可靠，仍可能影响 bitcode parse 和优化阶段。轻量后端只能减少依赖面，不能完全消除 LLVM/Core 与 C++ runtime 依赖。

## 14. 与“保留优化”的关系

“轻量后端”不等于删掉优化。当前方案保留的是 IR-level 优化：

- 参数绑定。
- snapshot 常量传播。
- function inlining。
- mem2reg。
- InstCombine。
- CFG simplification。
- scalar loop unroll。
- global DCE。

被移除的是后端 codegen 优化。也就是说：

```text
保留：LLVM IR 优化
移除：LLVM 通用机器码后端优化
替换：手写 AArch64 窄 codegen
```

如果后续性能不足，分析路径应是：

1. dump 优化后 IR，确认业务特化是否充分。
2. 确认 loop 是否展开、branch 是否折叠、snapshot load 是否常量化。
3. dump light backend 机器码，检查是否存在明显多余 load/store/mov/spill。
4. 对热点 IR shape 增加轻量后端 peephole 或 encoder 能力。
5. 对超出 light backend 能力的函数，选择 ORC fallback 或保留原函数。

## 15. 审查问题清单

评审时建议逐项确认：

- 是否确认目标只需要 AArch64/aarch64_be LP64。
- 是否确认业务 JIT kernel 经过特化后落在支持 IR 子集内。
- 是否确认无需 vector/NEON。
- 是否确认 runtime-variable loop 不作为 light backend 承诺能力。
- 是否确认 light-only 包不包含 LLVMCodeGen/ORC/JITLink/RuntimeDyld。
- 是否确认 `EASYJIT_LIGHT=force` 下所有目标用例通过。
- 是否保存优化后 IR 和机器码 dump 作为审查附件。
- 是否在真实 aarch64_be 板上验证 endian 数据读写。
- 是否确认目标 SDK 的 C++ runtime/new/delete/string/vector 基础能力可用。
- 是否明确 fallback 策略：try/fallback 还是 force/fail-fast。

## 16. 结论

轻量 AArch64 后端是一个面向 EasyJIT 特化后 IR 的窄 codegen。它通过牺牲通用性和部分后端优化能力，换取更小的运行时依赖、更清晰的可审查路径和更适合嵌入式交付的包形态。

它适合：

- 标量数值 kernel。
- 参数绑定后常量化明显的函数。
- snapshot/struct/array 访问较规则的函数。
- 对包体积和运行时依赖高度敏感的 AArch64/aarch64_be 场景。

它不适合：

- 依赖完整 C++ ABI lowering 的复杂函数。
- vector/NEON-heavy kernel。
- 动态复杂控制流和高寄存器压力函数。
- 需要 LLVM 后端做深度机器级优化的场景。

推荐交付策略是：默认通过 selfcheck 和业务 probe 圈定支持范围；`EASYJIT_LIGHT=force` 用于板端验证；正式集成时对关键路径保留 dump/verbose 能力，并为不支持函数保留可解释的 fallback 或失败路径。
