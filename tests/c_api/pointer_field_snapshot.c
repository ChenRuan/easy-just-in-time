/**
 * @file pointer_field_snapshot.c
 * @brief Verify C API snapshot + bind_array for struct pointer fields.
 */

#include <easy/easyjit_c.h>
#include <easy/attributes.h>

#include <stddef.h>
#include <stdio.h>

typedef struct {
    int base;
    const int* array;
    int unused;
} Config;

int EASY_JIT_EXPOSE eval_pointer_field(const Config* cfg, int x) {
    const int* local = cfg->array;
    return x + cfg->base + local[2];
}

int main(int argc, char** argv) {
    int data[4] = {11, 22, 33, 44};
    Config cfg = {7, data, 99};
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

    if (easyjit_context_bind_array(ctx,
                                   offsetof(Config, array),
                                   cfg.array,
                                   4,
                                   sizeof(int)) != EASYJIT_OK) {
        fprintf(stderr, "bind_array failed: %s\n", easyjit_get_last_error());
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

    if (easyjit_compile((void*)eval_pointer_field, ctx, &fn) != EASYJIT_OK) {
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

    typedef int (*eval_pointer_field_spec_t)(int);
    eval_pointer_field_spec_t spec = (eval_pointer_field_spec_t)raw;

    printf("pointer_field.result=%d\n", spec(5));

    easyjit_function_destroy(fn);
    return 0;
}
