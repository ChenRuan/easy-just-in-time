/**
 * @file add_int.c
 * @brief Minimal C API test: specialize add(a,b) with b=1 to get inc(a).
 *
 * This is the pure-C equivalent of the C++ test tests/simple/int_a.cpp.
 * It demonstrates the easyjit C API workflow:
 *   1. Create a context
 *   2. Bind parameters (forward a, specialize b=1)
 *   3. Compile
 *   4. Get function pointer and call
 *   5. Cleanup
 *
 * Build (two-step):
 *   # Step 1: compile with EasyJIT pass to embed bitcode
 *   clang -g -Xclang -disable-O0-optnone \
 *         -I<easyjit-include> \
 *         -Xclang -fpass-plugin=<EasyJitPass.so> \
 *         -c add_int.c -o add_int.o
 *
 *   # Step 2: link against the runtime
 *   clang add_int.o -L<easyjit-lib> -lEasyJitRuntime -o add_int
 */

#include <easy/easyjit_c.h>
#include <easy/attributes.h>
#include <stdio.h>
#include <stdlib.h>

/* The function we want to specialize at runtime.
 * EASY_JIT_EXPOSE tells the EasyJIT compiler pass to embed its bitcode
 * so it can be specialized at runtime.
 */
int EASY_JIT_EXPOSE add(int a, int b) {
    return a + b;
}

int main(void) {
    easyjit_error_t err;

    /* 1. Create a specialization context */
    easyjit_context_t ctx = NULL;
    err = easyjit_context_create(&ctx);
    if (err != EASYJIT_OK) {
        fprintf(stderr, "context_create failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    /* 2. Describe the parameter mapping:
     *    - param 0 (a): forward as runtime arg index 0
     *    - param 1 (b): specialize to constant 1
     */
    easyjit_context_set_forward(ctx, 0);
    easyjit_context_set_int(ctx, 1);

    /* 3. JIT-compile */
    easyjit_function_t fn = NULL;
    err = easyjit_compile((void*)add, ctx, &fn);
    if (err != EASYJIT_OK) {
        fprintf(stderr, "compile failed: %s\n", easyjit_get_last_error());
        easyjit_context_destroy(ctx);
        return 1;
    }

    /* 4. Get the raw function pointer */
    void* raw_ptr = NULL;
    err = easyjit_get_function_pointer(fn, &raw_ptr);
    if (err != EASYJIT_OK) {
        fprintf(stderr, "get_function_pointer failed: %s\n", easyjit_get_last_error());
        easyjit_function_destroy(fn);
        easyjit_context_destroy(ctx);
        return 1;
    }

    /* Cast to the specialized signature: int(int) */
    typedef int (*inc_fn_t)(int);
    inc_fn_t inc = (inc_fn_t)raw_ptr;

    /* 5. Call the specialized function */
    for (int v = 4; v < 8; ++v) {
        printf("inc(%d) is %d\n", v, inc(v));
    }

    /* 6. Cleanup */
    easyjit_function_destroy(fn);
    easyjit_context_destroy(ctx);

    return 0;
}
