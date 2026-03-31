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

/** Specialize the next parameter to a concrete pointer value. */
easyjit_error_t easyjit_context_set_pointer(easyjit_context_t ctx,
                                             const void* ptr);

/** Specialize the next parameter with a raw struct blob (memcpy semantics).
 *  `data` points to the struct bytes, `size` is sizeof(TheStruct). */
easyjit_error_t easyjit_context_set_struct(easyjit_context_t ctx,
                                            const void* data,
                                            size_t size);

/* --- Optimization level ------------------------------------------------ */

/** Set the optimization level (opt_level 0-3, opt_size 0-2).
 *  Default is O2 size=0. */
easyjit_error_t easyjit_context_set_opt_level(easyjit_context_t ctx,
                                               unsigned opt_level,
                                               unsigned opt_size);

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
