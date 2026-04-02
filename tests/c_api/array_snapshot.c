/**
 * @file array_snapshot.c
 * @brief Verify C API array snapshot specialization.
 */

#include <easy/easyjit_c.h>
#include <easy/attributes.h>

#include <stdio.h>

int EASY_JIT_EXPOSE eval_array(int x, const int* data) {
    return x + data[0] + data[3];
}

int main(void) {
    int data[4] = {10, 20, 30, 40};
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    void* raw = NULL;

    if (easyjit_context_create(&ctx) != EASYJIT_OK) {
        fprintf(stderr, "context_create failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    if (easyjit_context_set_forward(ctx, 0) != EASYJIT_OK) {
        fprintf(stderr, "set_forward failed: %s\n", easyjit_get_last_error());
        easyjit_context_destroy(ctx);
        return 1;
    }

    if (easyjit_context_set_array(ctx, data, 4, sizeof(int)) != EASYJIT_OK) {
        fprintf(stderr, "set_array failed: %s\n", easyjit_get_last_error());
        easyjit_context_destroy(ctx);
        return 1;
    }

    if (easyjit_context_set_opt_level(ctx, 3, 0) != EASYJIT_OK) {
        fprintf(stderr, "set_opt_level failed: %s\n", easyjit_get_last_error());
        easyjit_context_destroy(ctx);
        return 1;
    }

    if (easyjit_compile((void*)eval_array, ctx, &fn) != EASYJIT_OK) {
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

    typedef int (*eval_array_spec_t)(int);
    eval_array_spec_t spec = (eval_array_spec_t)raw;

    printf("array_snapshot.result=%d\n", spec(7));

    easyjit_function_destroy(fn);
    return 0;
}
