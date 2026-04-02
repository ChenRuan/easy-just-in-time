/**
 * @file easyjit_c.cpp
 * @brief Implementation of the minimal C interface for EasyJIT.
 *
 * This is a thin wrapper around the existing C++ easy::Context /
 * easy::Function machinery.  It does NOT duplicate logic — every call
 * delegates to the C++ implementation.
 */

#include <easy/easyjit_c.h>
#include <easy/runtime/Context.h>
#include <easy/runtime/Function.h>
#include <easy/runtime/BitcodeTracker.h>

#include <cstring>
#include <memory>
#include <unordered_map>
#include <string>

/* ------------------------------------------------------------------ */
/*  Layout helpers                                                     */
/*                                                                     */
/*  The EasyJIT HighLevelLayout system requires every parameter in     */
/*  the Context to have a corresponding layout_id registered in the    */
/*  BitcodeTracker.  In the C++ API this is done automatically by      */
/*  the template machinery (param.h).  For the C API we register       */
/*  a small set of "well-known" layout ids at library load time, one   */
/*  per number-of-fields (for scalar types NumFields=1).               */
/* ------------------------------------------------------------------ */

/* These are sentinel objects whose *addresses* serve as layout_ids.   */
static char g_layout_sentinel_1field;  /* NumFields = 1 (int/float/ptr) */

static void ensure_scalar_layout_registered() {
    static bool registered = false;
    if (!registered) {
        auto &BT = easy::BitcodeTracker::GetTracker();
        BT.registerLayout(&g_layout_sentinel_1field, 1);
        registered = true;
    }
}

/* Helper: set the standard 1-field layout for a scalar parameter. */
static void set_scalar_layout(easy::Context &ctx) {
    ensure_scalar_layout_registered();
    ctx.setArgumentLayout(&g_layout_sentinel_1field);
}

/* ------------------------------------------------------------------ */
/*  Thread-local error message                                         */
/* ------------------------------------------------------------------ */

static thread_local std::string g_last_error;

static void set_last_error(const char* msg) {
    g_last_error = msg ? msg : "";
}

static void clear_last_error() {
    g_last_error.clear();
}

extern "C" const char* easyjit_get_last_error(void) {
    return g_last_error.c_str();
}

/* ------------------------------------------------------------------ */
/*  Context handle internals                                           */
/* ------------------------------------------------------------------ */

struct easyjit_context_s {
    easy::Context ctx;
};

extern "C"
easyjit_error_t easyjit_context_create(easyjit_context_t* out_ctx) {
    clear_last_error();
    if (!out_ctx) {
        set_last_error("easyjit_context_create: out_ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    try {
        *out_ctx = new easyjit_context_s();
        return EASYJIT_OK;
    } catch (const std::exception& e) {
        set_last_error(e.what());
        return EASYJIT_ERROR_INTERNAL;
    }
}

extern "C"
void easyjit_context_destroy(easyjit_context_t ctx) {
    delete ctx;
}

/* --- Parameter binding -------------------------------------------- */

extern "C"
easyjit_error_t easyjit_context_set_forward(easyjit_context_t ctx,
                                             unsigned index) {
    clear_last_error();
    if (!ctx) {
        set_last_error("easyjit_context_set_forward: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    try {
        set_scalar_layout(ctx->ctx);
        ctx->ctx.setParameterIndex(index);
        return EASYJIT_OK;
    } catch (const std::exception& e) {
        set_last_error(e.what());
        return EASYJIT_ERROR_INTERNAL;
    }
}

extern "C"
easyjit_error_t easyjit_context_set_int(easyjit_context_t ctx,
                                         int64_t value) {
    clear_last_error();
    if (!ctx) {
        set_last_error("easyjit_context_set_int: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    try {
        set_scalar_layout(ctx->ctx);
        ctx->ctx.setParameterInt(value);
        return EASYJIT_OK;
    } catch (const std::exception& e) {
        set_last_error(e.what());
        return EASYJIT_ERROR_INTERNAL;
    }
}

extern "C"
easyjit_error_t easyjit_context_set_float(easyjit_context_t ctx,
                                           double value) {
    clear_last_error();
    if (!ctx) {
        set_last_error("easyjit_context_set_float: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    try {
        set_scalar_layout(ctx->ctx);
        ctx->ctx.setParameterFloat(value);
        return EASYJIT_OK;
    } catch (const std::exception& e) {
        set_last_error(e.what());
        return EASYJIT_ERROR_INTERNAL;
    }
}

extern "C"
easyjit_error_t easyjit_context_set_pointer(easyjit_context_t ctx,
                                             const void* ptr) {
    clear_last_error();
    if (!ctx) {
        set_last_error("easyjit_context_set_pointer: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    try {
        set_scalar_layout(ctx->ctx);
        ctx->ctx.setParameterPointer(ptr);
        return EASYJIT_OK;
    } catch (const std::exception& e) {
        set_last_error(e.what());
        return EASYJIT_ERROR_INTERNAL;
    }
}

extern "C"
easyjit_error_t easyjit_context_set_struct(easyjit_context_t ctx,
                                            const void* data,
                                            size_t size) {
    clear_last_error();
    if (!ctx) {
        set_last_error("easyjit_context_set_struct: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (!data && size > 0) {
        set_last_error("easyjit_context_set_struct: data is NULL with non-zero size");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    try {
        /* For structs passed via the C API we assume 1 LLVM-IR argument
         * (either passed by pointer or coerced to a single integer/register).
         * This matches the common case on aarch64/x86-64 for small structs. */
        set_scalar_layout(ctx->ctx);
        easy::serialized_arg arg(data, size);
        ctx->ctx.setParameterStruct(std::move(arg));
        return EASYJIT_OK;
    } catch (const std::exception& e) {
        set_last_error(e.what());
        return EASYJIT_ERROR_INTERNAL;
    }
}

extern "C"
easyjit_error_t easyjit_context_set_snapshot(easyjit_context_t ctx,
                                              const void* data,
                                              size_t size) {
    /* Snapshot is semantically identical to set_struct — the struct bytes
     * are serialized into the context and become compile-time constants.
     * This naming matches the C++ easy::snapshot() API. */
    return easyjit_context_set_struct(ctx, data, size);
}

extern "C"
easyjit_error_t easyjit_context_bind_array(easyjit_context_t ctx,
                                            size_t field_offset,
                                            const void* data,
                                            size_t count,
                                            size_t element_size) {
    clear_last_error();
    if (!ctx) {
        set_last_error("easyjit_context_bind_array: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (!data && count > 0) {
        set_last_error("easyjit_context_bind_array: data is NULL with non-zero count");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (element_size == 0 && count > 0) {
        set_last_error("easyjit_context_bind_array: element_size is zero with non-zero count");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    try {
        std::vector<char> bytes(count * element_size);
        if (!bytes.empty())
            std::memcpy(bytes.data(), data, bytes.size());
        ctx->ctx.bindArrayToLastStruct(field_offset, std::move(bytes), count, element_size);
        return EASYJIT_OK;
    } catch (const std::exception& e) {
        set_last_error(e.what());
        return EASYJIT_ERROR_INTERNAL;
    }
}

extern "C"
easyjit_error_t easyjit_context_set_opt_level(easyjit_context_t ctx,
                                               unsigned opt_level,
                                               unsigned opt_size) {
    clear_last_error();
    if (!ctx) {
        set_last_error("easyjit_context_set_opt_level: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (opt_level > 3) {
        set_last_error("easyjit_context_set_opt_level: opt_level must be 0-3");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (opt_size > 2) {
        set_last_error("easyjit_context_set_opt_level: opt_size must be 0-2");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    try {
        ctx->ctx.setOptLevel(opt_level, opt_size);
        return EASYJIT_OK;
    } catch (const std::exception& e) {
        set_last_error(e.what());
        return EASYJIT_ERROR_INTERNAL;
    }
}

extern "C"
easyjit_error_t easyjit_context_set_dump_ir(easyjit_context_t ctx,
                                             const char* file) {
    clear_last_error();
    if (!ctx) {
        set_last_error("easyjit_context_set_dump_ir: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    try {
        ctx->ctx.setDebugFile(file ? file : "");
        return EASYJIT_OK;
    } catch (const std::exception& e) {
        set_last_error(e.what());
        return EASYJIT_ERROR_INTERNAL;
    }
}

/* ------------------------------------------------------------------ */
/*  Compiled function handle internals                                 */
/* ------------------------------------------------------------------ */

struct easyjit_function_s {
    std::unique_ptr<easy::Function> fun;
};

extern "C"
easyjit_error_t easyjit_compile(void* func_ptr,
                                 easyjit_context_t ctx,
                                 easyjit_function_t* out_fn) {
    clear_last_error();
    if (!func_ptr) {
        set_last_error("easyjit_compile: func_ptr is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (!ctx) {
        set_last_error("easyjit_compile: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (!out_fn) {
        set_last_error("easyjit_compile: out_fn is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    try {
        auto compiled = easy::Function::Compile(func_ptr, ctx->ctx);
        if (!compiled) {
            set_last_error("easyjit_compile: compilation returned null");
            return EASYJIT_ERROR_COMPILE_FAILED;
        }
        auto* handle = new easyjit_function_s();
        handle->fun = std::move(compiled);
        *out_fn = handle;
        return EASYJIT_OK;
    } catch (const std::exception& e) {
        set_last_error(e.what());
        return EASYJIT_ERROR_COMPILE_FAILED;
    }
}

extern "C"
easyjit_error_t easyjit_get_function_pointer(easyjit_function_t fn,
                                              void** out_ptr) {
    clear_last_error();
    if (!fn) {
        set_last_error("easyjit_get_function_pointer: fn is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (!out_ptr) {
        set_last_error("easyjit_get_function_pointer: out_ptr is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (!fn->fun) {
        set_last_error("easyjit_get_function_pointer: function not compiled");
        return EASYJIT_ERROR_INTERNAL;
    }
    *out_ptr = fn->fun->getRawPointer();
    return EASYJIT_OK;
}

extern "C"
void easyjit_function_destroy(easyjit_function_t fn) {
    delete fn;
}

/* ------------------------------------------------------------------ */
/*  Simple cache                                                       */
/* ------------------------------------------------------------------ */

struct easyjit_cache_s {
    /* key → compiled function (owns the memory) */
    std::unordered_map<int64_t, easyjit_function_s*> entries;
};

extern "C"
easyjit_error_t easyjit_cache_create(easyjit_cache_t* out_cache) {
    clear_last_error();
    if (!out_cache) {
        set_last_error("easyjit_cache_create: out_cache is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    try {
        *out_cache = new easyjit_cache_s();
        return EASYJIT_OK;
    } catch (const std::exception& e) {
        set_last_error(e.what());
        return EASYJIT_ERROR_INTERNAL;
    }
}

extern "C"
void easyjit_cache_destroy(easyjit_cache_t cache) {
    if (!cache) return;
    for (auto& kv : cache->entries) {
        delete kv.second;
    }
    delete cache;
}

extern "C"
easyjit_error_t easyjit_cache_get_or_compile(easyjit_cache_t cache,
                                              int64_t key,
                                              void* func_ptr,
                                              easyjit_context_t ctx,
                                              void** out_ptr) {
    clear_last_error();
    if (!cache) {
        set_last_error("easyjit_cache_get_or_compile: cache is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (!out_ptr) {
        set_last_error("easyjit_cache_get_or_compile: out_ptr is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }

    /* Cache hit? */
    auto it = cache->entries.find(key);
    if (it != cache->entries.end()) {
        *out_ptr = it->second->fun->getRawPointer();
        return EASYJIT_OK;
    }

    /* Cache miss — compile. */
    if (!func_ptr) {
        set_last_error("easyjit_cache_get_or_compile: cache miss but func_ptr is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (!ctx) {
        set_last_error("easyjit_cache_get_or_compile: cache miss but ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }

    easyjit_function_t fn = nullptr;
    easyjit_error_t err = easyjit_compile(func_ptr, ctx, &fn);
    if (err != EASYJIT_OK) {
        return err;
    }

    *out_ptr = fn->fun->getRawPointer();
    cache->entries[key] = fn;
    return EASYJIT_OK;
}

extern "C"
easyjit_error_t easyjit_cache_has(easyjit_cache_t cache,
                                   int64_t key,
                                   int* out_hit) {
    clear_last_error();
    if (!cache || !out_hit) {
        set_last_error("easyjit_cache_has: NULL argument");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    *out_hit = (cache->entries.find(key) != cache->entries.end()) ? 1 : 0;
    return EASYJIT_OK;
}
