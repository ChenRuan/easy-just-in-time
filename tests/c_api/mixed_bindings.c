/**
 * @file mixed_bindings.c
 * @brief Example of mixing runtime args, scalar constants, and multiple struct snapshots.
 */

#include <easy/easyjit_c.h>
#include <easy/attributes.h>

#include <math.h>
#include <stdio.h>

typedef struct {
    int gain;
    int bias;
} ConfigA;

typedef struct {
    int enabled;
    int coeffs[4];
} ConfigB;

int EASY_JIT_EXPOSE process(const float* input,
                            float scale,
                            const ConfigA* a,
                            const ConfigB* b,
                            int index,
                            float offset) {
    int acc = (int)(input[index] * scale + offset);

    if (b->enabled) {
        acc += a->gain;
        acc += b->coeffs[2];
    } else {
        acc -= a->bias;
    }

    return acc;
}

static int process_ref(const float* input,
                       float scale,
                       const ConfigA* a,
                       const ConfigB* b,
                       int index,
                       float offset) {
    int acc = (int)(input[index] * scale + offset);

    if (b->enabled) {
        acc += a->gain;
        acc += b->coeffs[2];
    } else {
        acc -= a->bias;
    }

    return acc;
}

int main(void) {
    float input[8] = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f};
    ConfigA a = {10, 3};
    ConfigB b = {1, {11, 22, 33, 44}};

    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    void* raw = NULL;

    if (easyjit_context_create(&ctx) != EASYJIT_OK) {
        fprintf(stderr, "context_create failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    easyjit_context_set_forward(ctx, 0);
    easyjit_context_set_float(ctx, 2.0);
    easyjit_context_set_snapshot(ctx, &a, sizeof(a));
    easyjit_context_set_snapshot(ctx, &b, sizeof(b));
    easyjit_context_set_int(ctx, 4);
    easyjit_context_set_forward(ctx, 1);
    easyjit_context_set_opt_level(ctx, 3, 0);

    if (easyjit_compile((void*)process, ctx, &fn) != EASYJIT_OK) {
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

    typedef int (*process_spec_t)(const float*, float);
    process_spec_t spec = (process_spec_t)raw;

    {
        int got = spec(input, 1.5f);
        int want = process_ref(input, 2.0f, &a, &b, 4, 1.5f);
        printf("mixed.result=%d ref=%d\n", got, want);
        if (got != want) {
            fprintf(stderr, "mismatch: got=%d want=%d\n", got, want);
            easyjit_function_destroy(fn);
            return 1;
        }
    }

    easyjit_function_destroy(fn);
    return 0;
}
