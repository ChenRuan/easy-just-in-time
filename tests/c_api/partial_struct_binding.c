/*
 * @file partial_struct_binding.c
 * @brief Verify partial struct field specialization while keeping the struct pointer runtime-visible.
 */

#include <stdio.h>
#include <stddef.h>

#include "easy/easyjit_c.h"
#include "easy/attributes.h"

typedef struct {
    int dynamic_value;
    int fixed_flag;
} PartialConfig;

int EASY_JIT_EXPOSE eval_partial_config(const PartialConfig *cfg, int x) {
    if (cfg->fixed_flag) {
        return x + cfg->dynamic_value + cfg->fixed_flag;
    }
    return x - cfg->dynamic_value;
}

int main(void) {
    PartialConfig sample = {.dynamic_value = 111, .fixed_flag = 7};
    PartialConfig runtime_a = {.dynamic_value = 3, .fixed_flag = 0};
    PartialConfig runtime_b = {.dynamic_value = 9, .fixed_flag = 1234};

    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    void *raw = NULL;

    if (easyjit_context_create(&ctx) != EASYJIT_OK) {
        fprintf(stderr, "context_create failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    if (easyjit_context_set_partial_struct(ctx, 0) != EASYJIT_OK) {
        fprintf(stderr, "set_partial_struct failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    if (easyjit_context_bind_field(ctx,
                                   offsetof(PartialConfig, fixed_flag),
                                   &sample.fixed_flag,
                                   sizeof(sample.fixed_flag)) != EASYJIT_OK) {
        fprintf(stderr, "bind_field failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    if (easyjit_context_set_forward(ctx, 1) != EASYJIT_OK) {
        fprintf(stderr, "set_forward failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    if (easyjit_compile((void *)eval_partial_config, ctx, &fn) != EASYJIT_OK) {
        fprintf(stderr, "compile failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    easyjit_context_destroy(ctx);
    ctx = NULL;

    if (easyjit_get_function_pointer(fn, &raw) != EASYJIT_OK) {
        fprintf(stderr, "get_function_pointer failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    int (*spec)(const PartialConfig *, int) = (int (*)(const PartialConfig *, int))raw;

    int result_a = spec(&runtime_a, 10);
    int result_b = spec(&runtime_b, 10);

    printf("partial_struct_binding.result_a=%d\n", result_a);
    printf("partial_struct_binding.result_b=%d\n", result_b);

    easyjit_function_destroy(fn);
    return (result_a == 20 && result_b == 26) ? 0 : 1;
}
