# EasyJIT C++ 版本使用教程

本文面向已经有 C/C++ 业务代码、希望直接使用 EasyJIT C++ 接口的同事。重点是照着现有用例理解 `easy::jit` 怎么写，不涉及 EasyJIT 内部实现。

## 1. 基本概念

EasyJIT 的 C++ 接口主要做一件事：把一个普通函数按“部分参数固定、部分参数运行时传入”的方式编译成一个新的可调用对象。

最常见写法：

```cpp
#include <easy/jit.h>
#include <functional>

using namespace std::placeholders;

int add(int a, int b) {
  return a + b;
}

int main() {
  auto inc = easy::jit(add, _1, 1);
  int y = inc(4);  // 等价于 add(4, 1)，返回 5
}
```

可以参考仓库里的用例：

```text
tests/simple/int_a.cpp
tests/simple/float_a.cpp
tests/simple/double_a.cpp
tests/simple/long_a.cpp
```

这里的 `_1` 表示“这个参数保留到 JIT 后的新函数里，运行时再传”。普通值 `1` 表示“这个参数在 JIT 编译时就固定下来”。

## 2. 参数绑定规则

假设原始函数是：

```cpp
int f(int a, int b, int c);
```

那么：

```cpp
auto g = easy::jit(f, _1, 10, _2);
```

得到的新函数签名可以理解为：

```cpp
int g(int a, int c);
```

调用：

```cpp
int r = g(3, 7);
```

等价于：

```cpp
int r = f(3, 10, 7);
```

常用占位符来自标准库：

```cpp
#include <functional>
using namespace std::placeholders;
```

常见模式：

```cpp
auto all_runtime = easy::jit(f, _1, _2, _3);
auto fix_tail    = easy::jit(f, _1, _2, 100);
auto fix_head    = easy::jit(f, 100, _1, _2);
auto reorder     = easy::jit(f, _2, 100, _1);
```

建议业务侧先从“固定少量配置参数，保留数据指针和输出指针为 runtime 参数”开始，不要一上来固定所有东西。

## 3. 固定指针参数

如果某个指针指向的内容在 JIT 期间不变，可以把指针作为固定参数传进去。

参考：

```text
tests/simple/int_ptr_a.cpp
```

简化版：

```cpp
#include <easy/jit.h>
#include <functional>

using namespace std::placeholders;

static int add_from_ptr(int x, int const *p) {
  return x + *p;
}

int main() {
  static int value = 4321;
  auto fn = easy::jit(add_from_ptr, _1, &value);
  int r = fn(4);  // 4325
}
```

这类写法适合：

- 固定配置表
- 固定系数数组
- 固定只读参数

注意：如果指针指向的数据会变，JIT 编译时是否能看到新值取决于具体绑定方式和优化行为。业务上最好把“会变的数据”作为 `_1` / `_2` 这类 runtime 参数传入。

## 4. snapshot 固定结构体

如果配置结构体比较复杂，可以用 `easy::snapshot` 把当前结构体内容快照进 JIT。

参考：

```text
tests/simple/snapshot_struct.cpp
tests/simple/snapshot_array.cpp
tests/simple/snapshot_struct_by_value.cpp
tests/simple/config_snapshot.cpp
```

简化版：

```cpp
#include <easy/jit.h>
#include <functional>

using namespace std::placeholders;

struct Inner {
  int values[4];
};

struct Config {
  int base;
  Inner inner;
};

static int eval(int x, Config const &cfg) {
  return x + cfg.base + cfg.inner.values[2];
}

int main() {
  Config cfg{};
  cfg.base = 10;
  cfg.inner.values[2] = 30;

  auto fn = easy::jit(eval, _1, easy::snapshot(cfg));
  int r = fn(5);  // 45
}
```

适合 snapshot 的内容：

- 部署期间不变的配置
- 小型结构体
- 只读系数
- 固定长度数组

不建议 snapshot 的内容：

- 每次调用都变化的数据
- 很大的输入输出 buffer
- 生命周期不清晰的临时对象

## 5. 固定数组和循环边界

EasyJIT 常见收益来自“固定循环边界、固定系数、固定配置”，让编译器能做常量传播、循环展开、死代码删除。

参考：

```text
tests/simple/unroll.cpp
tests/c_api/wireless_beamform.c
tests/c_api/wireless_pointer_perf.c
```

简化版：

```cpp
#include <easy/jit.h>
#include <functional>
#include <vector>

using namespace std::placeholders;

int dot(int *a, std::vector<int> b, int n) {
  int x = 0;
  for (int i = 0; i != n; ++i) {
    x += a[i] * b[i];
  }
  return x;
}

int main() {
  std::vector<int> a = {1, 2, 3, 4};
  std::vector<int> b = {4, 3, 2, 1};

  auto dot_a = easy::jit(dot, a.data(), _1, 4);
  int r = dot_a(b);  // 20
}
```

这里固定了：

- `a.data()`
- `n = 4`

运行时保留：

- `b`

这类写法比“只固定一个普通整数”更可能带来收益。

## 6. 缓存 JIT 结果

不要在高频路径里每次都重新 `easy::jit`。如果同一组配置会重复使用，应使用 cache。

参考：

```text
tests/simple/cache.cpp
include/easy/code_cache.h
```

简化版：

```cpp
#include <easy/code_cache.h>
#include <functional>

using namespace std::placeholders;

int mul(int a, int b) {
  return a * b;
}

int main() {
  easy::Cache<int> cache;

  int factor = 3;
  auto const &fn = cache.jit(factor, mul, _1, factor);
  int r = fn(4);  // 12
}
```

这里 `factor` 同时作为 cache key 和 JIT 固定参数。业务里可以把 key 设计成配置 ID、模式号、参数组合 hash 等。

## 7. dump IR 排查问题

如果想看 EasyJIT 编译后的 IR，可以用 `easy::options::dump_ir`。

参考：

```text
tests/simple/int_ptr_a.cpp
tests/simple/unroll.cpp
tests/simple/opt_level.cpp
```

示例：

```cpp
#include <easy/jit.h>
#include <easy/options.h>
#include <functional>

using namespace std::placeholders;

int add(int a, int b) {
  return a + b;
}

int main() {
  auto fn = easy::jit(add, _1, 1, easy::options::dump_ir("jit.ll"));
  return fn(10);
}
```

生成的 `jit.ll` 可以用来确认：

- 固定参数有没有被常量传播
- 循环有没有被简化
- 是否出现了 light backend 不支持的 IR 形态

## 8. light backend 使用建议

如果运行环境使用轻量 AArch64 backend，建议优先选择形态简单的热点函数：

- 标量整数和浮点计算
- 指针 load/store
- 固定配置 + runtime 数据指针
- 简单分支和 select
- 固定循环边界或可被优化简化的循环

尽量避免一开始就选择：

- vector IR
- varargs
- exception / RTTI / 复杂 C++ 对象生命周期
- 大结构体按值传参
- 函数指针复杂跳转
- 过多跨 basic block 的活跃值

调试时可以打开：

```bash
EASYJIT_LIGHT=force
EASYJIT_LIGHT_VERBOSE=1
```

如果 light backend 不支持某个函数，会打印 reject reason。先用小函数跑通，再逐步扩大到真实业务 kernel。

## 9. 推荐接入步骤

建议业务侧按这个顺序来：

1. 找一个纯计算热点函数。
2. 把这个函数所在文件改成 C++ 编译，或者放到 `.cpp` 文件里。
3. 先用 `easy::jit(fn, _1, 固定参数...)` 跑通功能。
4. 加 cache，避免重复编译。
5. 用 `dump_ir` 检查固定参数是否真的被优化。
6. 用原函数和 JIT 后函数做结果对比。
7. 再做性能对比，区分 compile time、execute time、amortized total time。

最开始不要追求一次接入很大的业务流程。EasyJIT 更适合先从“参数稳定、调用频繁、计算密集”的小 kernel 开始。

## 10. Light backend 与 loop unroll / vector IR（round 11）

从 round 11 起，EasyJIT 运行时的优化 pipeline 在 `OptLevel >= 2`
时会自动加入 LLVM 的标量 loop unroll passes
（`LoopSimplify` → `LCSSA` → `LoopRotate` → `LoopUnroll`）。
配合 EasyJIT 自身的 snapshot / `ConstStructPropagate` 折叠能力，
**循环边界在 JIT 时变成常量后，会被完整展开成 straight-line scalar IR**，
light backend 就能继续走原有的标量 load/store/arith/branch 路径完成编译。

典型可被展开的模式：

```cpp
struct Cfg { int n; int scale; };

int kernel(const Cfg* cfg, const int* a, const int* b) {
    int acc = 0;
    for (int i = 0; i < cfg->n; ++i)
        acc += (a[i] + b[i]) * cfg->scale;
    return acc;
}
// JIT 时把 cfg 绑死 (n=8, scale=K)，runtime 只传 a/b
```

### 重要边界

- **light backend 仍然不支持 vector IR / NEON**。loop unroll 只是把
  循环展开成多次 scalar 操作，不会引入 vector 指令。
- 如果业务侧的编译器（特别是 `clang -O3`）在前端生成了 vector IR，
  light backend 现在会以下面这类清晰原因 reject：
  - `vector IR unsupported (return type)`
  - `vector IR unsupported (argument)`
  - `vector IR unsupported (instruction)`
  - `vector IR unsupported (operand)`
- 因此**编译 JIT 相关的 TU 时**，推荐加：

  ```
  -fno-vectorize -fno-slp-vectorize
  ```

  避免在前端阶段先把循环 vectorize 掉。
- 若展开后的代码体过大、寄存器压力过高，light backend 会以
  `scratch OOM`/`spill cap`/`frame > 4095B` 等原因 reject。
  此时可以在前端额外加 `-fno-unroll-loops`，
  或者把 runtime 的 `opt_level` 降到 1。

详细行为参见 `runtime/LightBackend_LIMITATIONS.md` 的
“Round 11 — Scalar Loop Unroll” 一节。

## 11. 跑一遍仓库自带的最小用例与自检

写新代码前，强烈建议先在你的目标环境上跑通仓库自带的两份样例：

- `examples/easyjit_cpp_minimal/`（4 个 case：`int` / snapshot / 小循环 / cache）
  - 独立 CMake 工程模板，不依赖主仓库构建系统，可以直接拷到产品工程里改路径。
  - 详见该目录下的 `README.md`。
- `tools/easyjit_light_selfcheck.cpp`（6 个 case：`int` / `double` / snapshot / pointer load-store / unroll(n=8) / cache）
  - 板端自检程序。一行 clang 命令编出来，配 `EASYJIT_LIGHT=force EASYJIT_LIGHT_VERBOSE=1` 用，
    一次性确认轻量 AArch64 后端在目标板子上 6 条路径全 PASS。
  - 文件头的注释里直接列出了推荐的编译命令。

跑完这两步再去把 EasyJIT 接到真实业务函数上，能少 90% 的“是接入坏了还是后端坏了”二义性。

## 12. dump 出 light backend 真正生成的机器码

如果只看 IR 已经无法回答“是不是后端这一改让代码变差了”，可以让 runtime 把每个被 light backend 接受的函数的**机器码 bytes** 落盘：

| 环境变量 | 作用 |
| --- | --- |
| `EASYJIT_LIGHT_DUMP_CODE_DIR=<dir>` | 每个成功编译的函数写一个 `<dir>/NNNN_<fnname>.bin`（4 位序号原子递增、线程安全）。目录不存在会尝试 `mkdir(0755)`，失败只在 stderr 打一条 warning，不影响 JIT 继续跑。 |
| `EASYJIT_LIGHT_DUMP_META=1` | stderr 多打一行 `[easyjit][light] code name=<n> bytes=<n> file=<path>`。 |
| `EASYJIT_LIGHT_DUMP_CODE_BASENAME=<prefix>` | 可选，在序号前再加前缀，便于一个目录里做 A/B 对比。 |

输出文件就是裸的 AArch64 指令流（小端、每条 4 字节），不含 ELF 头，文件大小等于 `bytes=`。反汇编一下：

```bash
# 仓库根目录跑一遍 selfcheck 同时 dump
rm -rf /tmp/ejcode
EASYJIT_LIGHT=force \
EASYJIT_LIGHT_VERBOSE=1 \
EASYJIT_LIGHT_DUMP_CODE_DIR=/tmp/ejcode \
EASYJIT_LIGHT_DUMP_META=1 \
  /tmp/easyjit_light_selfcheck --iters 1 --verbose

# 反汇编
aarch64-linux-gnu-objdump -D -b binary -m aarch64 /tmp/ejcode/0001_*.bin
# 或者
llvm-objdump -D -b binary -m aarch64 /tmp/ejcode/0001_*.bin
# 或者
hexdump -Cv /tmp/ejcode/0001_*.bin
```

排查性能问题时建议两个 dump 一起开：

1. 先 `easy::options::dump_ir("/tmp/foo.ll")` 拿到 specialized IR；
2. 再 `EASYJIT_LIGHT_DUMP_CODE_DIR=...` 拿到对应的机器码；
3. 比较 `*.ll` 和 `*.bin` 能直接告诉你回归到底是“相同 IR 后端劣化”还是“optimizer 在 backend 前生成了不同 IR”。

详见 `runtime/LightBackend_LIMITATIONS.md` 的 “Round 12 — Code Dump Diagnostics” 一节。
