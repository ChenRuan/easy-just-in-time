# EasyJIT C API 中文教程

这篇文档写给第一次接触 EasyJIT C API 的人。

目标很简单：

- 先搞清楚它到底在做什么
- 再知道常见场景该用哪个接口
- 最后能照着现有 C 用例自己改出一个 JIT 版本

如果你之前完全没看过这套接口，也没关系，我们从最基本的地方开始。

相关参考文件：

- `include/easy/easyjit_c.h`
- `tests/c_api/README.md`
- `tests/c_api/add_int.c`
- `tests/c_api/array_snapshot.c`
- `tests/c_api/pointer_field_snapshot.c`
- `tests/c_api/partial_struct_binding.c`
- `tests/c_api/config_process_easyjit.c`

---

## 1. 先把它理解成什么

EasyJIT C API 做的事情可以简单理解成：

> 你先告诉 EasyJIT：一个函数的哪些参数在这次调用里是固定不变的，哪些参数还是运行时传入的。  
> 它再根据这些“固定信息”编译出一个更专用的版本。

所以它不是“直接调用某个神秘的新函数”，而是分两段：

1. 先描述这次要怎么专门化
2. 再编译并拿到新的函数指针

---

## 2. 最常见的完整流程

一个标准的 C API 使用流程通常是这样：

1. 创建 `context`
2. 按目标函数的参数顺序，一个个把参数绑定到 `context`
3. 调用 `easyjit_compile`
4. 销毁 `context`
5. 取回专门化后的函数指针
6. 调用这个函数
7. 最后销毁 JIT 函数句柄

最小骨架如下：

```c
easyjit_context_t ctx = NULL;
easyjit_function_t fn = NULL;
void *raw = NULL;

easyjit_context_create(&ctx);

/* 这里按参数顺序，一项项往 ctx 里填 */

easyjit_compile((void *)target_function, ctx, &fn);
easyjit_context_destroy(ctx);

easyjit_get_function_pointer(fn, &raw);

/* 转成你真正要调用的函数类型 */
my_jit_fn_t f = (my_jit_fn_t)raw;
int result = f(...);

easyjit_function_destroy(fn);
```

如果你只先记住这一段，后面很多事情都会顺很多。

---

## 3. 最重要的规则：参数必须按原函数顺序绑定

假设原函数是：

```c
int foo(const Config *cfg, int x, int y);
```

那你在 `context` 里绑定参数时，也必须按这个顺序理解：

1. 第一个参数：`cfg`
2. 第二个参数：`x`
3. 第三个参数：`y`

你可以决定：

- 哪个参数保留成运行时参数
- 哪个参数固定成常量
- 哪个结构体整块 snapshot
- 哪个结构体只固定部分字段

但是顺序不能跳。

这点非常重要，因为很多“为什么结果不对”最后都能追到这里。

---

## 4. 先建立一个直觉：有哪些绑定方式

你可以把最常用的绑定方式先记成这几类。

### 4.1 运行时参数

用：

```c
easyjit_context_set_forward(...)
```

表示：

- 这个参数不要固定
- 最后还会出现在专门化后的函数签名里

### 4.2 普通常量参数

用：

```c
easyjit_context_set_int(...)
easyjit_context_set_float(...)
easyjit_context_set_pointer(...)
```

表示：

- 这个参数整个固定成一个常量

### 4.3 整个结构体都固定

用：

```c
easyjit_context_set_snapshot(...)
```

表示：

- 整个结构体内容都拷进去
- 后续访问它的字段时，JIT 可以把它们当成常量

### 4.4 结构体只有部分字段固定

用：

```c
easyjit_context_set_partial_struct(...)
easyjit_context_bind_field(...)
easyjit_context_bind_array(...)
```

表示：

- struct 这个参数本身还保留在运行时
- 但其中一部分成员会被当成编译期常量

---

## 5. 先看几个最常见的场景

### 场景 A：普通标量参数

原函数：

```c
int add_scale(int x, int bias, float scale);
```

如果你想让：

- `x` 运行时传入
- `bias` 固定成 `2`
- `scale` 固定成 `1.5`

写法就是：

```c
easyjit_context_set_forward(ctx, 0); /* x */
easyjit_context_set_int(ctx, 2);     /* bias */
easyjit_context_set_float(ctx, 1.5); /* scale */
```

最终得到的 JIT 函数只剩一个参数：

```c
int (*spec)(int);
```

### 场景 B：整个 struct 都稳定

原函数：

```c
int process(const Config *cfg, int x);
```

如果 `cfg` 整个都稳定，最简单的写法就是：

```c
easyjit_context_set_snapshot(ctx, &cfg, sizeof(cfg));
easyjit_context_set_forward(ctx, 0); /* x */
```

这表示：

- `cfg` 整个被固定了
- `x` 保留为运行时参数

最终函数签名大致变成：

```c
int (*spec)(int);
```

### 场景 C：只有部分 struct 字段稳定

原函数还是：

```c
int process(const Config *cfg, int x);
```

但这次只有：

- `cfg->enabled`
- `cfg->threshold`

是稳定配置，其他字段仍然变化。

那就不要再用整块 `snapshot`，而是改成：

```c
easyjit_context_set_partial_struct(ctx, 0);
easyjit_context_bind_field(ctx,
                           offsetof(Config, enabled),
                           &cfg.enabled,
                           sizeof(cfg.enabled));
easyjit_context_bind_field(ctx,
                           offsetof(Config, threshold),
                           &cfg.threshold,
                           sizeof(cfg.threshold));
easyjit_context_set_forward(ctx, 1); /* x */
```

这里的含义是：

- `cfg` 这个 struct 指针仍然是运行时参数
- 但 `enabled` 和 `threshold` 两个字段被固定成常量
- `x` 继续运行时传入

最终函数签名还是：

```c
int (*spec)(const Config *, int);
```

这就是“部分常量化”和“整块 snapshot”的最大区别。

### 场景 D：结构体里有数组指针

例如：

```c
typedef struct {
    int enabled;
    const float *weights;
    int count;
} Config;
```

如果你想让：

- `enabled` 固定
- `weights[0..count)` 也固定成常量数组

可以这样写：

```c
easyjit_context_set_partial_struct(ctx, 0);
easyjit_context_bind_field(ctx,
                           offsetof(Config, enabled),
                           &cfg.enabled,
                           sizeof(cfg.enabled));
easyjit_context_bind_array(ctx,
                           offsetof(Config, weights),
                           cfg.weights,
                           cfg.count,
                           sizeof(cfg.weights[0]));
easyjit_context_set_forward(ctx, 1);
```

这里要特别注意：

- `bind_field` 适合普通字段
- `bind_array` 适合“struct 里的指针字段所指向的数组”

---

## 6. 每个常用接口具体是干什么的

下面按实际使用顺序讲。

### `easyjit_context_create(easyjit_context_t *out_ctx)`

作用：

- 创建一个空的 specialization context

参数：

- `out_ctx`
  - 输出参数
  - 成功后返回一个可用的 `ctx`

例子：

```c
easyjit_context_t ctx = NULL;
if (easyjit_context_create(&ctx) != EASYJIT_OK) {
    fprintf(stderr, "context_create failed: %s\n", easyjit_get_last_error());
    return 1;
}
```

### `easyjit_context_destroy(easyjit_context_t ctx)`

作用：

- 销毁 `context`

通常在：

- `easyjit_compile` 成功之后

就可以销毁，因为编译结果已经放到 `easyjit_function_t` 里了。

### `easyjit_context_set_forward(easyjit_context_t ctx, unsigned index)`

作用：

- 把“下一个参数”标记为运行时参数

这里的 `index` 表示：

- 这个参数在最终专门化函数里的 runtime 参数序号

例如：

```c
easyjit_context_set_forward(ctx, 0);
easyjit_context_set_forward(ctx, 1);
```

表示最终函数里会保留两个运行时参数。

### `easyjit_context_set_int(easyjit_context_t ctx, int64_t value)`

作用：

- 把下一个参数整体固定为整数常量

例子：

```c
easyjit_context_set_int(ctx, 4);
```

### `easyjit_context_set_float(easyjit_context_t ctx, double value)`

作用：

- 把下一个参数固定为浮点常量

### `easyjit_context_set_pointer(easyjit_context_t ctx, const void *ptr)`

作用：

- 把下一个参数固定成一个常量指针值

注意：

- 它只固定“指针值本身”
- 不会自动把这块指针指向的对象内容一起 snapshot

如果你真正想固定的是指针后面的内容，通常应该考虑：

- `set_snapshot`
- `set_array`
- `bind_array`

### `easyjit_context_set_struct(easyjit_context_t ctx, const void *data, size_t size)`

作用：

- 把下一个参数按原始字节复制为一个常量 struct

### `easyjit_context_set_snapshot(easyjit_context_t ctx, const void *data, size_t size)`

作用：

- 和 `set_struct` 一样，都是“整个结构体常量化”

参数：

- `data`
  - 结构体地址
- `size`
  - 一般是 `sizeof(StructType)`

例子：

```c
easyjit_context_set_snapshot(ctx, &cfg, sizeof(cfg));
```

### `easyjit_context_set_partial_struct(easyjit_context_t ctx, unsigned index)`

作用：

- 声明“下一个参数是一个 struct 指针，我要对它做部分字段常量化”

参数：

- `index`
  - 这个 struct 参数在最终专门化函数里的 runtime 参数序号

注意：

- `set_partial_struct` 只是“声明这个参数走 partial struct 逻辑”
- 它本身不会自动固定任何字段
- 真正固定哪些字段，要靠后面的 `bind_field()` 和 `bind_array()`

### `easyjit_context_bind_field(easyjit_context_t ctx, size_t field_offset, const void *data, size_t size)`

作用：

- 给最近一次 `set_partial_struct()` 的那个 struct 绑定一个字段常量

参数：

- `field_offset`
  - 字段偏移
  - 推荐写法：`offsetof(StructType, member)`
- `data`
  - 指向这个字段的值
- `size`
  - 字段大小

例子：

```c
easyjit_context_bind_field(ctx,
                           offsetof(Config, enabled),
                           &cfg.enabled,
                           sizeof(cfg.enabled));
```

### `easyjit_context_bind_array(easyjit_context_t ctx, size_t field_offset, const void *data, size_t count, size_t element_size)`

作用：

- 给最近一次 `set_partial_struct()` 的 struct 中某个“指针字段”绑定一段常量数组

参数：

- `field_offset`
  - 指针字段偏移
- `data`
  - 数组首地址
- `count`
  - 元素个数
- `element_size`
  - 单个元素大小

例子：

```c
easyjit_context_bind_array(ctx,
                           offsetof(Config, weights),
                           cfg.weights,
                           cfg.count,
                           sizeof(cfg.weights[0]));
```

### `easyjit_context_set_array(easyjit_context_t ctx, const void *data, size_t count, size_t element_size)`

作用：

- 把“下一个参数本身”当成一个 flat array 做整体 snapshot

和 `bind_array` 的区别：

- `set_array`
  - 处理“这个参数本身就是数组”
- `bind_array`
  - 处理“struct 里的某个字段是数组指针”

### `easyjit_context_set_opt_level(easyjit_context_t ctx, unsigned opt_level, unsigned opt_size)`

作用：

- 设置 JIT 优化等级

例子：

```c
easyjit_context_set_opt_level(ctx, 3, 0);
```

### `easyjit_context_set_dump_ir(easyjit_context_t ctx, const char *file)`

作用：

- 打开 IR dump，方便你看 JIT 后到底发生了什么优化

通常会生成：

- `<file>.before.ll`
- `<file>`
- `<file>.after.ll`

### `easyjit_compile(void *func_ptr, easyjit_context_t ctx, easyjit_function_t *out_fn)`

作用：

- 按 `ctx` 里的绑定方式，对目标函数做 specialization + JIT 编译

参数：

- `func_ptr`
  - 目标函数地址
  - 一般写 `(void *)target_function`
- `ctx`
  - 前面构建好的 context
- `out_fn`
  - 输出编译结果句柄

### `easyjit_get_function_pointer(easyjit_function_t fn, void **out_ptr)`

作用：

- 从编译结果里取出原始函数地址

后面通常要自己转成正确函数类型：

```c
my_jit_fn_t f = (my_jit_fn_t)raw;
```

### `easyjit_function_destroy(easyjit_function_t fn)`

作用：

- 销毁 JIT 编译结果句柄

---

## 7. 最容易搞错的点：最终函数签名怎么判断

经验法则很简单：

- `set_int / set_float / set_snapshot / set_array`
  - 这些固定掉的参数，会从最终函数签名里消失
- `set_forward`
  - 这些参数会保留
- `set_partial_struct`
  - 这个 struct 参数也会保留，只是其中部分字段被固定

### 例子 1

原函数：

```c
int foo(int x, int y, int z);
```

绑定方式：

```c
easyjit_context_set_forward(ctx, 0); /* x */
easyjit_context_set_int(ctx, 2);     /* y */
easyjit_context_set_int(ctx, 3);     /* z */
```

最终函数签名：

```c
int (*spec)(int);
```

### 例子 2

原函数：

```c
int foo(const Config *cfg, int x, int y);
```

绑定方式：

```c
easyjit_context_set_partial_struct(ctx, 0); /* cfg 保留 */
easyjit_context_bind_field(...);
easyjit_context_set_forward(ctx, 1);        /* x 保留 */
easyjit_context_set_int(ctx, 5);            /* y 固定 */
```

最终函数签名：

```c
int (*spec)(const Config *, int);
```

这里不是三参数，也不是单参数，而是“剩下的运行时参数”那一版。

---

## 8. 怎么把原来的普通 C 用例改成 JIT 版本

这里给一个最常见的改写思路。

### 原始普通调用

原代码：

```c
int result = process_config(&cfg, group_index, x);
```

### 假设你决定这样专门化

- `cfg->enabled` 固定
- `cfg->priority` 固定
- `group_index` 固定
- `x` 保留为运行时参数

### 改写后的 context 构建

```c
easyjit_context_t ctx = NULL;
easyjit_function_t fn = NULL;
void *raw = NULL;

easyjit_context_create(&ctx);

easyjit_context_set_partial_struct(ctx, 0); /* cfg */
easyjit_context_bind_field(ctx,
                           offsetof(ConfigRecord, enabled),
                           &cfg.enabled,
                           sizeof(cfg.enabled));
easyjit_context_bind_field(ctx,
                           offsetof(ConfigRecord, priority),
                           &cfg.priority,
                           sizeof(cfg.priority));

easyjit_context_set_int(ctx, group_index);  /* group_index */
easyjit_context_set_forward(ctx, 1);        /* x */

easyjit_context_set_opt_level(ctx, 3, 0);

easyjit_compile((void *)process_config, ctx, &fn);
easyjit_context_destroy(ctx);

easyjit_get_function_pointer(fn, &raw);
```

### 然后写对最终函数指针类型

因为：

- `cfg` 保留
- `group_index` 被固定
- `x` 保留

所以最终函数签名是：

```c
typedef int (*jit_fn_t)(const ConfigRecord *, int);
```

然后：

```c
jit_fn_t f = (jit_fn_t)raw;
int result = f(&runtime_cfg, x);
```

---

## 9. 什么时候该用哪个接口

如果你只是想快速判断，这一节最实用。

### 情况 1：这个参数是普通整数，想固定掉

用：

- `easyjit_context_set_int`

### 情况 2：这个参数是运行时变量，要保留

用：

- `easyjit_context_set_forward`

### 情况 3：这个参数是 struct，而且整个 struct 都稳定

用：

- `easyjit_context_set_snapshot`

### 情况 4：这个参数是 struct，但只有部分字段稳定

用：

- `easyjit_context_set_partial_struct`
- `easyjit_context_bind_field`
- 必要时 `easyjit_context_bind_array`

### 情况 5：这个参数本身就是数组指针，想整块固定

用：

- `easyjit_context_set_array`

### 情况 6：struct 里有个数组指针字段，想把它指向的数据固定

用：

- `easyjit_context_bind_array`

---

## 10. 常见坑

### 坑 1：函数指针签名写成原函数签名

这是最常见的问题。

一定要按“最终还保留下来的运行时参数”来写函数指针类型。

### 坑 2：`bind_field()` 的偏移自己手算

不要手算。

统一用：

```c
offsetof(StructType, field)
```

### 坑 3：把指针字段当普通 field 去绑

如果你想固定的是“指针后面那段数组内容”，要优先考虑：

```c
easyjit_context_bind_array(...)
```

不是只把指针值本身 `bind_field` 掉。

### 坑 4：`set_partial_struct()` 之后没继续绑字段

`set_partial_struct()` 只是告诉 EasyJIT：

- “这个参数要走 partial struct 路径”

但如果后面不跟 `bind_field()` 或 `bind_array()`，那实际上没有固定任何成员。

### 坑 5：参数顺序乱了

一定按原函数参数顺序来消费 `context`。

---

## 11. 一个可以直接照抄的 partial struct 模板

```c
easyjit_context_t ctx = NULL;
easyjit_function_t fn = NULL;
void *raw = NULL;

if (easyjit_context_create(&ctx) != EASYJIT_OK) {
    fprintf(stderr, "context_create failed: %s\n", easyjit_get_last_error());
    return 1;
}

if (easyjit_context_set_partial_struct(ctx, 0) != EASYJIT_OK) {
    fprintf(stderr, "set_partial_struct failed: %s\n", easyjit_get_last_error());
    return 1;
}

if (easyjit_context_bind_field(ctx,
                               offsetof(Config, enabled),
                               &cfg.enabled,
                               sizeof(cfg.enabled)) != EASYJIT_OK) {
    fprintf(stderr, "bind_field failed: %s\n", easyjit_get_last_error());
    return 1;
}

if (easyjit_context_set_forward(ctx, 1) != EASYJIT_OK) {
    fprintf(stderr, "set_forward failed: %s\n", easyjit_get_last_error());
    return 1;
}

if (easyjit_compile((void *)target_fn, ctx, &fn) != EASYJIT_OK) {
    fprintf(stderr, "compile failed: %s\n", easyjit_get_last_error());
    return 1;
}

easyjit_context_destroy(ctx);
ctx = NULL;

if (easyjit_get_function_pointer(fn, &raw) != EASYJIT_OK) {
    fprintf(stderr, "get_function_pointer failed: %s\n", easyjit_get_last_error());
    return 1;
}

typedef int (*jit_fn_t)(const Config *, int);
jit_fn_t f = (jit_fn_t)raw;

int result = f(&runtime_cfg, x);

easyjit_function_destroy(fn);
```

---

## 12. 怎么确认它真的生效了

最稳的办法是：

1. 打开 IR dump
2. 看 `.after.ll`

如果部分常量化生效了，通常会看到：

- 某些字段读取被替换成常量
- 某些分支被直接消掉
- 更好的情况下，整个函数会被压得更小

所以如果你怀疑：

- “我是不是绑错了字段”
- “为什么看起来性能没变化”

优先看 IR 往往最快。

---

## 13. 最后一句总结

这套 C API 可以这样记：

- `set_forward`
  - 参数继续运行时传
- `set_int / set_float / set_pointer`
  - 参数整体固定
- `set_snapshot`
  - 整个 struct 固定
- `set_partial_struct`
  - struct 继续运行时传
- `bind_field`
  - 固定 struct 里的某个普通字段
- `bind_array`
  - 固定 struct 里某个数组指针字段指向的数据

如果你只是想把原来整块 snapshot 的用例改成“部分字段常量化”，最短做法就是：

> 把 `set_snapshot()` 换成 `set_partial_struct()`，  
> 再用 `bind_field()` / `bind_array()` 把真正稳定的成员补进去，  
> 其他参数保持原来的 `set_forward()` / `set_int()` 逻辑不变。
