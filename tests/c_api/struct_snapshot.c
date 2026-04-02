/**
 * @file struct_snapshot.c
 * @brief Verify C API struct snapshot specialization and IR dumping.
 */

#include <easy/easyjit_c.h>
#include <easy/attributes.h>

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int enabled;
    int gain;
    int bias;
} Config;

int EASY_JIT_EXPOSE eval_config(const Config* cfg, int x) {
    if (cfg->enabled) {
        return x + cfg->gain;
    }
    return x - cfg->bias;
}

int main(int argc, char** argv) {
    Config cfg = {1, 7, 99};
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    void* raw = NULL;

    if (easyjit_context_create(&ctx) != EASYJIT_OK) {
        fprintf(stderr, "context_create failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    if (easyjit_context_set_snapshot(ctx, &cfg, sizeof(cfg)) != EASYJIT_OK) {
        fprintf(stderr, "set_snapshot failed: %s\n", easyjit_get_last_error());
        easyjit_context_destroy(ctx);
        return 1;
    }

    if (easyjit_context_set_forward(ctx, 0) != EASYJIT_OK) {
        fprintf(stderr, "set_forward failed: %s\n", easyjit_get_last_error());
        easyjit_context_destroy(ctx);
        return 1;
    }

    if (easyjit_context_set_opt_level(ctx, 3, 0) != EASYJIT_OK) {
        fprintf(stderr, "set_opt_level failed: %s\n", easyjit_get_last_error());
        easyjit_context_destroy(ctx);
        return 1;
    }

    if (argc > 1) {
        if (easyjit_context_set_dump_ir(ctx, argv[1]) != EASYJIT_OK) {
            fprintf(stderr, "set_dump_ir failed: %s\n", easyjit_get_last_error());
            easyjit_context_destroy(ctx);
            return 1;
        }
    }

    if (easyjit_compile((void*)eval_config, ctx, &fn) != EASYJIT_OK) {
        fprintf(stderr, "compile failed: %s\n", easyjit_get_last_error());
        easyjit_context_destroy(ctx);
        return 1;
    }
    easyjit_context_destroy(ctx);

    if (easyjit_get_function_pointer(fn, &raw) != EASYJIT_OK) {
        fprintf(stderr, "get_function_pointer failed: %s\n", easyjit_get_last_error());
        easyjit_function_destroy(fn);
        return 1;
    }

    typedef int (*eval_config_spec_t)(int);
    eval_config_spec_t spec = (eval_config_spec_t)raw;

    printf("result=%d\n", spec(5));

    easyjit_function_destroy(fn);
    return 0;
}
