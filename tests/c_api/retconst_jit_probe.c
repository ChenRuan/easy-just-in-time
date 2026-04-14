/*
 * retconst_jit_probe.c
 *
 * Minimal probe for constant-return specialization.
 *
 * Keep this translation unit intentionally small so the embedded bitcode fed to
 * EasyJIT looks more like a focused benchmark than a debugging harness.
 *
 * Modes:
 *   1 - AOT single constant-return function
 *   2 - AOT multi-target function-pointer table
 *   3 - JIT single specialized constant-return function
 *   4 - JIT multi-target specialized function-pointer table
 */

#include <easy/attributes.h>
#include <easy/easyjit_c.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONFIG_MAX 12
#ifndef LOOP_COUNT
#define LOOP_COUNT 200000000
#endif

typedef struct {
    int value;
} SmallConfig;

typedef int (*retconst_fn_t)(void);

static const int kValues[CONFIG_MAX] = {
    15, 23, 31, 7, 42, 5, 19, 27, 33, 9, 11, 17
};

int EASY_JIT_EXPOSE eval_snapshot(const SmallConfig *cfg) {
    return cfg->value;
}

__attribute__((noinline))
static int aot_ret_15(void) {
    return 15;
}

#define DEFINE_AOT_SLOT(N)                          \
    __attribute__((noinline))                      \
    static int aot_slot_##N(void) {                \
        return kValues[N];                         \
    }

DEFINE_AOT_SLOT(0)
DEFINE_AOT_SLOT(1)
DEFINE_AOT_SLOT(2)
DEFINE_AOT_SLOT(3)
DEFINE_AOT_SLOT(4)
DEFINE_AOT_SLOT(5)
DEFINE_AOT_SLOT(6)
DEFINE_AOT_SLOT(7)
DEFINE_AOT_SLOT(8)
DEFINE_AOT_SLOT(9)
DEFINE_AOT_SLOT(10)
DEFINE_AOT_SLOT(11)

static void init_aot_table(retconst_fn_t fn_ptrs[CONFIG_MAX]) {
    fn_ptrs[0] = aot_slot_0;
    fn_ptrs[1] = aot_slot_1;
    fn_ptrs[2] = aot_slot_2;
    fn_ptrs[3] = aot_slot_3;
    fn_ptrs[4] = aot_slot_4;
    fn_ptrs[5] = aot_slot_5;
    fn_ptrs[6] = aot_slot_6;
    fn_ptrs[7] = aot_slot_7;
    fn_ptrs[8] = aot_slot_8;
    fn_ptrs[9] = aot_slot_9;
    fn_ptrs[10] = aot_slot_10;
    fn_ptrs[11] = aot_slot_11;
}

static double elapsed_seconds(clock_t begin, clock_t end) {
    return (double)(end - begin) / CLOCKS_PER_SEC;
}

static int compile_jit_function(int value,
                                easyjit_function_t *out_fn,
                                retconst_fn_t *out_ptr) {
    SmallConfig cfg;
    easyjit_context_t ctx = NULL;
    easyjit_error_t err;
    void *raw = NULL;

    cfg.value = value;

    err = easyjit_context_create(&ctx);
    if (err != EASYJIT_OK) {
        fprintf(stderr, "context_create failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    easyjit_context_set_snapshot(ctx, &cfg, sizeof(cfg));
    easyjit_context_set_opt_level(ctx, 3, 0);

    err = easyjit_compile((void *)eval_snapshot, ctx, out_fn);
    easyjit_context_destroy(ctx);
    if (err != EASYJIT_OK) {
        fprintf(stderr, "compile failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    err = easyjit_get_function_pointer(*out_fn, &raw);
    if (err != EASYJIT_OK) {
        fprintf(stderr, "get_function_pointer failed: %s\n", easyjit_get_last_error());
        easyjit_function_destroy(*out_fn);
        *out_fn = NULL;
        return 1;
    }

    *out_ptr = (retconst_fn_t)raw;
    return 0;
}

static int run_aot_single(void) {
    clock_t begin;
    clock_t end;
    volatile int sum = 0;
    int i;

    begin = clock();
    for (i = 0; i < LOOP_COUNT; ++i) {
        sum += aot_ret_15();
    }
    end = clock();

    printf("mode=1 sum=%d\n", (int)sum);
    printf("steady-state: %.6f sec\n", elapsed_seconds(begin, end));
    return 0;
}

static int run_aot_multi(void) {
    retconst_fn_t fn_ptrs[CONFIG_MAX];
    clock_t begin;
    clock_t end;
    volatile int sum = 0;
    int i;

    init_aot_table(fn_ptrs);

    begin = clock();
    for (i = 0; i < LOOP_COUNT; ++i) {
        sum += fn_ptrs[i % CONFIG_MAX]();
    }
    end = clock();

    printf("mode=2 sum=%d\n", (int)sum);
    printf("steady-state: %.6f sec\n", elapsed_seconds(begin, end));
    return 0;
}

static int run_jit_single(void) {
    easyjit_function_t fn = NULL;
    retconst_fn_t ptr = NULL;
    clock_t warm_begin;
    clock_t warm_end;
    clock_t begin;
    clock_t end;
    volatile int sum = 0;
    int i;

    warm_begin = clock();
    if (compile_jit_function(15, &fn, &ptr) != 0) {
        return 1;
    }
    warm_end = clock();

    begin = clock();
    for (i = 0; i < LOOP_COUNT; ++i) {
        sum += ptr();
    }
    end = clock();

    printf("mode=3 sum=%d\n", (int)sum);
    printf("warm-up: %.6f sec\n", elapsed_seconds(warm_begin, warm_end));
    printf("steady-state: %.6f sec\n", elapsed_seconds(begin, end));

    easyjit_function_destroy(fn);
    return 0;
}

static int run_jit_multi(void) {
    easyjit_function_t handles[CONFIG_MAX];
    retconst_fn_t fn_ptrs[CONFIG_MAX];
    clock_t warm_begin;
    clock_t warm_end;
    clock_t begin;
    clock_t end;
    volatile int sum = 0;
    int i;

    memset(handles, 0, sizeof(handles));
    memset(fn_ptrs, 0, sizeof(fn_ptrs));

    warm_begin = clock();
    for (i = 0; i < CONFIG_MAX; ++i) {
        if (compile_jit_function(kValues[i], &handles[i], &fn_ptrs[i]) != 0) {
            int j;
            for (j = 0; j < CONFIG_MAX; ++j) {
                if (handles[j]) {
                    easyjit_function_destroy(handles[j]);
                }
            }
            return 1;
        }
    }
    warm_end = clock();

    begin = clock();
    for (i = 0; i < LOOP_COUNT; ++i) {
        sum += fn_ptrs[i % CONFIG_MAX]();
    }
    end = clock();

    printf("mode=4 sum=%d\n", (int)sum);
    printf("warm-up: %.6f sec\n", elapsed_seconds(warm_begin, warm_end));
    printf("steady-state: %.6f sec\n", elapsed_seconds(begin, end));

    for (i = 0; i < CONFIG_MAX; ++i) {
        easyjit_function_destroy(handles[i]);
    }
    return 0;
}

static void usage(const char *argv0) {
    printf("usage: %s <mode>\n", argv0);
    printf("  1 - aot single ret-const\n");
    printf("  2 - aot multi-target fnptr ret-const\n");
    printf("  3 - jit single ret-const\n");
    printf("  4 - jit multi-target fnptr ret-const\n");
}

int main(int argc, char **argv) {
    int mode;

    printf("============================================================\n");
    printf("  retconst_jit_probe\n");
    printf("  LOOP_COUNT = %d   CONFIG_MAX = %d\n", LOOP_COUNT, CONFIG_MAX);
    printf("============================================================\n");

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    mode = atoi(argv[1]);
    switch (mode) {
    case 1:
        return run_aot_single();
    case 2:
        return run_aot_multi();
    case 3:
        return run_jit_single();
    case 4:
        return run_jit_multi();
    default:
        usage(argv[0]);
        return 1;
    }
}
