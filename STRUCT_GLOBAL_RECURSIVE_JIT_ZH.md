# Struct 常量快照、全局 Struct 快照与递归 JIT

这份文档整理三个容易混在一起的能力：

- 函数参数里的 `struct` 作为编译期常量传入 JIT。
- 全局 `struct` 作为编译期常量传入 JIT。
- 递归 JIT 以及 `EASY_JIT_KEEP_NATIVE_SCOPE()` 的使用方式。

核心区别先放在最前面：普通参数快照是实参绑定，写在 `easy::jit(fn, ..., easy::snapshot(cfg))` 的参数位置；全局变量快照是 option，写成 `easy::options::global_snapshot(g_cfg)` 或 `easy::options::global_partial_snapshot(...)`，它不占原函数的参数位。

## 1. 参数 struct 作为常量传入

### 1.1 完整传入 struct

当函数本来接收一个 `struct` 参数，并且调用 JIT 时已经知道这个结构体内容，可以用 `easy::snapshot(cfg)` 把整块结构体冻结成编译期常量。

```cpp
#include <easy/jit.h>

#include <cstdio>
#include <functional>

using namespace std::placeholders;

struct Inner {
  int values[4];
};

struct Config {
  int base;
  Inner inner;
};

static int eval(int x, Config const& cfg) {
  return x + cfg.base + cfg.inner.values[2];
}

int main() {
  Config cfg{};
  cfg.base = 10;
  cfg.inner.values[2] = 30;

  auto fn = easy::jit(eval, _1, easy::snapshot(cfg));

  cfg.base = 1000;
  cfg.inner.values[2] = 2000;

  std::printf("result=%d\n", fn(5)); // 45
}
```

这里 `cfg` 在 JIT 编译时被序列化进 `Context`，`eval` 的第二个参数被常量替换。后面再改 `cfg` 不会影响已经生成的机器码，所以结果仍然是 `5 + 10 + 30 = 45`。

### 1.2 partial 传入：`bind_field`

如果只想固定一部分字段，其余字段仍从运行时参数读取，可以用 `easy::snapshot(cfg, easy::bind_field(...))`。

```cpp
#include <easy/jit.h>

#include <cstdio>
#include <functional>

using namespace std::placeholders;

struct Config {
  int enabled;
  int bias;
};

static int eval(int x, Config const& cfg) {
  if (cfg.enabled)
    return x + cfg.bias;
  return x - 99;
}

int main() {
  Config cfg{1, 7};

  auto fn = easy::jit(
      eval,
      _1,
      easy::snapshot(cfg, easy::bind_field(&Config::enabled)));

  Config runtime_cfg{0, 1000};
  std::printf("result=%d\n", fn(5, runtime_cfg)); // 1005
}
```

这段里只有 `enabled` 被固定成 `1`，所以 JIT 可以把 `if (cfg.enabled)` 这条分支消掉；`bias` 没有被绑定，仍然来自调用 `fn(5, runtime_cfg)` 时传入的 `runtime_cfg.bias`，结果是 `5 + 1000 = 1005`。

### 1.3 partial 传入：`bind_array`

`bind_array` 用来处理结构体里的指针字段。它固定的不是指针地址本身，而是指针指向的数组内容。

```cpp
#include <easy/jit.h>
#include <easy/snapshot.h>

#include <cstdio>

struct PointerConfig {
  int base;
  const int* array;
  int unused;
};

int eval_pointer_snapshot(int x, PointerConfig const& cfg) {
  const int* local = cfg.array;
  return x + cfg.base + local[2];
}

int main() {
  int data[4] = {11, 22, 33, 44};
  PointerConfig cfg{7, data, 99};

  auto fn = easy::jit(
      eval_pointer_snapshot,
      5,
      easy::snapshot(cfg, easy::bind_array(&PointerConfig::array, 4)));

  data[2] = 3000;

  std::printf("result=%d\n", fn()); // 45
}
```

这里 `array[0..3]` 的内容在编译时被拷贝下来，后面再改 `data[2]` 不会影响 `fn()`。如果只写 `easy::snapshot(cfg)` 而不写 `bind_array`，只能固定结构体里保存的指针值，不能把指针背后的数组内容变成常量。

### 1.4 嵌套 struct 场景

完整快照天然支持嵌套结构体，只要整个对象是 trivially copyable。

```cpp
struct Nested {
  int scale;
  int table[4];
};

struct Config {
  int bias;
  Nested nested;
};

static int eval_nested(int x, Config const& cfg) {
  return x * cfg.nested.scale + cfg.bias + cfg.nested.table[1];
}

Config cfg{};
cfg.bias = 3;
cfg.nested.scale = 2;
cfg.nested.table[1] = 20;

auto fn = easy::jit(eval_nested, _1, easy::snapshot(cfg));
```

如果是 partial 快照，目前 C++ helper 接收的是 `Field Object::*` 或 `Pointer Object::*` 这种一级 member pointer。因此可以直接绑定顶层字段，或者把整个嵌套对象当作一个顶层字段绑定：

```cpp
auto fn = easy::jit(
    eval_nested,
    _1,
    easy::snapshot(cfg, easy::bind_field(&Config::nested)));
```

上面会把 `nested` 这一整块固定住，但没有直接表达“只绑定 `Config::nested.scale`，不绑定 `Config::nested.table`”的 C++ 路径 helper。需要这种更细粒度能力时，可以先改成顶层字段、绑定整个 nested 子对象，或者后续扩展一个支持 member path 的 binding API。

## 2. 全局 struct 作为常量传入

### 2.1 完整传入全局 struct

全局快照不是普通实参，它是 `easy::options` 里的编译选项。函数里仍然直接访问全局变量，JIT 编译时通过 option 告诉 pass：这个全局对象可以按当前内容当成常量。

```cpp
#include <easy/jit.h>

#include <cstdio>
#include <functional>

using namespace std::placeholders;

struct GlobalConfig {
  int enabled;
  int bias;
};

GlobalConfig g_cfg{1, 7};

int eval_global_snapshot(int x) {
  if (g_cfg.enabled)
    return x + g_cfg.bias;
  return x - 99;
}

int main() {
  auto fn = easy::jit(
      eval_global_snapshot,
      _1,
      easy::options::global_snapshot(g_cfg));

  g_cfg.enabled = 0;
  g_cfg.bias = 1000;

  std::printf("result=%d\n", fn(5)); // 12
}
```

这里 `global_snapshot(g_cfg)` 把 `g_cfg{1, 7}` 作为编译期常量传给优化流程。编译之后再把全局变量改成 `{0, 1000}`，已经生成的 `fn` 不会跟着变，结果仍是 `5 + 7 = 12`。

### 2.2 partial 全局传入：`bind_field`

只固定全局结构体里的某些字段时，用 `easy::options::global_partial_snapshot(...)`。

```cpp
struct PartialGlobalConfig {
  int enabled;
  int bias;
};

PartialGlobalConfig g_partial_cfg{1, 7};

int eval_global_partial(int x) {
  if (g_partial_cfg.enabled)
    return x + g_partial_cfg.bias;
  return x - 99;
}

auto partial_fn = easy::jit(
    eval_global_partial,
    _1,
    easy::options::global_partial_snapshot(
        g_partial_cfg,
        easy::bind_field(&PartialGlobalConfig::enabled)));

g_partial_cfg.enabled = 0;
g_partial_cfg.bias = 1000;

std::printf("result=%d\n", partial_fn(5)); // 1005
```

`enabled` 被固定成 `1`，所以分支被优化成真分支；`bias` 没固定，仍然从运行时的全局内存读取，所以修改后的 `1000` 会生效。

### 2.3 partial 全局传入：`bind_array`

全局结构体里有指针字段时，也可以用 `bind_array` 固定指针背后的数组内容。

```cpp
struct PartialGlobalArrayConfig {
  int enabled;
  int bias;
  const int* values;
};

static int g_values[] = {10, 20, 30};
PartialGlobalArrayConfig g_partial_array_cfg{1, 2, g_values};

int eval_global_partial_array(int idx) {
  if (g_partial_array_cfg.enabled)
    return g_partial_array_cfg.values[idx] + g_partial_array_cfg.bias;
  return -1;
}

auto partial_array_fn = easy::jit(
    eval_global_partial_array,
    _1,
    easy::options::global_partial_snapshot(
        g_partial_array_cfg,
        easy::bind_field(&PartialGlobalArrayConfig::enabled),
        easy::bind_array(&PartialGlobalArrayConfig::values, 3)));

g_partial_array_cfg.enabled = 0;
g_partial_array_cfg.bias = 50;
g_values[1] = 200;

std::printf("result=%d\n", partial_array_fn(1)); // 70
```

这里 `enabled` 固定为 `1`，`values[0..2]` 固定为 `{10, 20, 30}`，但 `bias` 仍然从全局变量读取。因此结果是 `values[1]` 的编译期值 `20` 加上运行时全局 `bias = 50`，得到 `70`。

### 2.4 全局嵌套 struct

完整全局快照同样天然支持嵌套结构体。

```cpp
struct Nested {
  int scale;
  int table[4];
};

struct GlobalConfig {
  int bias;
  Nested nested;
};

GlobalConfig g_nested_cfg{3, {2, {10, 20, 30, 40}}};

int eval_global_nested(int x) {
  return x * g_nested_cfg.nested.scale +
         g_nested_cfg.bias +
         g_nested_cfg.nested.table[1];
}

auto fn = easy::jit(
    eval_global_nested,
    _1,
    easy::options::global_snapshot(g_nested_cfg));
```

partial 全局快照的嵌套限制和普通参数一致：当前 C++ binding helper 直接表达的是顶层字段。可以绑定整个 nested 子对象：

```cpp
auto fn = easy::jit(
    eval_global_nested,
    _1,
    easy::options::global_partial_snapshot(
        g_nested_cfg,
        easy::bind_field(&GlobalConfig::nested)));
```

这会把 `nested` 整块作为常量；如果需要只固定 nested 内部某个叶子字段，当前接口还需要扩展 member path，或者把目标字段提到顶层。

### 2.5 与 `easy::Cache` 的关系

全局快照尤其要小心 cache。`easy::Cache<>` 的自动 key 是“函数地址 + 本次构造出的 `Context`”；显式 key 版本 `easy::Cache<Key>` 则完全按用户传入的 key 判断是否复用。无论哪种形式，cache 都不会监听全局变量的写入。

```cpp
static easy::Cache<> cache;

GlobalConfig g_cfg{1, 7};

auto const& fn1 = cache.jit(
    eval_global_snapshot,
    _1,
    easy::options::global_snapshot(g_cfg));

g_cfg.bias = 1000;

auto const& fn2 = cache.jit(
    eval_global_snapshot,
    _1,
    easy::options::global_snapshot(g_cfg));
```

如果重新调用 `cache.jit(...)` 时 option 会重新构造 `Context`，新的全局快照内容有机会进入自动 key；但 cache 本身不会“感知”中间那次 `g_cfg.bias = 1000`，也不会主动让已经返回的 `fn1` 失效。更危险的是显式 key：

```cpp
static easy::Cache<int> cache;

GlobalConfig g_cfg{1, 7};

auto const& fn1 = cache.jit(
    0,
    eval_global_snapshot,
    _1,
    easy::options::global_snapshot(g_cfg));

g_cfg.bias = 1000;

auto const& fn2 = cache.jit(
    0,
    eval_global_snapshot,
    _1,
    easy::options::global_snapshot(g_cfg)); // 仍然命中 key=0 的旧代码
```

如果全局变量变化会影响 specialization，推荐把版本号放进显式 key，或者在变化后手动清 cache。

```cpp
static easy::Cache<int> cache;

int cfg_version = 0;

auto rebuild = [&] {
  return cache.jit(
      cfg_version,
      eval_global_snapshot,
      _1,
      easy::options::global_snapshot(g_cfg));
};

auto const& fn1 = rebuild();

g_cfg.bias = 1000;
++cfg_version;

auto const& fn2 = rebuild(); // key 变了，会重新编译
```

也可以直接清空：

```cpp
cache.clear();

auto const& fn = cache.jit(
    eval_global_snapshot,
    _1,
    easy::options::global_snapshot(g_cfg));
```

C API 对应的是：

```c
easyjit_error_t err = easyjit_cache_clear(cache);
if (err != EASYJIT_OK) {
    fprintf(stderr, "cache_clear failed: %s\n", easyjit_get_last_error());
}
```

结论是：全局快照是 option，不是运行时观察器。它描述的是“这次编译时的全局对象状态”；如果全局对象后续变化，需要用户用 key/version 或 `clear()` 明确告诉 cache。

## 3. 递归 JIT 的使用场景和结果

### 3.1 默认不递归 JIT

默认情况下，一个被 JIT 的函数直接调用 helper 时，entry 本身会进入 JIT，helper 保持 native。这个默认值更保守：不会因为外层函数被 JIT，就自动把下面一串 direct callee 都搬进 JIT 模块。

```cpp
#include <easy/jit.h>

#include <cstdio>
#include <functional>

using namespace std::placeholders;

__attribute__((noinline))
int helper_jit(int x) {
  return x * 2;
}

__attribute__((noinline))
int outer(int x) {
  return helper_jit(x) + 1;
}

int main() {
  auto fn = easy::jit(outer, _1);
  std::printf("result=%d\n", fn(5)); // 11
}
```

这里结果仍然是普通函数调用语义；实现上看，`outer` 是 JIT 版本，`helper_jit` 会被剥成外部声明，并通过 EasyJIT 的 global mapping 解析到宿主进程里的原生函数地址。

```llvm
; 默认：helper_jit 不进入 JIT 模块，只保留声明
declare i32 @_Z10helper_jiti(i32)
```

### 3.2 显式开启递归 JIT

如果确实希望把 direct callee 一起编进 JIT 模块，需要在外侧调用点显式加 option：

```cpp
auto fn = easy::jit(
    outer,
    _1,
    easy::options::recursive_jit());
```

加了 `easy::options::recursive_jit()` 后，`outer` 引用到的 helper 定义会保留在 JIT module 里。这个适合“外层入口很小，但真正热路径在 helper 里”的场景。

```llvm
; recursive_jit option：helper_jit 保留定义，进入 JIT 模块
define internal i32 @_Z10helper_jiti(i32 %x) {
  ...
}
```

### 3.3 部分递归：保留某些调用为 native

开启递归 JIT 后，如果某个 helper 不希望被递归 JIT，比如里面有不适合搬进 JIT 的库调用、平台 API、复杂副作用，或者暂时想保持与原生实现完全一致，可以在调用点外面加 `EASY_JIT_KEEP_NATIVE_SCOPE()`。

```cpp
#include <easy/jit.h>

#include <cstdio>
#include <functional>

using namespace std::placeholders;

__attribute__((noinline))
int helper_jit(int x) {
  return x * 2;
}

__attribute__((noinline))
int helper_native(int x) {
  return x * 3;
}

__attribute__((noinline))
int outer(int x) {
  int result = helper_jit(x);
  {
    EASY_JIT_KEEP_NATIVE_SCOPE();
    result += helper_native(x);
  }
  return result;
}

int main() {
  auto fn = easy::jit(
      outer,
      _1,
      easy::options::recursive_jit());
  std::printf("result=%d\n", fn(5)); // 25
}
```

结果是 `helper_jit` 仍然递归 JIT，`helper_native` 保持 native。当前测试里还会检查 dump 出来的 IR：`helper_native` 不再是 `define`，而是 `declare`，再通过 EasyJIT 写入的 global mapping 解析到宿主进程里的真实函数地址。

```llvm
; helper_jit 会保留定义，进入 JIT 模块
define internal i32 @_Z10helper_jiti(i32 %x) {
  ...
}

; helper_native 只保留声明，运行时解析到 native 地址
declare i32 @_Z13helper_nativei(i32)
```

### 3.4 不支持在 JIT 后的机器码里再次调用 `easy::jit`

一个容易误用的场景是在被 JIT 的函数内部再调用 `easy::jit`。

```cpp
int inner(int x, int bias) {
  return x + bias;
}

int outer_bad(int x) {
  auto fn = easy::jit(inner, std::placeholders::_1, 7);
  return fn(x);
}

auto outer_fn = easy::jit(outer_bad, std::placeholders::_1);
```

这个模式不建议也不作为当前支持目标。原因是 EasyJIT 的 bitcode 发现和注册发生在原始编译产物上，JIT 后机器码里的函数地址不是原始 host function address，也不会重新跑 pass 去登记新的 bitcode。实际效果通常不是“嵌套生成一份新的 JIT”，而是找不到正确的 bitcode/上下文，甚至触发崩溃。

如果需求是“调用某个入口时，把它下面的一部分 helper 也 JIT 掉”，应该在外层 `easy::jit(...)` 上加 `easy::options::recursive_jit()`；如果需求是“递归 JIT 开启后，某些 helper 不要 JIT”，就在调用点加 `EASY_JIT_KEEP_NATIVE_SCOPE()`。这个模型更接近原本的函数调用写法，也避免在 JIT 产物里再次启动编译器。

### 3.5 当前边界

`EASY_JIT_KEEP_NATIVE_SCOPE()` 主要覆盖直接调用场景：

```cpp
{
  EASY_JIT_KEEP_NATIVE_SCOPE();
  result += helper_native(x); // 直接 call，保持 native
}
```

函数指针、虚调用、复杂控制流里的间接调用是否能被精确识别，取决于前端和优化后 IR 是否还能看成明确的 direct call。需要强控制时，建议把需要保留 native 的调用写成清晰的直接调用，并把 scope 放在 call 前后的最小代码块里。
