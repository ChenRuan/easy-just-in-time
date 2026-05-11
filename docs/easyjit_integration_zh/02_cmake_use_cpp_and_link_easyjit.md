# CMake：把 C 项目改为用 C++ 编译并链接 EasyJIT

本文说明两种接入方式：

1. 保守方式：原 `.c` 文件继续按 C 编译，只把使用 EasyJIT 的文件改成 `.cpp`，最终链接用 C++ linker。
2. 激进方式：把目标里的 `.c` 文件也当 C++ 编译。

如果项目很复杂，推荐先用保守方式。只有当业务侧明确希望“整个目标都按 C++ 编译”，并且愿意处理 C/C++ 兼容问题时，再使用激进方式。

## 1. 准备 EasyJIT 路径

假设 EasyJIT 源码和静态库路径如下：

```cmake
set(EASYJIT_ROOT "/path/to/easy-jit-main")
set(EASYJIT_LIB  "/path/to/libEasyJitRuntimeWithNeededLLVM.a")
```

常见静态库可能是：

```text
libEasyJitRuntime.a
libEasyJitRuntimeWithLLVM.a
libEasyJitRuntimeWithNeededLLVM.a
```

如果是部署到板子，通常优先使用已经为目标平台交叉编译好的静态包。不要把 x86_64 主机上的 runtime 静态库链接进 AArch64 目标程序。

拿到静态库包后，业务 CMake 里最关键的是这几行：

```cmake
set(EASYJIT_ROOT "/path/to/easy-jit-main")
set(EASYJIT_LIB  "/path/to/libEasyJitRuntimeWithNeededLLVM.a")

target_include_directories(their_app PRIVATE
  ${EASYJIT_ROOT}/include
)

target_link_libraries(their_app PRIVATE
  ${EASYJIT_LIB}
  pthread
  dl
)

set_target_properties(their_app PROPERTIES
  LINKER_LANGUAGE CXX
)
```

含义：

- `${EASYJIT_ROOT}/include` 让源码能 `#include <easy/jit.h>`。
- `${EASYJIT_LIB}` 把 EasyJIT runtime 和需要的 LLVM 静态对象链接进最终程序。
- `LINKER_LANGUAGE CXX` 确保最终链接走 C++ linker，避免缺 C++ runtime 符号。
- `pthread` / `dl` 是常见 Linux 链接依赖；如果目标平台没有 `dl` 或使用全静态特殊 libc，需要按产品工具链调整。

静态库不是运行时 `dlopen` 加载的文件，而是在最终链接时打进可执行文件或业务 `.so`。板子上通常只需要部署最终产物，不需要单独部署这个 `.a`。

## 2. 根 CMake 启用 C++

如果原来是：

```cmake
project(Product C)
```

可以改成：

```cmake
project(Product C CXX)
```

或者在后面加：

```cmake
enable_language(CXX)
```

这一步只是告诉 CMake：项目里允许有 C++ 文件。它不会自动把所有 `.c` 文件变成 C++。

## 3. 指定 gcc/g++

如果脚本里显式指定编译器，可以这样传：

```bash
cmake -S . -B build \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER=g++
```

交叉编译示例：

```bash
cmake -S . -B build \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++
```

如果使用 clang：

```bash
cmake -S . -B build \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++
```

重点：

- `.c` 文件默认仍由 `CMAKE_C_COMPILER` 编译。
- `.cpp` / `.cc` / `.cxx` 文件由 `CMAKE_CXX_COMPILER` 编译。
- 最终 target 如果包含 C++ 源文件，CMake 通常会自动用 C++ linker。
- 如果不确定，可以显式设置 `LINKER_LANGUAGE CXX`。

## 4. 保守方式：只让使用 EasyJIT 的文件用 C++

假设原目标是：

```cmake
add_executable(product_app
  src/main.c
  src/module_a.c
  src/module_b.c
)
```

现在新增一个使用 EasyJIT 的 C++ 文件：

```cmake
add_executable(product_app
  src/main.c
  src/module_a.c
  src/module_b.c
  src/jit_entry.cpp
)
```

然后加 include 和 link：

```cmake
target_include_directories(product_app PRIVATE
  ${EASYJIT_ROOT}/include
)

target_link_libraries(product_app PRIVATE
  ${EASYJIT_LIB}
)

set_target_properties(product_app PROPERTIES
  LINKER_LANGUAGE CXX
)
```

这是最推荐的方式：

- 原 C 文件不动。
- 只有 `jit_entry.cpp` 使用 `<easy/jit.h>`。
- 最终链接走 C++ linker，能正确带上 C++ runtime。

## 5. 如果目标是库而不是可执行文件

如果业务项目先编一个静态库：

```cmake
add_library(product_core STATIC
  src/module_a.c
  src/module_b.c
  src/jit_entry.cpp
)

target_include_directories(product_core PRIVATE
  ${EASYJIT_ROOT}/include
)
```

最终可执行文件链接：

```cmake
add_executable(product_app src/main.c)

target_link_libraries(product_app PRIVATE
  product_core
  ${EASYJIT_LIB}
)

set_target_properties(product_app PROPERTIES
  LINKER_LANGUAGE CXX
)
```

注意：静态库本身只是打包 `.o`，真正解析符号发生在最终链接可执行文件或 `.so` 时。因此 EasyJIT runtime 静态库一般放在最终产物的 link 阶段更稳。

## 6. 链接 pthread/dl/stdc++

如果最终链接器是 `g++` 或 `clang++`，C++ 标准库通常会自动带上。某些平台仍可能需要显式加：

```cmake
target_link_libraries(product_app PRIVATE
  ${EASYJIT_LIB}
  pthread
  dl
)
```

如果使用 libstdc++，并且最终链接仍由 `gcc` 驱动，可能需要：

```cmake
target_link_libraries(product_app PRIVATE
  stdc++
)
```

但更推荐让最终链接使用 C++ linker：

```cmake
set_target_properties(product_app PROPERTIES LINKER_LANGUAGE CXX)
```

如果使用 libc++，可能需要根据工具链补：

```cmake
target_link_libraries(product_app PRIVATE
  c++
  c++abi
  unwind
)
```

具体是否需要取决于目标 sysroot 和工具链打包方式。

## 7. 激进方式：把某个 target 的 .c 都当 C++ 编译

如果业务侧希望整个 target 都用 C++ 编译，可以对源文件设置 `LANGUAGE CXX`。

示例：

```cmake
set(PRODUCT_C_SOURCES
  src/main.c
  src/module_a.c
  src/module_b.c
)

set_source_files_properties(${PRODUCT_C_SOURCES}
  PROPERTIES LANGUAGE CXX
)

add_executable(product_app
  ${PRODUCT_C_SOURCES}
  src/jit_entry.cpp
)

target_include_directories(product_app PRIVATE
  ${EASYJIT_ROOT}/include
)

target_link_libraries(product_app PRIVATE
  ${EASYJIT_LIB}
)

set_target_properties(product_app PROPERTIES
  LINKER_LANGUAGE CXX
)
```

这种方式会让原来的 `.c` 代码按 C++ 规则编译，可能暴露很多兼容问题。

常见问题：

- `void*` 不能隐式转成具体指针。
- `malloc` 返回值需要强转。
- C99 VLA 在 C++ 中不可用。
- C 的 compound literal 在 C++ 中不可用或兼容性差。
- `restrict`、`_Atomic` 等 C 特性需要处理。
- 变量名用了 C++ 关键字，例如 `new`、`class`、`template`。
- designated initializer 在不同 C++ 标准下兼容性不同。
- C 头文件被 C++ include 时可能需要 `extern "C"`。

因此复杂项目不建议一开始全量打开。可以先挑一个小 target 或一个子目录试。

## 8. 不推荐的做法

不建议在 CMake 里全局写：

```cmake
set(CMAKE_C_COMPILER g++)
```

原因：

- 编译器应在第一次 configure 前由命令行或 toolchain file 指定。
- CMake 已经区分 C 和 CXX 语言，不需要把 C 编译器强行设成 g++。
- 复杂项目里这样改容易影响所有子目录，问题不好定位。

也不建议把所有 `.c` 文件直接改名成 `.cpp`。这会让 diff 很大，也会影响已有构建脚本、文件规则和代码所有权。

## 9. 使用 toolchain file 的项目

如果产品项目使用 toolchain file，建议在 toolchain file 里设置：

```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
```

如果是 clang 交叉编译：

```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_SYSROOT /path/to/sysroot)
```

EasyJIT runtime 静态库必须和目标架构一致。

## 10. 静态链接顺序

如果手写链接命令，静态库顺序很重要。一般是：

```bash
g++ obj1.o obj2.o ... libProduct.a libEasyJitRuntimeWithNeededLLVM.a -lpthread -ldl -o product_app
```

不要把 EasyJIT 静态库放在最前面。

CMake 的 `target_link_libraries(product_app PRIVATE ${EASYJIT_LIB})` 通常会处理好顺序。

如果遇到静态库互相引用导致 unresolved symbol，可以考虑：

```cmake
target_link_options(product_app PRIVATE
  "LINKER:--start-group"
)
target_link_libraries(product_app PRIVATE
  product_core
  ${EASYJIT_LIB}
)
target_link_options(product_app PRIVATE
  "LINKER:--end-group"
)
```

更常见的写法是直接在 `target_link_libraries` 中使用 linker flags：

```cmake
target_link_libraries(product_app PRIVATE
  -Wl,--start-group
  product_core
  ${EASYJIT_LIB}
  -Wl,--end-group
  pthread
  dl
)
```

只有在确实出现循环依赖时再用 group。

## 11. PIC / PIE

如果链接的是静态可执行文件，通常可以关闭 PIE：

```cmake
target_compile_options(product_app PRIVATE
  -fno-pic
  -fno-pie
)

target_link_options(product_app PRIVATE
  -no-pie
)
```

如果生成的是 `.so`，不能关闭 PIC。shared library 需要 `-fPIC`。

检查最终二进制是否 PIE：

```bash
readelf -h product_app | grep Type
```

结果：

```text
Type: EXEC
```

通常是非 PIE。

```text
Type: DYN
```

通常是 PIE 或 shared object。

## 12. 最小完整 CMake 示例

```cmake
cmake_minimum_required(VERSION 3.16)
project(ProductWithEasyJit C CXX)

set(EASYJIT_ROOT "/path/to/easy-jit-main")
set(EASYJIT_LIB  "/path/to/libEasyJitRuntimeWithNeededLLVM.a")

add_executable(product_app
  src/main.c
  src/normal_c_module.c
  src/jit_user.cpp
)

target_include_directories(product_app PRIVATE
  ${EASYJIT_ROOT}/include
)

target_link_libraries(product_app PRIVATE
  ${EASYJIT_LIB}
  pthread
  dl
)

set_target_properties(product_app PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  LINKER_LANGUAGE CXX
)
```

`src/jit_user.cpp` 中可以直接：

```cpp
#include <easy/jit.h>
#include <functional>

using namespace std::placeholders;

static int add(int a, int b) {
  return a + b;
}

int use_easyjit(int x) {
  auto fn = easy::jit(add, _1, 1);
  return fn(x);
}
```

## 13. 常见错误

### 找不到 easy/jit.h

检查：

```cmake
target_include_directories(product_app PRIVATE ${EASYJIT_ROOT}/include)
```

### undefined reference to EasyJIT 符号

检查：

- 是否链接了目标架构的 EasyJIT 静态库。
- 静态库是否放在最终 target 的 `target_link_libraries` 中。
- 链接顺序是否正确。

### undefined reference to C++ 标准库符号

让最终链接使用 C++ linker：

```cmake
set_target_properties(product_app PROPERTIES LINKER_LANGUAGE CXX)
```

或者显式链接：

```cmake
target_link_libraries(product_app PRIVATE stdc++)
```

### 编译 .c 当 C++ 后报很多语法错误

说明原 C 代码不是 C++ 兼容代码。建议退回保守方式：只把使用 EasyJIT 的文件写成 `.cpp`，其他 `.c` 继续按 C 编译。

### 运行时 light backend reject

打开：

```bash
EASYJIT_LIGHT=force
EASYJIT_LIGHT_VERBOSE=1
```

查看 reject reason。先换一个更小、更简单的函数验证接入链路，再处理复杂函数形态。

## 附：可直接拷贝的最小 CMake 模板

仓库根目录下的 `examples/easyjit_cpp_minimal/CMakeLists.txt` 是一份**独立、自包含**的最小模板，
不 `add_subdirectory` 主仓库，对接路径全部走三个 cache 变量
（`EASYJIT_ROOT` / `EASYJIT_LIB` / `EASYJIT_PASS`）。

第一次把 EasyJIT 接进产品工程时，建议：

1. 先在那个目录里 `cmake -S . -B build -DEASYJIT_ROOT=... -DEASYJIT_LIB=... -DEASYJIT_PASS=...`
   把 demo 编出来跑通；
2. 把它的 `target_compile_options(... -Xclang -fpass-plugin=...)` 和
   `target_link_libraries(... ${EASYJIT_LIB} pthread dl)` 两段照抄进你的工程；
3. 板端再跑一次 `tools/easyjit_light_selfcheck.cpp` 做端到端自检。

`examples/easyjit_cpp_minimal/README.md` 列出了 4 个常见坑（plugin ABI 不匹配、bitcode 缺失、链接顺序、
LINKER_LANGUAGE）和对应排查办法，遇到链接错误可以先翻一遍。
