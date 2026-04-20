#include <easy/easyjit_c.h>
#include <easy/attributes.h>

#include <stddef.h>
#include <stdio.h>

typedef struct {
    int enabled;
    int bias;
} PartialGlobalConfig;

typedef struct {
    int enabled;
    int bias;
    const int *values;
} PartialGlobalArrayConfig;

PartialGlobalConfig g_partial_cfg = {1, 7};
static int g_values[] = {10, 20, 30};
PartialGlobalArrayConfig g_partial_array_cfg = {1, 2, g_values};

int EASY_JIT_EXPOSE eval_global_partial_c(int x) {
    if (g_partial_cfg.enabled) {
        return x + g_partial_cfg.bias;
    }
    return x - 99;
}

int EASY_JIT_EXPOSE eval_global_partial_array_c(int idx) {
    if (g_partial_array_cfg.enabled) {
        return g_partial_array_cfg.values[idx] + g_partial_array_cfg.bias;
    }
    return -1;
}

typedef int (*jit_unary_fn_t)(int);

static int run_partial_field_case(void) {
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    void *raw = NULL;

    if (easyjit_context_create(&ctx) != EASYJIT_OK) {
        printf("context_create failed: %s\n", easyjit_get_last_error());
        return 1;
    }
    if (easyjit_context_set_forward(ctx, 0) != EASYJIT_OK) {
        printf("set_forward failed: %s\n", easyjit_get_last_error());
        return 1;
    }
    if (easyjit_context_set_global_partial_struct(ctx, &g_partial_cfg) != EASYJIT_OK) {
        printf("set_global_partial_struct failed: %s\n", easyjit_get_last_error());
        return 1;
    }
    if (easyjit_context_bind_global_field(ctx,
                                          offsetof(PartialGlobalConfig, enabled),
                                          &g_partial_cfg.enabled,
                                          sizeof(g_partial_cfg.enabled)) != EASYJIT_OK) {
        printf("bind_global_field failed: %s\n", easyjit_get_last_error());
        return 1;
    }
    if (easyjit_compile((void *)eval_global_partial_c, ctx, &fn) != EASYJIT_OK) {
        printf("compile failed: %s\n", easyjit_get_last_error());
        return 1;
    }
    if (easyjit_get_function_pointer(fn, &raw) != EASYJIT_OK) {
        printf("get_function_pointer failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    g_partial_cfg.enabled = 0;
    g_partial_cfg.bias = 1000;

    printf("global_partial_snapshot_c.result=%d\n", ((jit_unary_fn_t)raw)(5));

    easyjit_function_destroy(fn);
    easyjit_context_destroy(ctx);
    return 0;
}

static int run_partial_array_case(void) {
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    void *raw = NULL;

    if (easyjit_context_create(&ctx) != EASYJIT_OK) {
        printf("context_create failed: %s\n", easyjit_get_last_error());
        return 1;
    }
    if (easyjit_context_set_forward(ctx, 0) != EASYJIT_OK) {
        printf("set_forward failed: %s\n", easyjit_get_last_error());
        return 1;
    }
    if (easyjit_context_set_global_partial_struct(ctx, &g_partial_array_cfg) != EASYJIT_OK) {
        printf("set_global_partial_struct failed: %s\n", easyjit_get_last_error());
        return 1;
    }
    if (easyjit_context_bind_global_field(ctx,
                                          offsetof(PartialGlobalArrayConfig, enabled),
                                          &g_partial_array_cfg.enabled,
                                          sizeof(g_partial_array_cfg.enabled)) != EASYJIT_OK) {
        printf("bind_global_field failed: %s\n", easyjit_get_last_error());
        return 1;
    }
    if (easyjit_context_bind_global_array(ctx,
                                          offsetof(PartialGlobalArrayConfig, values),
                                          g_partial_array_cfg.values,
                                          3,
                                          sizeof(g_partial_array_cfg.values[0])) != EASYJIT_OK) {
        printf("bind_global_array failed: %s\n", easyjit_get_last_error());
        return 1;
    }
    if (easyjit_compile((void *)eval_global_partial_array_c, ctx, &fn) != EASYJIT_OK) {
        printf("compile failed: %s\n", easyjit_get_last_error());
        return 1;
    }
    if (easyjit_get_function_pointer(fn, &raw) != EASYJIT_OK) {
        printf("get_function_pointer failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    g_partial_array_cfg.enabled = 0;
    g_partial_array_cfg.bias = 50;
    g_values[0] = 100;
    g_values[1] = 200;
    g_values[2] = 300;

    printf("global_partial_snapshot_c.array_result=%d\n", ((jit_unary_fn_t)raw)(1));

    easyjit_function_destroy(fn);
    easyjit_context_destroy(ctx);
    return 0;
}

int main(void) {
    if (run_partial_field_case() != 0) {
        return 1;
    }
    if (run_partial_array_case() != 0) {
        return 1;
    }
    return 0;
}
