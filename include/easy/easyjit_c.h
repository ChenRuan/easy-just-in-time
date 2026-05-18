/**
 * @file easyjit_c.h
 * @brief Minimal C interface for EasyJIT runtime specialization.
 *
 * This header provides an opaque-handle based C API that wraps the core
 * EasyJIT C++ functionality.  It is designed for use from pure-C code
 * (such as the wireless/ examples) that needs to:
 *
 *   1. Build a specialization context (bind concrete argument values).
 *   2. JIT-compile a registered function with that context.
 *   3. Obtain a raw function pointer to the specialized code.
 *   4. Release resources when no longer needed.
 *
 * Thread-safety: individual handles are NOT thread-safe; do not share a
 * single context or compiled-function handle across threads without
 * external synchronization.  Creating independent handles in parallel
 * threads is fine.
 *
 * All functions return an easyjit_error_t status code.  On error the
 * handle is left in a valid-but-unspecified state; the caller should
 * still call the matching destroy function.
 */

#ifndef EASYJIT_C_H
#define EASYJIT_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Error codes                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    EASYJIT_OK = 0,
    EASYJIT_ERROR_INVALID_ARGUMENT = 1,
    EASYJIT_ERROR_NOT_REGISTERED   = 2,   /* function bitcode not found */
    EASYJIT_ERROR_COMPILE_FAILED   = 3,
    EASYJIT_ERROR_INTERNAL         = 4,
} easyjit_error_t;

/**
 * Return a human-readable description of the last error that occurred
 * on the calling thread.  Returns "" when no error has been recorded.
 */
const char* easyjit_get_last_error(void);

/**
 * Explicitly run EasyJIT bitcode registration for the current linked image.
 *
 * Standard ELF loaders normally execute EasyJIT's generated .init_array
 * constructor automatically.  Custom embedded loaders that only resolve and
 * call selected symbols may skip .init_array; call this once after loading the
 * image and before calling easyjit_compile() or any business entry that uses
 * EasyJIT.  The operation is idempotent.
 */
void easyjit_register_module(void);

/** Internal helper used by compiler-generated module registration stubs. */
void easyjit_register_module_range(void* start, void* stop);

/* ------------------------------------------------------------------ */
/*  Context handle — describes which arguments are specialized         */
/* ------------------------------------------------------------------ */

/** Opaque handle to a JIT compilation context. */
typedef struct easyjit_context_s* easyjit_context_t;

/** Create a new, empty context.  Must be destroyed with easyjit_context_destroy. */
easyjit_error_t easyjit_context_create(easyjit_context_t* out_ctx);

/** Destroy a context and free associated memory. */
void easyjit_context_destroy(easyjit_context_t ctx);

/* --- Parameter binding ------------------------------------------------
 *
 * Parameters are added in **positional order** matching the target
 * function's signature.  Each call appends one parameter slot.
 *
 * "forward" means "leave this parameter as a runtime argument of the
 * specialized function" (i.e. a placeholder / _1, _2, …).
 * The `index` is the 0-based position in the *resulting* function's
 * parameter list.
 */

/** Mark the next parameter as forwarded (kept as runtime arg).
 *  `index` is the 0-based slot in the resulting specialized signature. */
easyjit_error_t easyjit_context_set_forward(easyjit_context_t ctx,
                                             unsigned index);

/** Specialize the next parameter to a concrete integer value. */
easyjit_error_t easyjit_context_set_int(easyjit_context_t ctx,
                                         int64_t value);

/** Specialize the next parameter to a concrete floating-point value. */
easyjit_error_t easyjit_context_set_float(easyjit_context_t ctx,
                                           double value);

/** Specialize the next parameter to a concrete pointer value.
 *  The pointer value itself is baked into the JIT'd code as a constant,
 *  but the memory it points to is NOT captured — dereferences still
 *  happen at runtime.  Use set_struct / set_snapshot for struct-pointer
 *  parameters where you want the pointed-to data to be constant-folded. */
easyjit_error_t easyjit_context_set_pointer(easyjit_context_t ctx,
                                             const void* ptr);

/** Specialize the next parameter with a raw struct blob (memcpy semantics).
 *  `data` points to the struct bytes, `size` is sizeof(TheStruct).
 *
 *  Equivalent to easy::snapshot() in the C++ API: the struct contents
 *  are copied into the JIT context and become compile-time constants.
 *  All field accesses in the JIT'd function are constant-folded away.
 *
 *  The target function parameter should be a struct pointer (T*)
 *  or const reference (T const&).
 */
easyjit_error_t easyjit_context_set_struct(easyjit_context_t ctx,
                                            const void* data,
                                            size_t size);

/**
 * Snapshot a struct for specialization (convenience alias for set_struct).
 *
 * This is the C equivalent of easy::snapshot(cfg) in the C++ API.
 * The struct bytes at `data` are copied into the context and become
 * compile-time constants.  The JIT pass will create a constant
 * allocation initialized with these bytes, so all field accesses
 * (cfg->field) are folded to constants and branches are eliminated.
 *
 * Example:
 *   PdcchTrpConfig cfg = { ... };
 *   easyjit_context_set_snapshot(ctx, &cfg, sizeof(cfg));
 */
easyjit_error_t easyjit_context_set_snapshot(easyjit_context_t ctx,
                                              const void* data,
                                              size_t size);

/**
 * Snapshot a global object for specialization without adding it to the
 * specialized function signature.
 *
 * `global_addr` identifies the referenced global variable in the compiled
 * module. `data` / `size` provide the bytes that should be treated as the
 * compile-time value of that global during JIT specialization.
 *
 * This is the C API counterpart of the C++ helper:
 *   easy::options::global_snapshot(g_cfg)
 *
 * The target function must already reference this global directly; this call
 * only marks which global object should be materialized as a constant during
 * specialization.
 */
easyjit_error_t easyjit_context_set_global_snapshot(easyjit_context_t ctx,
                                                     const void* global_addr,
                                                     const void* data,
                                                     size_t size);

/**
 * Mark a directly referenced global struct for partial specialization.
 *
 * Unlike easyjit_context_set_global_snapshot(), this does not freeze the
 * entire global object. Instead, subsequent easyjit_context_bind_global_field()
 * / easyjit_context_bind_global_array() calls select which members should be
 * treated as compile-time constants, while all other fields continue to read
 * the live global variable at runtime.
 */
easyjit_error_t easyjit_context_set_global_partial_struct(easyjit_context_t ctx,
                                                           const void* global_addr);

/**
 * Bind one scalar/pointer leaf field inside the most recent global partial
 * struct to a compile-time constant byte representation.
 */
easyjit_error_t easyjit_context_bind_global_field(easyjit_context_t ctx,
                                                   size_t field_offset,
                                                   const void* data,
                                                   size_t size);

/**
 * Bind a constant array to a pointer field inside the most recent global
 * partial struct.
 */
easyjit_error_t easyjit_context_bind_global_array(easyjit_context_t ctx,
                                                   size_t field_offset,
                                                   const void* data,
                                                   size_t count,
                                                   size_t element_size);

/**
 * Forward the next parameter as a runtime struct pointer while allowing
 * selected fields to be bound as compile-time constants.
 *
 * This is intended for function parameters of type `T*` / `const T*`
 * where only some members should be specialized. The parameter remains in the
 * specialized function signature at runtime index `index`, unlike
 * easyjit_context_set_snapshot() which removes the parameter entirely.
 *
 * After calling this, append one or more easyjit_context_bind_field() and/or
 * easyjit_context_bind_array() calls to describe which members are constant.
 */
easyjit_error_t easyjit_context_set_partial_struct(easyjit_context_t ctx,
                                                    unsigned index);

/**
 * Bind one scalar/pointer leaf field inside the most recent partial-struct
 * parameter to a compile-time constant byte representation.
 *
 * `field_offset` should usually be produced with offsetof(T, member).
 * `data` points to the field value to bake in; `size` is usually sizeof(field).
 *
 * Example:
 *   easyjit_context_set_partial_struct(ctx, 0);
 *   easyjit_context_bind_field(ctx, offsetof(Config, enabled),
 *                              &cfg.enabled, sizeof(cfg.enabled));
 */
easyjit_error_t easyjit_context_bind_field(easyjit_context_t ctx,
                                            size_t field_offset,
                                            const void* data,
                                            size_t size);

/**
 * Bind an array snapshot to a pointer field inside the most recent snapshot.
 *
 * This extends the latest easyjit_context_set_snapshot()/set_struct()/
 * set_partial_struct() call by replacing the pointer field at byte offset
 * `field_offset` with a private constant array built from `data[0..count)`.
 *
 * The usual pattern is:
 *   easyjit_context_set_snapshot(ctx, &cfg, sizeof(cfg));
 *   easyjit_context_bind_array(ctx, offsetof(Config, array), cfg.array, n, sizeof(int));
 *
 * This is the C equivalent of:
 *   easy::snapshot(cfg, easy::bind_array(&Config::array, n))
 *
 * The binding applies to the most recently appended snapshot/partial-struct
 * parameter and is intended for read-only pointee data.
 */
easyjit_error_t easyjit_context_bind_array(easyjit_context_t ctx,
                                            size_t field_offset,
                                            const void* data,
                                            size_t count,
                                            size_t element_size);

/**
 * Snapshot a flat array for specialization.
 *
 * This appends one pointer parameter whose pointee contents are copied into the
 * context as a private constant array.  It is the C equivalent of:
 *   easy::snapshot_array(data, count)
 *
 * Example:
 *   easyjit_context_set_forward(ctx, 0);                 // x
 *   easyjit_context_set_array(ctx, data, 4, sizeof(int)); // values
 */
easyjit_error_t easyjit_context_set_array(easyjit_context_t ctx,
                                           const void* data,
                                           size_t count,
                                           size_t element_size);

/* --- Optimization level ------------------------------------------------ */

/** Set the optimization level (opt_level 0-3, opt_size 0-2).
 *  Default is O2 size=0. */
easyjit_error_t easyjit_context_set_opt_level(easyjit_context_t ctx,
                                               unsigned opt_level,
                                               unsigned opt_size);

/**
 * Enable IR dumping for a C API compilation context.
 *
 * When set, compilation writes three files:
 *   <file>.before.ll  - IR before EasyJIT optimization/specialization
 *   <file>            - final optimized IR
 *   <file>.after.ll   - IR after optimization
 *
 * Pass NULL or "" to disable dumping.
 */
easyjit_error_t easyjit_context_set_dump_ir(easyjit_context_t ctx,
                                             const char* file);

/* ------------------------------------------------------------------ */
/*  Compiled function handle                                           */
/* ------------------------------------------------------------------ */

/** Opaque handle to a JIT-compiled function. */
typedef struct easyjit_function_s* easyjit_function_t;

/**
 * JIT-compile `func_ptr` (which must have been compiled with the EasyJIT
 * clang pass so its bitcode is registered) using the specialization
 * described by `ctx`.
 *
 * On success, `*out_fn` receives a handle that owns the compiled code.
 * The context may be destroyed immediately after this call.
 */
easyjit_error_t easyjit_compile(void* func_ptr,
                                 easyjit_context_t ctx,
                                 easyjit_function_t* out_fn);

/**
 * Obtain the raw function pointer for the specialized code.
 * The pointer remains valid as long as the easyjit_function_t handle is
 * alive.
 *
 * The caller must cast this to the correct C function-pointer type
 * matching the specialized signature.
 */
easyjit_error_t easyjit_get_function_pointer(easyjit_function_t fn,
                                              void** out_ptr);

/** Destroy a compiled function handle and release JIT resources. */
void easyjit_function_destroy(easyjit_function_t fn);

/* ------------------------------------------------------------------ */
/*  Simple cache (optional convenience)                                */
/* ------------------------------------------------------------------ */

/** Opaque handle to a simple integer-keyed function cache. */
typedef struct easyjit_cache_s* easyjit_cache_t;

/** Create a new cache.  Must be destroyed with easyjit_cache_destroy. */
easyjit_error_t easyjit_cache_create(easyjit_cache_t* out_cache);

/** Destroy a cache and all compiled functions it owns. */
void easyjit_cache_destroy(easyjit_cache_t cache);

/** Clear all compiled functions from a cache while keeping the cache reusable. */
easyjit_error_t easyjit_cache_clear(easyjit_cache_t cache);

/**
 * Look up or compile a specialized function in the cache.
 *
 * If `key` is already in the cache, `*out_ptr` receives the existing
 * function pointer and no compilation occurs.
 *
 * If `key` is NOT in the cache, `func_ptr` is JIT-compiled with `ctx`,
 * the result is stored under `key`, and `*out_ptr` receives the new
 * function pointer.
 *
 * `ctx` may be NULL when you know the key is already cached.
 */
easyjit_error_t easyjit_cache_get_or_compile(easyjit_cache_t cache,
                                              int64_t key,
                                              void* func_ptr,
                                              easyjit_context_t ctx,
                                              void** out_ptr);

/**
 * Check whether `key` exists in the cache.
 * Returns EASYJIT_OK and sets *out_hit to 1 if found, 0 otherwise.
 */
easyjit_error_t easyjit_cache_has(easyjit_cache_t cache,
                                   int64_t key,
                                   int* out_hit);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* EASYJIT_C_H */
