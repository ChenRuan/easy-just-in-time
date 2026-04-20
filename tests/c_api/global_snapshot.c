#include <easy/easyjit_c.h>
#include <easy/attributes.h>

#include <stdio.h>

typedef struct {
    int enabled;
    int bias;
} GlobalConfig;

GlobalConfig g_cfg = {1, 7};

int EASY_JIT_EXPOSE eval_global_snapshot_c(int x) {
    if (g_cfg.enabled) {
        return x + g_cfg.bias;
    }
    return x - 99;
}

int main(void) {
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    void *raw = NULL;
    typedef int (*jit_fn_t)(int);

    if (easyjit_context_create(&ctx) != EASYJIT_OK) {
        printf("context_create failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    if (easyjit_context_set_forward(ctx, 0) != EASYJIT_OK) {
        printf("set_forward failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    if (easyjit_context_set_global_snapshot(ctx, &g_cfg, &g_cfg, sizeof(g_cfg)) != EASYJIT_OK) {
        printf("set_global_snapshot failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    if (easyjit_compile((void *)eval_global_snapshot_c, ctx, &fn) != EASYJIT_OK) {
        printf("compile failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    if (easyjit_get_function_pointer(fn, &raw) != EASYJIT_OK) {
        printf("get_function_pointer failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    g_cfg.enabled = 0;
    g_cfg.bias = 1000;

    printf("global_snapshot_c.result=%d\n", ((jit_fn_t)raw)(5));

    easyjit_function_destroy(fn);
    easyjit_context_destroy(ctx);
    return 0;
}
