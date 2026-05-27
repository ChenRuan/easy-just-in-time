# EasyJIT 业务接入中文说明

这个目录用于沉淀业务侧接入 EasyJIT 时最常见的几类问题：

- [01_easyjit_cpp_usage.md](01_easyjit_cpp_usage.md)：EasyJIT C++ 接口怎么用，按仓库现有用例讲 `easy::jit`、参数绑定、snapshot、cache、dump IR、轻量后端。
- [02_cmake_use_cpp_and_link_easyjit.md](02_cmake_use_cpp_and_link_easyjit.md)：CMake 项目怎么启用 C++、怎么让 C 代码整体或局部按 C++ 编译、怎么 include EasyJIT 并链接静态库。
- [03_light_backend_design.md](03_light_backend_design.md)：轻量 AArch64 后端完整设计说明，覆盖架构、IR 子集、大小端模型、可复刻测试、优缺点和技术评审问题清单。

配套交付物（仓库根目录下）：

- **`examples/easyjit_cpp_minimal/`** — 最小可独立拷贝的 CMake 工程模板。3 个 case：`int` 标量、struct snapshot、4 次小循环。`cmake -S . -B build -DEASYJIT_ROOT=... -DEASYJIT_LIB=... -DEASYJIT_PASS=...` 即可构建，**不依赖**主仓库的构建系统。第一次接入时强烈推荐先把它跑通。
- **`tools/easyjit_light_selfcheck.cpp`** — 板端自检程序，6 个 case 覆盖 `int / fp / snapshot / pointer load-store / 循环展开 / cache`。直接用本仓库的 `clang -Xclang -fpass-plugin=EasyJitPass.so` 编译，配合 `EASYJIT_LIGHT=force` 强制走轻量 AArch64 后端，确认目标板子上每条路径都打通。
- **`tools/build_easyjit_light_selfcheck.sh`** — 一键构建 selfcheck 的辅助脚本，不用自己拼 `-Xclang -fpass-plugin=...`、`-I include/`、静态库路径。支持 `--clangxx / --target / --sysroot / --stdlib / --extra-cxxflags / --extra-ldflags`，native / cross 都行。它**只**用来编 selfcheck，不参与业务工程构建。
- **`runtime/LightBackend_LIMITATIONS.md`** — 轻量后端当前覆盖与已知不支持点，配合 `EASYJIT_LIGHT_VERBOSE=1` 一起看；末尾 “Round 12 — Code Dump Diagnostics” 一节介绍 `EASYJIT_LIGHT_DUMP_CODE_DIR / EASYJIT_LIGHT_DUMP_META` 两个新的 dump 机器码诊断开关。

建议的接入顺序：

1. 先把 `examples/easyjit_cpp_minimal/` 跑通，确认编译器 / pass plugin / 运行时库三者匹配。
2. 阅读 [02_cmake_use_cpp_and_link_easyjit.md](02_cmake_use_cpp_and_link_easyjit.md)，把这套 CMake 片段搬到你的产品工程里。
3. 阅读 [01_easyjit_cpp_usage.md](01_easyjit_cpp_usage.md)，把一个**小的** kernel 套上 `easy::jit(...)` 跑通。
4. 在目标板子上跑一次 `tools/easyjit_light_selfcheck.cpp`，确认 6 个 case 全 PASS，再把 EasyJIT 接到真实业务函数。
