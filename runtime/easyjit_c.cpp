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

#include "SreDebugLog.h"

#include <cstring>
#include <memory>
#include <unordered_map>
#include <string>
#include <cstdio>

#define EASYJIT_RT_LOG(...) EASYJIT_SRE_LOG("[c-api] " __VA_ARGS__)

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
/*  Process-local error message                                        */
/* ------------------------------------------------------------------ */

static std::string g_last_error;

static void set_last_error(const char* msg) {
    EASYJIT_RT_LOG("set_last_error: %s\n", msg ? msg : "<null>");
    g_last_error = msg ? msg : "";
}

static void clear_last_error() {
    EASYJIT_RT_LOG("clear_last_error\n");
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
    EASYJIT_RT_LOG("easyjit_context_create: begin out_ctx=%p\n", (void*)out_ctx);
    if (!out_ctx) {
        set_last_error("easyjit_context_create: out_ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
        *out_ctx = new easyjit_context_s();
        EASYJIT_RT_LOG("easyjit_context_create: success ctx=%p inner=%p\n",
                       (void*)*out_ctx, (void*)&((*out_ctx)->ctx));
        return EASYJIT_OK;
}

extern "C"
void easyjit_context_destroy(easyjit_context_t ctx) {
    EASYJIT_RT_LOG("easyjit_context_destroy: ctx=%p\n", (void*)ctx);
    delete ctx;
}

/* --- Parameter binding -------------------------------------------- */

extern "C"
easyjit_error_t easyjit_context_set_forward(easyjit_context_t ctx,
                                             unsigned index) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_context_set_forward: ctx=%p index=%u\n",
                   (void*)ctx, index);
    if (!ctx) {
        set_last_error("easyjit_context_set_forward: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
        set_scalar_layout(ctx->ctx);
        ctx->ctx.setParameterIndex(index);
        return EASYJIT_OK;
}

extern "C"
easyjit_error_t easyjit_context_set_int(easyjit_context_t ctx,
                                         int64_t value) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_context_set_int: ctx=%p value=%lld\n",
                   (void*)ctx, (long long)value);
    if (!ctx) {
        set_last_error("easyjit_context_set_int: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
        set_scalar_layout(ctx->ctx);
        ctx->ctx.setParameterInt(value);
        return EASYJIT_OK;
}

extern "C"
easyjit_error_t easyjit_context_set_float(easyjit_context_t ctx,
                                           double value) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_context_set_float: ctx=%p value=%f\n",
                   (void*)ctx, value);
    if (!ctx) {
        set_last_error("easyjit_context_set_float: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
        set_scalar_layout(ctx->ctx);
        ctx->ctx.setParameterFloat(value);
        return EASYJIT_OK;
}

extern "C"
easyjit_error_t easyjit_context_set_pointer(easyjit_context_t ctx,
                                             const void* ptr) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_context_set_pointer: ctx=%p ptr=%p\n",
                   (void*)ctx, ptr);
    if (!ctx) {
        set_last_error("easyjit_context_set_pointer: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
        set_scalar_layout(ctx->ctx);
        ctx->ctx.setParameterPointer(ptr);
        return EASYJIT_OK;
}

extern "C"
easyjit_error_t easyjit_context_set_struct(easyjit_context_t ctx,
                                            const void* data,
                                            size_t size) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_context_set_struct: ctx=%p data=%p size=%zu\n",
                   (void*)ctx, data, size);
    if (!ctx) {
        set_last_error("easyjit_context_set_struct: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (!data && size > 0) {
        set_last_error("easyjit_context_set_struct: data is NULL with non-zero size");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
        /* For structs passed via the C API we assume 1 LLVM-IR argument
         * (either passed by pointer or coerced to a single integer/register).
         * This matches the common case on aarch64/x86-64 for small structs. */
        set_scalar_layout(ctx->ctx);
        easy::serialized_arg arg(data, size);
        ctx->ctx.setParameterStruct(std::move(arg));
        return EASYJIT_OK;
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
easyjit_error_t easyjit_context_set_global_snapshot(easyjit_context_t ctx,
                                                     const void* global_addr,
                                                     const void* data,
                                                     size_t size) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_context_set_global_snapshot: ctx=%p global=%p data=%p size=%zu\n",
                   (void*)ctx, global_addr, data, size);
    if (!ctx) {
        set_last_error("easyjit_context_set_global_snapshot: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (!global_addr) {
        set_last_error("easyjit_context_set_global_snapshot: global_addr is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (!data && size > 0) {
        set_last_error("easyjit_context_set_global_snapshot: data is NULL with non-zero size");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
        easy::serialized_arg arg(data, size);
        ctx->ctx.setGlobalStruct(global_addr, std::move(arg));
        return EASYJIT_OK;
}

extern "C"
easyjit_error_t easyjit_context_set_global_partial_struct(easyjit_context_t ctx,
                                                           const void* global_addr) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_context_set_global_partial_struct: ctx=%p global=%p\n",
                   (void*)ctx, global_addr);
    if (!ctx) {
        set_last_error("easyjit_context_set_global_partial_struct: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (!global_addr) {
        set_last_error("easyjit_context_set_global_partial_struct: global_addr is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
        ctx->ctx.setGlobalPartialStruct(global_addr);
        return EASYJIT_OK;
}

extern "C"
easyjit_error_t easyjit_context_bind_global_field(easyjit_context_t ctx,
                                                   size_t field_offset,
                                                   const void* data,
                                                   size_t size) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_context_bind_global_field: ctx=%p offset=%zu data=%p size=%zu\n",
                   (void*)ctx, field_offset, data, size);
    if (!ctx) {
        set_last_error("easyjit_context_bind_global_field: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (!data && size > 0) {
        set_last_error("easyjit_context_bind_global_field: data is NULL with non-zero size");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
        std::vector<char> bytes(size);
        if (!bytes.empty())
            std::memcpy(bytes.data(), data, size);
        ctx->ctx.bindFieldToLastGlobalPartialStruct(field_offset, std::move(bytes));
        return EASYJIT_OK;
}

extern "C"
easyjit_error_t easyjit_context_bind_global_array(easyjit_context_t ctx,
                                                   size_t field_offset,
                                                   const void* data,
                                                   size_t count,
                                                   size_t element_size) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_context_bind_global_array: ctx=%p offset=%zu data=%p count=%zu elem=%zu total=%zu\n",
                   (void*)ctx, field_offset, data, count, element_size,
                   count * element_size);
    if (!ctx) {
        set_last_error("easyjit_context_bind_global_array: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (!data && count > 0) {
        set_last_error("easyjit_context_bind_global_array: data is NULL with non-zero count");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (element_size == 0 && count > 0) {
        set_last_error("easyjit_context_bind_global_array: element_size is zero with non-zero count");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
        std::vector<char> bytes(count * element_size);
        if (!bytes.empty())
            std::memcpy(bytes.data(), data, bytes.size());
        ctx->ctx.bindArrayToLastGlobalPartialStruct(field_offset, std::move(bytes), count, element_size);
        return EASYJIT_OK;
}

extern "C"
easyjit_error_t easyjit_context_set_partial_struct(easyjit_context_t ctx,
                                                    unsigned index) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_context_set_partial_struct: ctx=%p index=%u\n",
                   (void*)ctx, index);
    if (!ctx) {
        set_last_error("easyjit_context_set_partial_struct: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
        set_scalar_layout(ctx->ctx);
        ctx->ctx.setPartialStruct(index);
        return EASYJIT_OK;
}

extern "C"
easyjit_error_t easyjit_context_bind_field(easyjit_context_t ctx,
                                            size_t field_offset,
                                            const void* data,
                                            size_t size) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_context_bind_field: ctx=%p offset=%zu data=%p size=%zu\n",
                   (void*)ctx, field_offset, data, size);
    if (!ctx) {
        set_last_error("easyjit_context_bind_field: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (!data && size > 0) {
        set_last_error("easyjit_context_bind_field: data is NULL with non-zero size");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
        std::vector<char> bytes(size);
        if (!bytes.empty())
            std::memcpy(bytes.data(), data, size);
        ctx->ctx.bindFieldToLastPartialStruct(field_offset, std::move(bytes));
        return EASYJIT_OK;
}

extern "C"
easyjit_error_t easyjit_context_bind_array(easyjit_context_t ctx,
                                            size_t field_offset,
                                            const void* data,
                                            size_t count,
                                            size_t element_size) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_context_bind_array: ctx=%p offset=%zu data=%p count=%zu elem=%zu total=%zu\n",
                   (void*)ctx, field_offset, data, count, element_size,
                   count * element_size);
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
        std::vector<char> bytes(count * element_size);
        if (!bytes.empty())
            std::memcpy(bytes.data(), data, bytes.size());
        ctx->ctx.bindArrayToLastStruct(field_offset, std::move(bytes), count, element_size);
        return EASYJIT_OK;
}

extern "C"
easyjit_error_t easyjit_context_set_array(easyjit_context_t ctx,
                                           const void* data,
                                           size_t count,
                                           size_t element_size) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_context_set_array: ctx=%p data=%p count=%zu elem=%zu total=%zu\n",
                   (void*)ctx, data, count, element_size, count * element_size);
    if (!ctx) {
        set_last_error("easyjit_context_set_array: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (!data && count > 0) {
        set_last_error("easyjit_context_set_array: data is NULL with non-zero count");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    if (element_size == 0 && count > 0) {
        set_last_error("easyjit_context_set_array: element_size is zero with non-zero count");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
        set_scalar_layout(ctx->ctx);
        std::vector<char> bytes(count * element_size);
        if (!bytes.empty())
            std::memcpy(bytes.data(), data, bytes.size());
        ctx->ctx.setParameterArray(std::move(bytes), count, element_size);
        return EASYJIT_OK;
}

extern "C"
easyjit_error_t easyjit_context_set_opt_level(easyjit_context_t ctx,
                                               unsigned opt_level,
                                               unsigned opt_size) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_context_set_opt_level: ctx=%p opt=%u size=%u\n",
                   (void*)ctx, opt_level, opt_size);
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
        ctx->ctx.setOptLevel(opt_level, opt_size);
        return EASYJIT_OK;
}

extern "C"
easyjit_error_t easyjit_context_set_dump_ir(easyjit_context_t ctx,
                                             const char* file) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_context_set_dump_ir: ctx=%p file=%s\n",
                   (void*)ctx, file ? file : "<null>");
    if (!ctx) {
        set_last_error("easyjit_context_set_dump_ir: ctx is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
        ctx->ctx.setDebugFile(file ? file : "");
        return EASYJIT_OK;
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
    EASYJIT_RT_LOG("easyjit_compile: begin func_ptr=%p ctx=%p out_fn=%p\n",
                   func_ptr, (void*)ctx, (void*)out_fn);
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
    EASYJIT_RT_LOG("easyjit_compile: calling easy::Function::Compile\n");
    auto compiled = easy::Function::Compile(func_ptr, ctx->ctx);
    EASYJIT_RT_LOG("easyjit_compile: Function::Compile returned unique_ptr=%p\n",
                   (void*)compiled.get());
    if (!compiled) {
        EASYJIT_RT_LOG("easyjit_compile: easy::Function::Compile returned null\n");
        set_last_error("easyjit_compile: compilation returned null");
        return EASYJIT_ERROR_COMPILE_FAILED;
    }
    auto* handle = new easyjit_function_s();
    if (!handle) {
        set_last_error("easyjit_compile: handle allocation failed");
        return EASYJIT_ERROR_INTERNAL;
    }
    EASYJIT_RT_LOG("easyjit_compile: allocated handle=%p\n", (void*)handle);
    handle->fun = std::move(compiled);
    *out_fn = handle;
    EASYJIT_RT_LOG("easyjit_compile: success handle=%p\n", (void*)handle);
    return EASYJIT_OK;
}

extern "C"
easyjit_error_t easyjit_get_function_pointer(easyjit_function_t fn,
                                              void** out_ptr) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_get_function_pointer: fn=%p out_ptr=%p\n",
                   (void*)fn, (void*)out_ptr);
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
    EASYJIT_RT_LOG("easyjit_get_function_pointer: raw=%p\n", *out_ptr);
    return EASYJIT_OK;
}

extern "C"
void easyjit_function_destroy(easyjit_function_t fn) {
    EASYJIT_RT_LOG("easyjit_function_destroy: fn=%p\n", (void*)fn);
    delete fn;
}

/* ------------------------------------------------------------------ */
/*  Simple cache                                                       */
/* ------------------------------------------------------------------ */

struct easyjit_cache_s {
    /* key → compiled function (owns the memory) */
    std::unordered_map<int64_t, easyjit_function_s*> entries;
};

static void easyjit_cache_clear_entries(easyjit_cache_s* cache) {
    EASYJIT_RT_LOG("easyjit_cache_clear_entries: cache=%p entries=%zu\n",
                   (void*)cache, cache ? cache->entries.size() : 0);
    for (auto& kv : cache->entries) {
        delete kv.second;
    }
    cache->entries.clear();
}

extern "C"
easyjit_error_t easyjit_cache_create(easyjit_cache_t* out_cache) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_cache_create: out_cache=%p\n", (void*)out_cache);
    if (!out_cache) {
        set_last_error("easyjit_cache_create: out_cache is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    *out_cache = new easyjit_cache_s();
    if (!*out_cache) {
        set_last_error("easyjit_cache_create: allocation failed");
        return EASYJIT_ERROR_INTERNAL;
    }
    return EASYJIT_OK;
}

extern "C"
void easyjit_cache_destroy(easyjit_cache_t cache) {
    EASYJIT_RT_LOG("easyjit_cache_destroy: cache=%p\n", (void*)cache);
    if (!cache) return;
    easyjit_cache_clear_entries(cache);
    delete cache;
}

extern "C"
easyjit_error_t easyjit_cache_clear(easyjit_cache_t cache) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_cache_clear: cache=%p\n", (void*)cache);
    if (!cache) {
        set_last_error("easyjit_cache_clear: cache is NULL");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    easyjit_cache_clear_entries(cache);
    return EASYJIT_OK;
}

extern "C"
easyjit_error_t easyjit_cache_get_or_compile(easyjit_cache_t cache,
                                              int64_t key,
                                              void* func_ptr,
                                              easyjit_context_t ctx,
                                              void** out_ptr) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_cache_get_or_compile: cache=%p key=%lld func=%p ctx=%p out=%p\n",
                   (void*)cache, (long long)key, func_ptr, (void*)ctx,
                   (void*)out_ptr);
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
        EASYJIT_RT_LOG("easyjit_cache_get_or_compile: hit key=%lld raw=%p\n",
                       (long long)key, *out_ptr);
        return EASYJIT_OK;
    }
    EASYJIT_RT_LOG("easyjit_cache_get_or_compile: miss key=%lld\n",
                   (long long)key);

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
    EASYJIT_RT_LOG("easyjit_cache_get_or_compile: compile err=%d fn=%p\n",
                   (int)err, (void*)fn);
    if (err != EASYJIT_OK) {
        return err;
    }

    *out_ptr = fn->fun->getRawPointer();
    cache->entries[key] = fn;
    EASYJIT_RT_LOG("easyjit_cache_get_or_compile: stored key=%lld raw=%p entries=%zu\n",
                   (long long)key, *out_ptr, cache->entries.size());
    return EASYJIT_OK;
}

extern "C"
easyjit_error_t easyjit_cache_has(easyjit_cache_t cache,
                                   int64_t key,
                                   int* out_hit) {
    clear_last_error();
    EASYJIT_RT_LOG("easyjit_cache_has: cache=%p key=%lld out_hit=%p\n",
                   (void*)cache, (long long)key, (void*)out_hit);
    if (!cache || !out_hit) {
        set_last_error("easyjit_cache_has: NULL argument");
        return EASYJIT_ERROR_INVALID_ARGUMENT;
    }
    *out_hit = (cache->entries.find(key) != cache->entries.end()) ? 1 : 0;
    return EASYJIT_OK;
}
