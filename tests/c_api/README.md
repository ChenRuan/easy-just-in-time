# EasyJIT C Interface — Overview

## Recommended file set

The `tests/c_api/` directory is intentionally kept focused. The recommended
examples to keep and read are:

| File | Purpose |
|------|---------|
| `include/easy/easyjit_c.h` | Public C API header |
| `tests/c_api/EASYJIT_CAPI_ZH.md` | Chinese step-by-step tutorial |
| `tests/c_api/add_int.c` | Smallest end-to-end example |
| `tests/c_api/array_snapshot.c` | Flat array specialization |
| `tests/c_api/struct_snapshot.c` | Whole-struct snapshot specialization |
| `tests/c_api/pointer_field_snapshot.c` | Snapshot + bind pointed-to array field |
| `tests/c_api/partial_struct_binding.c` | Partial struct field specialization |
| `tests/c_api/mixed_bindings.c` | Mixed runtime args + scalar constants + snapshots |
| `tests/c_api/cache_example.c` | Cache / reuse flow |
| `tests/c_api/wireless_beamform.c` | More realistic domain-style kernel |
| `tests/c_api/config_process_base.c` | Baseline benchmark |
| `tests/c_api/config_process_easyjit.c` | JIT benchmark companion |
| `tests/c_api/embedded_diag_easyjit.c` | Embedded board diagnostic harness |
| `tests/c_api/run_c_api_tests.sh` | One-command build & run script |

Several one-off benchmark probes and performance investigation variants were
intentionally removed to keep this directory readable.

## Core design

### API surface (15 functions)

```
Context lifecycle:
  easyjit_context_create()        → create empty context
  easyjit_context_destroy()       → free context

Parameter binding (positional, in order):
  easyjit_context_set_forward()   → keep as runtime arg
  easyjit_context_set_int()       → specialize to int constant
  easyjit_context_set_float()     → specialize to float constant
  easyjit_context_set_pointer()   → specialize to pointer constant
  easyjit_context_set_struct()    → specialize struct (raw memcpy)
  easyjit_context_set_partial_struct() → keep struct ptr runtime-visible, bind selected members
  easyjit_context_bind_field()    → bind one scalar/pointer leaf field on latest partial struct
  easyjit_context_set_opt_level() → set O-level

Compilation:
  easyjit_compile()               → JIT compile, returns handle
  easyjit_get_function_pointer()  → extract raw function pointer
  easyjit_function_destroy()      → free compiled code

Cache (optional convenience):
  easyjit_cache_create()
  easyjit_cache_destroy()
  easyjit_cache_get_or_compile()  → lookup-or-compile in one call
  easyjit_cache_has()

Error handling:
  easyjit_get_last_error()        → thread-local error string
```

### Design principles
1. **Opaque handles** — all internal state hidden behind `easyjit_context_t`, `easyjit_function_t`, `easyjit_cache_t`
2. **Thin wrapper** — every function delegates to existing `easy::Context`, `easy::Function`, `easy::BitcodeTracker`; no logic duplication
3. **Error codes + thread-local message** — `easyjit_error_t` enum plus `easyjit_get_last_error()` for details
4. **No C++ headers exposed** — the C header uses only `<stddef.h>` and `<stdint.h>`

### Key technical challenge solved: Layout registration

The EasyJIT runtime uses a "layout" system to map high-level parameters to LLVM IR arguments (a struct might decompose into multiple fields). In the C++ API, this is handled automatically by templates (`param.h` → `layout::set_layout<T>()`).

For the C API, we register a sentinel layout_id with `NumFields=1` in the `BitcodeTracker` and attach it to every scalar parameter. This works for all simple C types (int, float, pointer) and for structs passed as a single LLVM-IR argument.

## Build & usage flow

```bash
# 1. Build easyjit (the library now includes C API symbols)
cd build-easyjit-minimal && ninja EasyJitRuntime

# 2. Compile C code with EasyJIT pass (embeds bitcode)
clang -g -Xclang -disable-O0-optnone \
      -I<easyjit>/include \
      -Xclang -fpass-plugin=EasyJitPass.so \
      -L<easyjit>/lib -Wl,-rpath,<easyjit>/lib \
      -lEasyJitRuntime \
      my_code.c -o my_code

# 3. Functions marked with EASY_JIT_EXPOSE can be specialized at runtime
```

The user-side C code needs only:
- `#include <easy/easyjit_c.h>` for the C API
- `#include <easy/attributes.h>` for `EASY_JIT_EXPOSE`

### Mixed binding example

`mixed_bindings.c` demonstrates how to bind parameters in signature order when a
kernel takes multiple runtime values plus multiple structs:

```c
easyjit_context_set_forward(ctx, 0);          // input
easyjit_context_set_float(ctx, 2.0);          // scale
easyjit_context_set_snapshot(ctx, &a, sizeof(a));
easyjit_context_set_snapshot(ctx, &b, sizeof(b));
easyjit_context_set_int(ctx, 4);              // index
easyjit_context_set_forward(ctx, 1);          // offset
```

The specialized function only keeps the forwarded arguments, so the original
signature:

```c
process(const float* input, float scale, const ConfigA* a,
        const ConfigB* b, int index, float offset)
```

becomes:

```c
int (*process_spec_t)(const float*, float);
```

### Flat array snapshot

For a plain pointer parameter, the C API now exposes the same idea as
`easy::snapshot_array(...)`:

```c
easyjit_context_set_forward(ctx, 0);                 // x
easyjit_context_set_array(ctx, data, 4, sizeof(int)); // values
```

This turns a function like:

```c
int eval_array(int x, const int* data);
```

into:

```c
int (*eval_array_spec_t)(int);
```

### Pointer-field array binding

For a struct like `struct Config { const int *array; ... };`, first snapshot the
struct itself, then bind the pointed-to array onto the most recent snapshot:

```c
easyjit_context_set_snapshot(ctx, &cfg, sizeof(cfg));
easyjit_context_bind_array(ctx,
                           offsetof(Config, array),
                           cfg.array,
                           4,
                           sizeof(int));
```

This is the C equivalent of:

```cpp
easy::snapshot(cfg, easy::bind_array(&Config::array, 4))
```

### Partial struct specialization

If only some members are invariant, keep the struct pointer as a runtime
argument and bind the invariant fields explicitly:

```c
easyjit_context_set_partial_struct(ctx, 0);  // cfg stays in specialized signature
easyjit_context_bind_field(ctx,
                           offsetof(Config, enabled),
                           &cfg.enabled,
                           sizeof(cfg.enabled));
easyjit_context_set_forward(ctx, 1);         // x
```

This lets EasyJIT treat `cfg->enabled` as a constant while other members of
`cfg` still come from the runtime pointer.

## Current status

| Feature | Status |
|---------|--------|
| Int parameter specialization | ✅ Working |
| Float parameter specialization | ✅ Working (API ready, not separately tested) |
| Pointer forwarding | ✅ Working |
| Array snapshot | ✅ Working |
| Partial struct field specialization | ✅ Working |
| Loop-bound specialization | ✅ Working (beamform example) |
| Cache / reuse | ✅ Working |
| Struct specialization | ⚠️ API ready, works for single-field layout; multi-field structs may need per-case layout registration |
| Multi-file linking | ❌ Not tested (kernel in separate .c from main) |

## Remaining limitations and next steps

1. **Struct layout**: The current C API assumes all parameters map to 1 LLVM-IR argument. Structs that are decomposed into multiple fields by the ABI (e.g., a 3-field struct passed in 3 registers) would need a `easyjit_context_set_struct_with_nfields(ctx, data, size, nfields)` variant or automatic detection from the bitcode.

2. **Multi-file**: If the kernel lives in a separate `.c` file from `main()`, both must be compiled with the EasyJIT pass. This works in principle but hasn't been tested with the C API.

3. **Nested pointees beyond 1-D arrays**: `easyjit_context_set_array(...)` and `easyjit_context_bind_array(...)` cover flat read-only arrays, but multi-level pointers (`T**`) and recursive pointee graphs are still not supported.

4. **Thread safety**: Individual handles are not thread-safe. The `ensure_scalar_layout_registered()` uses a simple `static bool` which is safe on the first call but could be made more robust with `std::call_once`.

5. **Symbol export control**: Currently all C API symbols are exported simply because they have `extern "C"` linkage and the shared library doesn't use a version script. For production, a `.map` file or `__attribute__((visibility("default")))` should be used.

6. **wireless/ integration**: Once the actual `wireless/` directory exists with real examples, they should be adaptable using the pattern demonstrated in `wireless_beamform.c` — mark kernels with `EASY_JIT_EXPOSE`, create contexts, bind parameters, and compile.

## Benchmark note

`config_process_base.c` and `config_process_easyjit.c` are larger performance examples.
They are intentionally skipped by `run_c_api_tests.sh` unless you set:

```bash
INCLUDE_BENCHMARKS=1 ./run_c_api_tests.sh <LLVM_BUILD_DIR> <EASYJIT_BUILD_DIR>
```
