/*
 * retconst_jit_probe.c
 *
 * Isolate the cost of executing JIT-generated constant-return functions versus
 * ordinary AOT functions with a similar dispatch shape.
 *
 * Modes:
 *   1 - AOT single function, direct/constant return
 *   2 - AOT multi-target function-pointer table, constant return
 *   3 - JIT single specialized function, final IR should be "ret const"
 *   4 - JIT multi-target function-pointer table, each final IR should be
 *       "ret const"
 */

#include <easy/attributes.h>
#include <easy/easyjit_c.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>

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

static volatile int g_last_mode = -1;
static volatile int g_last_value = -1;
static const char *g_last_stage = "start";

static void log_stage(int mode, int value, const char *stage) {
    g_last_mode = mode;
    g_last_value = value;
    g_last_stage = stage;
    printf("[probe] mode=%d value=%d stage=%s\n", mode, value, stage);
    fflush(stdout);
}

static void fatal_handler(int signo) {
    fprintf(stderr, "[probe] fatal signal=%d mode=%d value=%d stage=%s\n",
            signo, g_last_mode, g_last_value, g_last_stage);
    fflush(stderr);
    _Exit(128 + signo);
}

static void install_handlers(void) {
    signal(SIGSEGV, fatal_handler);
    signal(SIGBUS, fatal_handler);
    signal(SIGILL, fatal_handler);
    signal(SIGABRT, fatal_handler);
}

int EASY_JIT_EXPOSE eval_snapshot(const SmallConfig *cfg) {
    return cfg->value;
}

__attribute__((noinline))
static int aot_ret_15(void) { return 15; }

#define DEFINE_AOT_SLOT(N)                         \
    __attribute__((noinline))                     \
    static int aot_slot_##N(void) {               \
        return kValues[N];                        \
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

    for (i = 0; i < CONFIG_MAX; ++i) {
        printf("aot[%d]=%p\n", i, (void *)fn_ptrs[i]);
    }

    begin = clock();
    for (i = 0; i < LOOP_COUNT; ++i) {
        sum += fn_ptrs[i % CONFIG_MAX]();
    }
    end = clock();

    printf("mode=2 sum=%d\n", (int)sum);
    printf("steady-state: %.6f sec\n", elapsed_seconds(begin, end));
    return 0;
}

static int compile_jit_function(int value,
                                int mode,
                                easyjit_function_t *out_fn,
                                retconst_fn_t *out_ptr) {
    SmallConfig cfg;
    easyjit_context_t ctx = NULL;
    easyjit_error_t err;
    void *raw = NULL;

    cfg.value = value;

    log_stage(mode, value, "before context_create");
    err = easyjit_context_create(&ctx);
    if (err != EASYJIT_OK) {
        fprintf(stderr, "context_create failed: %s\n", easyjit_get_last_error());
        return 1;
    }
    log_stage(mode, value, "after context_create");

    log_stage(mode, value, "before set_snapshot");
    easyjit_context_set_snapshot(ctx, &cfg, sizeof(cfg));
    log_stage(mode, value, "after set_snapshot");

    log_stage(mode, value, "before set_opt_level");
    easyjit_context_set_opt_level(ctx, 3, 0);
    log_stage(mode, value, "after set_opt_level");

    log_stage(mode, value, "before compile");
    err = easyjit_compile((void *)eval_snapshot, ctx, out_fn);
    log_stage(mode, value, "after compile");

    log_stage(mode, value, "before context_destroy");
    easyjit_context_destroy(ctx);
    log_stage(mode, value, "after context_destroy");
    if (err != EASYJIT_OK) {
        fprintf(stderr, "compile failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    log_stage(mode, value, "before get_function_pointer");
    err = easyjit_get_function_pointer(*out_fn, &raw);
    log_stage(mode, value, "after get_function_pointer");
    if (err != EASYJIT_OK) {
        fprintf(stderr, "get_function_pointer failed: %s\n", easyjit_get_last_error());
        easyjit_function_destroy(*out_fn);
        *out_fn = NULL;
        return 1;
    }

    *out_ptr = (retconst_fn_t)raw;
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
    if (compile_jit_function(15, 3, &fn, &ptr) != 0) {
        return 1;
    }
    warm_end = clock();

    printf("jit_single=%p\n", (void *)ptr);
    fflush(stdout);

    log_stage(3, 15, "before execute");
    begin = clock();
    for (i = 0; i < LOOP_COUNT; ++i) {
        sum += ptr();
    }
    end = clock();
    log_stage(3, 15, "after execute");

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
        if (compile_jit_function(kValues[i], 4, &handles[i], &fn_ptrs[i]) != 0) {
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

    for (i = 0; i < CONFIG_MAX; ++i) {
        printf("jit[%d]=%p value=%d\n", i, (void *)fn_ptrs[i], kValues[i]);
    }
    fflush(stdout);

    log_stage(4, 0, "before execute");
    begin = clock();
    for (i = 0; i < LOOP_COUNT; ++i) {
        sum += fn_ptrs[i % CONFIG_MAX]();
    }
    end = clock();
    log_stage(4, 0, "after execute");

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

    install_handlers();

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
