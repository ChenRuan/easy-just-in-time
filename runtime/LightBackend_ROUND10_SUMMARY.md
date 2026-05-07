# EasyJIT Light Backend — Round 10 收尾汇报

> 提交基线: `e0acd1b Add minimal GPR spill reload to light backend` (`llvm15_trim`)
> 验收日期: 2026-05-08

## 1. 背景

EasyJIT 原 JIT 路径依赖 LLVM 的完整后端链路：`MinimalOrcJIT` →
`LLVMOrcJIT` → `LLVMRuntimeDyld` / `LLVMJITLink` → `LLVMCodeGen` /
`LLVMSelectionDAG` / `LLVMAsmPrinter` / `LLVMAArch64*` / `MC`。这套链路
对嵌入式 / 严格受控部署来说体积大、初始化慢、难以裁剪，而且引入了大量
不需要的目标后端代码。

Round 1–10 给 EasyJIT 加了一条独立的 light backend 路径：

```
specialize 后 IR  →  受限 IR shape 识别  →  手写 AArch64 emitter
                  →  mmap RW + memcpy + mprotect RX  →  直接 call
```

该路径完全不引用任何 LLVM 后端模块；当 IR 走出 light backend 支持的
形状时，full mode 会回退到 ORC，light-only 模式则抛
`LightBackendCompileError`，让上层显式处理。

## 2. 已支持能力（Round 10 完成后）

- Triple gate: `aarch64-*` / `aarch64_be-*` / `arm64-*`，其余拒绝。
- 指令字节流恒为小端 emit（ARM ARM B2.6.2），数据访问遵循 target data
  endian；light backend JIT 出的代码 target==host，BE 板子 BE 数据语义
  自洽。
- 整型/指针：`i32` / `i64` / 指针，常量 + 负数走 MOVZ/MOVK halfword chain。
- 浮点：`float` / `double` 加减乘除、`fneg`、ordered `fcmp`、FP `select`、
  `fcmp + br` fused、`fmuladd`、`fpext` / `fptrunc`、`fp ↔ int` 转换、
  `bitcast` (FMOV W↔S, X↔D)。
- 调用约定：x0..x7 / s0..s7 / d0..d7 in-register 参数；第 9 个起的标量
  `i32` / `i64` / pointer / float / double 通过 AAPCS64 NSAA 共享 overflow
  area。GPR overflow 走 lazy reload，不长期占用 scratch。
- 控制流：单/多 BB、PHI 在前驱拷贝、`br` / `br i1`、`select`。
- 内存：常量 alloca、constant-offset GEP、动态 GEP（最多两 dynamic terms）、
  pointer-tracking、ConstantData/PrivateLinkage 全局 reify。
- 寄存器分配（Round 8 → Round 10）：
  - x{argCount}..x15 caller-saved scratch；
  - x19..x28 callee-saved scratch（prologue save / epilogue restore）；
  - 同 BB 内 SSA 死亡值回收 freelist；
  - binop dest-reg reuse；
  - **Round 10**: minimal GPR spill/reload，integer/pointer-width 局部
    SSA 在 scratch OOM 时自动 spill 到帧上 8 字节槽（最多 32 槽 = 256 B），
    valueInReg 自动 reload。FP/PHI/Argument/cross-BB/ptrLoc/向量不参与
    spill。
- 板上 smoke 工具：`tests/c_api/be_backend_probe.c` 共 9 个 case
  (`mem` / `stack` / `fp` / `snapshot` / `pressure` / `combo` / `stress` /
  `branch` / `gauntlet`)，CLI 支持 `--case` / `--iters` / `--verbose` /
  `--dump-prefix`。

## 3. 验证结果（Round 10 验收当日，host=aarch64 LE）

### 3.1 持续回归（5 个 light backend 专属 target）

```
cmake --build build-llvm15-global --target check-light-endian \
        check-light-fp check-light-stack-args check-light-dynamic-gep \
        check-light-gpr-pressure -j$(nproc)
```

| Target | 结果 |
|---|---|
| `check-light-endian` | PASS — light endian/triple parity |
| `check-light-fp` | PASS — calc + clamp + maxf + stale_repro + ret_const + dcalc + dclamp + dmax + dret_15 + dtoi + scalar fp conversions |
| `check-light-stack-args` | PASS — i64 + i32 + ptr + float + double + mixed overflow + frame_plus_stack |
| `check-light-dynamic-gep` | PASS — 6 个 IR shape 全过 (含 ADD-ext coverage 守护) |
| `check-light-gpr-pressure` | PASS — 17 GPR-producing SSA 单 BB 函数，依赖 x19..x28 |

### 3.2 board-friendly probe

```
EASYJIT_LIGHT=force ./tests/c_api/output/be_backend_probe_o0 \
    --case all --iters 50 --verbose
```

```
[case] mem        OK (50 iters)
[case] stack      OK (50 iters)
[case] fp         OK (50 iters)
[case] snapshot   OK (50 iters)
[case] pressure   OK (50 iters)
[case] combo      OK (50 iters)
[case] stress     OK (50 iters)
[case] branch     OK (50 iters)
BE_PROBE_RESULT PASS
```

```
EASYJIT_LIGHT=force ./tests/c_api/output/be_backend_probe_o0 \
    --case gauntlet --iters 20 --verbose
```

```
[case] gauntlet  OK (20 iters)
BE_PROBE_RESULT PASS
```

### 3.3 C API regression

```
EASYJIT_LIGHT=force tests/c_api/run_c_api_tests.sh \
    /home/ruanchen/workspace/llvm-project-15.0.4/build \
    $(pwd)/build-llvm15-global
```

退出码 `0`；`add_int` / `array_snapshot` / `cache_example` /
`embedded_diag_easyjit` / `global_partial_snapshot` /
`global_snapshot` / `light_backend_perf` (mem / stack / fp / snapshot /
pressure 五段全 PASS) / `mixed_bindings` / `partial_struct_binding` /
`pointer_field_snapshot` / `struct_snapshot` / `wireless_beamform`
(8/8 subcarriers OK) 等全部输出 `OK` / `ALL PASSED` /
`PERF_RESULT PASS`，无 `FAIL` / `ERROR` / `reject` / `abort`。

### 3.4 BE 真机 / QEMU smoke

本轮**未在 BE 板或 QEMU 上重新跑**（前几轮已确认 `aarch64_be` 触发
路径 + opcode bit pattern 不变）。BE 板验收建议命令见第 5 节。

## 4. 体积 / 依赖审计

### 4.1 light-only 静态构建

```
cmake -G Ninja -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm \
      -DEASYJIT_LIGHT_BACKEND_ONLY=ON \
      -DEASY_JIT_RUNTIME_TYPE=STATIC \
      -DCMAKE_BUILD_TYPE=Release ../easy-jit-main
ninja EasyJitRuntime light_endian_parity_test
```

| 产物 | 大小 |
|---|---|
| `libEasyJitRuntime.a` (13 个 .o，含 light_aarch64.cpp) | **42 266 170 B ≈ 40.31 MiB** |
| `light_endian_parity_test` raw | 9 044 424 B ≈ 8.63 MiB |
| `light_endian_parity_test` stripped | **4 135 352 B ≈ 3.94 MiB** |
| 其他 light_*_test stripped | 4 135 184 B（一致） |

> 备注：`.a` 体积主要来自尚未做死代码裁剪的 LLVM IR 头模板展开；
> 最终 stripped 可执行只 ~4 MiB，与 light-only 目标完全一致。

### 4.2 LLVM 组件闭包

light-only 模式 `runtime/CMakeLists.txt` 只请求：

```
analysis  bitreader  bitwriter  core  instcombine  ipo  linker
scalaropts  support  transformutils
```

`llvm-config --libs` 解析后的完整闭包（30 个静态库，77.7 MB 原始 .a）：

```
LLVMAggressiveInstCombine  LLVMAnalysis  LLVMAsmParser  LLVMBinaryFormat
LLVMBitReader  LLVMBitstreamReader  LLVMBitWriter  LLVMCore
LLVMDebugInfoCodeView  LLVMDebugInfoDWARF  LLVMDebugInfoMSF
LLVMDebugInfoPDB  LLVMDemangle  LLVMFrontendOpenMP  LLVMInstCombine
LLVMInstrumentation  LLVMipo  LLVMIRReader  LLVMLinker  LLVMMC
LLVMMCParser  LLVMObject  LLVMProfileData  LLVMRemarks  LLVMScalarOpts
LLVMSupport  LLVMSymbolize  LLVMTextAPI  LLVMTransformUtils
LLVMVectorize
```

### 4.3 Forbidden 后端依赖结论 ✅

`grep -iE "CodeGen|Orc|JITLink|RuntimeDyld|SelectionDAG|AsmPrinter|AArch64|^-lLLVMTarget$|GlobalISel|MCJIT|ExecutionEngine"` 在闭包列表上结果为
**空**。

`nm -C build-llvm15-light-only/bin/libEasyJitRuntime.a` 与
`nm -C build-llvm15-light-only/bin/light_endian_parity_test` 中匹配
`llvm::orc::|llvm::JITLink|llvm::AArch64|llvm::SelectionDAG|llvm::AsmPrinter|RuntimeDyld|llvm::CodeGen` 的符号数 = **0**。

即 light-only 模式确认无：

- ❌ `LLVMCodeGen` / `LLVMSelectionDAG` / `LLVMAsmPrinter`
- ❌ `LLVMOrcJIT` / `LLVMJITLink` / `LLVMRuntimeDyld` / `LLVMMCJIT` /
       `LLVMExecutionEngine`
- ❌ `LLVMAArch64*` / `LLVMTarget` / `LLVMGlobalISel`
- ❌ 任何 `llvm-tablegen` 生成的 target descriptor

这与前几轮文档列出的允许清单（Analysis / BinaryFormat / BitReader /
BitstreamReader / BitWriter / Core / DebugInfo* / Demangle / ipo /
Linker / MC / Object / ProfileData / Remarks / ScalarOpts / Support /
TextAPI / TransformUtils / + IRReader / Vectorize / AsmParser /
AggressiveInstCombine / FrontendOpenMP / Instrumentation / Symbolize）
一致；新增的几项 (`AsmParser` / `Vectorize` / `FrontendOpenMP` /
`Symbolize`) 是 LLVM-15 `instcombine` / `linker` 的传递闭包，与
codegen/jit 无关。

## 5. 板子可用验证命令

把 `libEasyJitRuntime.a` + `EasyJitPass.so` (host 编译) + `tests/c_api/`
推到 BE 板，按现有 build flow 编译 `be_backend_probe.c` 后：

```
# 必跑：
EASYJIT_LIGHT=force ./be_backend_probe --case all --iters 50 --verbose
EASYJIT_LIGHT=force ./be_backend_probe --case gauntlet --iters 20 --verbose
# 期望输出末尾：
#   BE_PROBE_RESULT PASS
```

```
# 可选 verbose 调试：
EASYJIT_LIGHT_VERBOSE=1 EASYJIT_LIGHT_DIAG=1 \
  EASYJIT_LIGHT=force ./be_backend_probe --case stress --iters 10 --verbose
```

```
# 可选 dump:
EASYJIT_LIGHT=force ./be_backend_probe --case all --iters 1 \
  --dump-prefix /tmp/be_probe_dump
```

如目标平台是 `aarch64_be`，所有 case 应该全过；如某个 case reject，
注意把 reject reason（如 `scratch OOM (binop)` / `frame > 4095B` /
`fp scratch OOM (...)`）回传，定位是否是当前 light backend 已知未支持
形状。

## 6. 剩余限制

- **向量 IR**：完全不支持。任何 `<N x T>` 或 v 寄存器多 lane 操作
  reject。
- **FP / V 寄存器 spill**：未实现；FP 高压函数仍可能
  `fp scratch OOM (...)`。
- **跨 BB spill**：未实现。spill 只在 same-block 整数 SSA 上工作；
  cross-BB 活值仍占满 scratch。
- **PHI / Argument spill**：不参与 spill；PHI 节点仍按预分配方式钉死
  寄存器，多 PHI 函数仍可能 OOM。
- **C++ 高级 ABI**：不支持 exception / RTTI / aggregate-by-value /
  HFA / varargs / 隐式 sret。
- **unordered FP predicates / `FCMP_ONE`**：当前只覆盖 ordered 分支语义。
- **frame ≤ 4 KiB**：超过 LDR/STR uimm12-scaled 编码上限的栈帧
  clean reject (`frame > 4095B`)。
- **complex IR shapes**（多重指针穿透、复杂控制流、间接调用、调用其他
  非已 specialize 的函数）：light backend 默认 reject，full mode 走
  ORC fallback；light-only 模式抛 `LightBackendCompileError`。
- **BE 真机持续验证**：build-side 已经 LE/BE bit-stream parity；建议
  在 BE 板/QEMU 上至少周期性跑一次 `be_backend_probe`，特别是新增
  feature 之后。

## 7. 明天起建议下一步

按性价比从高到低（每条都可以 1–2 天落地）：

1. **CI 接 BE QEMU smoke**：在 host CI 通过 `qemu-aarch64_be -L`
   套一遍 `be_backend_probe --case all`；可以在不真做 BE 板对接的
   情况下提供持续验证。
2. **scratch OOM 出错时打印更准确的 reject reason**（区分 binop /
   select / cast / spill-cap-exhausted），便于板侧直接定位是哪种
   形状超出能力。
3. **`be_backend_probe` 增加 `--repro` 序列化**：把当前 fallback 的
   reject reason + spec'd IR 抓到一个 .ll，便于日后做 reduce。
4. **小修：把 light-only build 接进 CI**，确认 forbidden symbol = 0
   作为持续守护。
5. *(可选，下个里程碑)* cross-BB 活值 spill：把当前的 same-block 算法
   推到“在 BB 边界 flush ⇒ 在新 BB 入口按需 reload”。这一步会让
   PHI 多、cross-BB 活值多的真实业务 kernel 也能进 light 路径。

> 注意：本轮（round 10）刻意**不**做完整 register allocator、不引入
> SelectionDAG/Orc/AsmPrinter 任何依赖、不动 vector 支持。这些都是
> 下一阶段评估项。
