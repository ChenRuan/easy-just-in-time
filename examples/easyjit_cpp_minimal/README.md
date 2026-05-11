# EasyJIT C++ 最小集成示例

本目录是一个**独立可拷贝**的 CMake 工程模板，演示如何在你自己的项目中通过 C++ API 接入 EasyJIT，**不依赖**主仓库的 `add_subdirectory`、不要求把 EasyJIT 一起构建进你的工程。

适用对象：

- 想在板端业务工程里调用 `easy::jit(...)`、`easy::Cache<>` 的同学；
- 已经在板端有一份预编译的 EasyJIT 运行时（`libEasyJitRuntime.so` 或带 LLVM 的胖静态库 `libEasyJitRuntimeWithNeededLLVM.a`）和 `EasyJitPass.so`，希望把它们 plug 进既有 CMake 工程。

## 1. 你需要先准备好的三样东西

| Cache 变量 | 含义 | 典型路径 |
| --- | --- | --- |
| `EASYJIT_ROOT` | EasyJIT 源码根目录（用来取 `include/easy/*.h`） | `.../easy-jit-main` |
| `EASYJIT_LIB`  | 要链接的 EasyJIT 运行时库 | 板端：`.../libEasyJitRuntimeWithNeededLLVM.a`；本机调试：`.../build-llvm15-global/bin/libEasyJitRuntime.so` |
| `EASYJIT_PASS` | `EasyJitPass.so`（clang 编译期由 `-fpass-plugin` 加载） | `.../build-llvm15-global/bin/EasyJitPass.so` 或 `.../prebuilt/llvm15.0.4/EasyJitPass.so` |

并且：

- `CMAKE_CXX_COMPILER` **必须**指向与 `EasyJitPass.so` 同一份 LLVM 编出来的 `clang++`（默认就是 LLVM 15.0.4 自己的 clang）。用系统 clang-18 之类去加载 LLVM-15 的 pass plugin 一定会报 ABI 不匹配。

## 2. 配置 & 构建

```bash
cmake -S . -B build \
  -DCMAKE_CXX_COMPILER=/abs/path/to/llvm15/bin/clang++ \
  -DEASYJIT_ROOT=/abs/path/to/easy-jit-main \
  -DEASYJIT_LIB=/abs/path/to/libEasyJitRuntimeWithNeededLLVM.a \
  -DEASYJIT_PASS=/abs/path/to/EasyJitPass.so

cmake --build build -j
```

如果用动态库（`libEasyJitRuntime.so`），运行时记得能找到它：

```bash
LD_LIBRARY_PATH=/abs/path/to/easyjit/build/bin ./build/easyjit_cpp_minimal
```

## 3. 运行

```bash
./build/easyjit_cpp_minimal
```

预期输出：

```
EasyJIT C++ minimal demo
  add_int(10, 5)    = 15 (expect 15)
  apply_cfg(7)      = 15 (expect 15)
  dot_like(A, B)    = 220 (expect 220)
  cache hit=1, f1(2)=7 f2(2)=7 (both expect 7)
OK
```

强制走轻量 AArch64 后端（板端验证用）：

```bash
EASYJIT_LIGHT=force EASYJIT_LIGHT_VERBOSE=1 ./build/easyjit_cpp_minimal
```

跑通这四行后，**先**把 `tools/easyjit_light_selfcheck.cpp` 拷过来做更完整的自检，**再**把 EasyJIT 接到真实业务函数上。

## 4. CMakeLists.txt 在做什么

```cmake
target_compile_options(... PRIVATE
    -Xclang -fpass-plugin=${EASYJIT_PASS}
    -Xclang -disable-O0-optnone)
```

这是接入 EasyJIT 的关键一行。它让 clang 在编译每个 `.cpp` 时都加载 `EasyJitPass.so`，pass 会把所有 `EASY_JIT_EXPOSE` 标注函数的 bitcode 嵌进 .o 文件。**漏掉这一行**，运行期 `easy::jit(...)` 找不到 bitcode 直接 abort。

```cmake
target_link_libraries(... PRIVATE ${EASYJIT_LIB} pthread dl)
```

当 `EASYJIT_LIB` 是一个胖静态库时，`pthread`、`dl` **必须**写在它后面，由静态库引出的符号才能被解析。`LINKER_LANGUAGE CXX` 保证胖 `.a` 里的 C++ 全局对象会被 libstdc++ 正确链接。

## 5. 常见坑

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `cannot load plugin EasyJitPass.so: undefined symbol ...` | 用了与 plugin 不匹配版本的 clang | 强制 `-DCMAKE_CXX_COMPILER=.../llvm15/bin/clang++` |
| 运行时 `Could not find bitcode for function ...` | 该 .cpp 编译时没启用 pass plugin | 检查 `compile_commands.json`，确保每个 TU 都带 `-Xclang -fpass-plugin=...` |
| `undefined reference to pthread_create / dlopen` | 链接顺序错 | `${EASYJIT_LIB}` 必须在 `pthread dl` 之前 |
| `LightBackendCompileError: fmuladd non-f32` 等 | 使用了轻量后端尚未覆盖的 IR shape | 移除 `EASYJIT_LIGHT=force`，让运行时回退到完整 JIT；或参考 `runtime/LightBackend_LIMITATIONS.md` 改写算子 |

## 6. 你可能还想看

- `tools/easyjit_light_selfcheck.cpp` — 6 个 case 的板端自检程序。
- `docs/easyjit_integration_zh/01_easyjit_cpp_usage.md` — C++ API 使用细节。
- `docs/easyjit_integration_zh/02_cmake_use_cpp_and_link_easyjit.md` — 更完整的 CMake 接入说明。
- `runtime/LightBackend_LIMITATIONS.md` — 轻量后端当前覆盖与限制。
