/**
 * @file cache_example.c
 * @brief Demonstrates the easyjit C API cache for reusing compiled functions.
 *
 * This example shows how to use easyjit_cache_* to avoid recompiling the
 * same specialization.  This is the pattern that wireless/ examples would
 * use when the specialization key (e.g. number of antennas) can change
 * at runtime but is often repeated.
 */

#include <easy/easyjit_c.h>
#include <easy/attributes.h>
#include <stdio.h>
#include <stdlib.h>

/* A simple "kernel" function: multiply-accumulate with a constant factor.
 * EASY_JIT_EXPOSE tells the pass to embed bitcode for runtime specialization.
 */
int EASY_JIT_EXPOSE mac(int x, int factor) {
    return x * factor;
}

int main(void) {
    easyjit_error_t err;

    /* Create a cache */
    easyjit_cache_t cache = NULL;
    err = easyjit_cache_create(&cache);
    if (err != EASYJIT_OK) {
        fprintf(stderr, "cache_create failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    typedef int (*mac_specialized_t)(int);

    /* Test with different "factor" values, some repeated */
    int factors[] = { 3, 5, 3, 7, 5, 3 };
    int n = sizeof(factors) / sizeof(factors[0]);

    for (int i = 0; i < n; i++) {
        int factor = factors[i];
        void* fptr = NULL;

        /* Check if we already compiled for this factor */
        int hit = 0;
        easyjit_cache_has(cache, (int64_t)factor, &hit);

        if (!hit) {
            /* Build context: forward x, specialize factor */
            easyjit_context_t ctx = NULL;
            easyjit_context_create(&ctx);
            easyjit_context_set_forward(ctx, 0);
            easyjit_context_set_int(ctx, (int64_t)factor);

            err = easyjit_cache_get_or_compile(cache, (int64_t)factor,
                                               (void*)mac, ctx, &fptr);
            easyjit_context_destroy(ctx);

            if (err != EASYJIT_OK) {
                fprintf(stderr, "cache_get_or_compile failed for factor=%d: %s\n",
                        factor, easyjit_get_last_error());
                easyjit_cache_destroy(cache);
                return 1;
            }
            printf("[compiled] ");
        } else {
            err = easyjit_cache_get_or_compile(cache, (int64_t)factor,
                                               NULL, NULL, &fptr);
            if (err != EASYJIT_OK) {
                fprintf(stderr, "cache hit failed: %s\n", easyjit_get_last_error());
                easyjit_cache_destroy(cache);
                return 1;
            }
            printf("[cached]   ");
        }

        mac_specialized_t spec_mac = (mac_specialized_t)fptr;
        printf("mac(10, %d) = %d\n", factor, spec_mac(10));
    }

    easyjit_cache_destroy(cache);
    return 0;
}
