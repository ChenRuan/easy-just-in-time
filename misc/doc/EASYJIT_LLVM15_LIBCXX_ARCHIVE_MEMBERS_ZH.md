# EasyJIT + LLVM 15 libc++ 静态库对象清单

本文记录一次在本机验证 EasyJIT `llvm15` 分支与 LLVM 15.0.4 使用 libc++ ABI 构建时，链接 `EasyJitRuntime` 会从 libc++ 静态库中拉入哪些 archive member。

## 验证环境

- EasyJIT: `/home/ruanchen/workspace/llvm-project-15.0.4/easy-jit-main`
- LLVM 源码: `/home/ruanchen/workspace/llvm-project-15.0.4`
- libc++ 安装前缀: `/tmp/llvm15-libcxx-install-clang`
- EasyJIT 构建目录: `/tmp/easyjit-llvm15-libcxx`
- linker map: `/tmp/easyjit-libcxx.map`
- member 清单: `/tmp/easyjit-libcxx-archive-members.tsv`

这次 libc++ runtime 构建使用了 `LIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON`，所以 libc++abi 的对象会被合进同一个 `libc++.a`。如果目标工具链的 `libc++.a` 和 `libc++abi.a` 是分开的，需要按下面的分类拆开看。

本机这次最终 `.so` 没有完整链接成功，原因是临时构建出来的 LLVM/libc++ 静态库不是 PIC；但 linker map 已生成，足够判断静态 archive member 选择情况。

## 全量拉入对象

这次从 `libc++.a` 中拉入 33 个对象：

```text
abort_message.cpp.o
algorithm.cpp.o
chrono.cpp.o
condition_variable.cpp.o
condition_variable_destructor.cpp.o
cxa_default_handlers.cpp.o
cxa_demangle.cpp.o
cxa_exception.cpp.o
cxa_exception_storage.cpp.o
cxa_guard.cpp.o
cxa_handlers.cpp.o
cxa_personality.cpp.o
cxa_thread_atexit.cpp.o
cxa_virtual.cpp.o
exception.cpp.o
fallback_malloc.cpp.o
future.cpp.o
hash.cpp.o
ios.cpp.o
ios.instantiations.cpp.o
locale.cpp.o
memory.cpp.o
mutex.cpp.o
new.cpp.o
private_typeinfo.cpp.o
shared_mutex.cpp.o
stdexcept.cpp.o
stdlib_exception.cpp.o
stdlib_stdexcept.cpp.o
stdlib_typeinfo.cpp.o
string.cpp.o
system_error.cpp.o
thread.cpp.o
```

## 按库归类

纯 libc++ 侧大概率需要：

```text
algorithm.cpp.o
chrono.cpp.o
condition_variable.cpp.o
condition_variable_destructor.cpp.o
exception.cpp.o
future.cpp.o
hash.cpp.o
ios.cpp.o
ios.instantiations.cpp.o
locale.cpp.o
memory.cpp.o
mutex.cpp.o
new.cpp.o
shared_mutex.cpp.o
stdexcept.cpp.o
string.cpp.o
system_error.cpp.o
thread.cpp.o
```

libc++abi 侧大概率需要：

```text
abort_message.cpp.o
cxa_default_handlers.cpp.o
cxa_demangle.cpp.o
cxa_exception.cpp.o
cxa_exception_storage.cpp.o
cxa_guard.cpp.o
cxa_handlers.cpp.o
cxa_personality.cpp.o
cxa_thread_atexit.cpp.o
cxa_virtual.cpp.o
fallback_malloc.cpp.o
private_typeinfo.cpp.o
stdlib_exception.cpp.o
stdlib_stdexcept.cpp.o
stdlib_typeinfo.cpp.o
```

## 与 new/delete 相关的结论

这次实际拉入了 `new.cpp.o`，没有拉入 `stdlib_new_delete.cpp.o`。

如果目标系统修改了 libc++/libc++abi 中的 `operator new/delete`，需要重点确认目标工具链里这些符号最终落在哪个 archive member。不同 libc++ 构建选项、版本、ABI 合并方式可能让对象名或归属库不同。

## libunwind 注意点

这次 linker map 中没有选中 `libunwind.a` 的成员。这不等于目标环境一定不需要 unwind，只表示这轮链接在非 PIC 报错前没有拉入它。

如果目标环境需要完整静态异常展开，目标服务器上建议额外确认 `libunwind.a` 是否拉入类似下面的对象：

```text
UnwindLevel1.c.o
UnwindRegistersSave.S.o
UnwindRegistersRestore.S.o
libunwind.cpp.o
```

## 复跑与抽取清单

在目标服务器上拿到 linker map 后，可以用仓库里的脚本抽取 archive member：

```bash
misc/extract_archive_members_from_map.sh /path/to/linker.map /path/to/output-dir
```

如果使用 Ninja，可以把 `EasyJitRuntime` 的链接命令加上 map 参数后复跑：

```bash
cmd=$(ninja -C /path/to/easyjit-build -t commands EasyJitRuntime | tail -1)
cmd=${cmd/ -shared / -Wl,-Map,/tmp/easyjit-libcxx.map -Wl,--cref -shared }
eval "$cmd" >/tmp/easyjit-libcxx-link-with-map.log 2>&1 || true
misc/extract_archive_members_from_map.sh /tmp/easyjit-libcxx.map /tmp/easyjit-libcxx-members
```
