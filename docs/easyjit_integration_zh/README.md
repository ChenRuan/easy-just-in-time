# EasyJIT 业务接入中文说明

这个目录用于沉淀业务侧接入 EasyJIT 时最常见的两类问题：

- [01_easyjit_cpp_usage.md](01_easyjit_cpp_usage.md)：EasyJIT C++ 接口怎么用，按仓库现有用例讲 `easy::jit`、参数绑定、snapshot、cache、dump IR。
- [02_cmake_use_cpp_and_link_easyjit.md](02_cmake_use_cpp_and_link_easyjit.md)：CMake 项目怎么启用 C++、怎么让 C 代码整体或局部按 C++ 编译、怎么 include EasyJIT 并链接静态库。

建议先看第一篇，确认业务函数应该怎么写 `easy::jit`；再看第二篇，把产品项目的 CMake 接上。
