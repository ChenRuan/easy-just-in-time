/*
 * retconst_jit_probe.c
 *
 * Small-struct probe that mirrors config_process_easyjit.c as closely as
 * possible, but reduces the JIT body to a constant-return-style specialization.
 *
 * Modes:
 *   1 - AOT baseline direct call
 *   2 - AOT multi-target function-pointer dispatch
 *   3 - JIT all keys, same warm-up/steady-state structure as config_process_easyjit
 *   4 - JIT all keys, but steady-state always uses key 0
 */

#include <easy/attributes.h>
#include <easy/easyjit_c.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONFIG_MAX 12
#define GROUP_MAX 4
#ifndef LOOP_COUNT
#define LOOP_COUNT 200000000
#endif
#define KEY_STRIDE 13
#define MAKE_KEY(cfg, grp) ((cfg) * KEY_STRIDE + (grp))
#define NUM_KEYS CONFIG_MAX

typedef struct {
    int value;
} SmallConfig;

typedef struct {
    int config_index;
    int group_index;
    int key;
} KeyInfo;

typedef int (*jit_fn_t)(void);

static SmallConfig *g_configs = NULL;
static int g_groups[GROUP_MAX];
static KeyInfo g_keys[NUM_KEYS];

static void init_configs(void) {
    int i;

    g_configs = (SmallConfig *)malloc(sizeof(SmallConfig) * CONFIG_MAX);
    if (!g_configs) {
        fprintf(stderr, "malloc failed\n");
        exit(1);
    }
    memset(g_configs, 0, sizeof(SmallConfig) * CONFIG_MAX);
    for (i = 0; i < CONFIG_MAX; ++i) {
        g_configs[i].value = i;
    }
}

static void init_groups(void) {
    int i;

    for (i = 0; i < GROUP_MAX; ++i) {
        g_groups[i] = 15 + i * 8;
    }
}

static void prepare_keys(void) {
    int i;

    for (i = 0; i < NUM_KEYS; ++i) {
        g_keys[i].config_index = i;
        g_keys[i].group_index = i % GROUP_MAX;
        g_keys[i].key = MAKE_KEY(i, i % GROUP_MAX);
    }
}

static inline SmallConfig *get_config(int index) {
    return &g_configs[index];
}

static void update_config(int config_index, int group_index) {
    SmallConfig *cfg = get_config(config_index);
    cfg->value = g_groups[group_index];
}

int EASY_JIT_EXPOSE process_small_jit(SmallConfig *cfg) {
    return cfg->value;
}

__attribute__((noinline))
static int process_small_base(int config_index) {
    SmallConfig *cfg = get_config(config_index);
    return cfg->value;
}

#define DEFINE_SLOT_FN(N)                                \
    __attribute__((noinline))                            \
    static int process_small_slot_##N(void) {            \
        return process_small_base(N);                    \
    }

DEFINE_SLOT_FN(0)
DEFINE_SLOT_FN(1)
DEFINE_SLOT_FN(2)
DEFINE_SLOT_FN(3)
DEFINE_SLOT_FN(4)
DEFINE_SLOT_FN(5)
DEFINE_SLOT_FN(6)
DEFINE_SLOT_FN(7)
DEFINE_SLOT_FN(8)
DEFINE_SLOT_FN(9)
DEFINE_SLOT_FN(10)
DEFINE_SLOT_FN(11)

static void init_slot_ptrs(jit_fn_t fn_ptrs[CONFIG_MAX]) {
    fn_ptrs[0] = process_small_slot_0;
    fn_ptrs[1] = process_small_slot_1;
    fn_ptrs[2] = process_small_slot_2;
    fn_ptrs[3] = process_small_slot_3;
    fn_ptrs[4] = process_small_slot_4;
    fn_ptrs[5] = process_small_slot_5;
    fn_ptrs[6] = process_small_slot_6;
    fn_ptrs[7] = process_small_slot_7;
    fn_ptrs[8] = process_small_slot_8;
    fn_ptrs[9] = process_small_slot_9;
    fn_ptrs[10] = process_small_slot_10;
    fn_ptrs[11] = process_small_slot_11;
}

static double elapsed_seconds(clock_t begin, clock_t end) {
    return (double)(end - begin) / CLOCKS_PER_SEC;
}

static int run_aot_direct(void) {
    clock_t begin;
    clock_t end;
    volatile int sum = 0;
    int i;

    begin = clock();
    for (i = 0; i < LOOP_COUNT; ++i) {
        int config_index = i % CONFIG_MAX;
        int group_index = i % GROUP_MAX;
        update_config(config_index, group_index);
        sum += process_small_base(config_index);
    }
    end = clock();

    printf("mode=1 sum=%d\n", (int)sum);
    printf("steady-state: %.6f sec\n", elapsed_seconds(begin, end));
    return 0;
}

static int run_aot_multifptr(void) {
    jit_fn_t fn_ptrs[CONFIG_MAX];
    clock_t begin;
    clock_t end;
    volatile int sum = 0;
    int i;

    init_slot_ptrs(fn_ptrs);

    begin = clock();
    for (i = 0; i < LOOP_COUNT; ++i) {
        int config_index = i % CONFIG_MAX;
        int group_index = i % GROUP_MAX;
        update_config(config_index, group_index);
        sum += fn_ptrs[config_index]();
    }
    end = clock();

    printf("mode=2 sum=%d\n", (int)sum);
    printf("steady-state: %.6f sec\n", elapsed_seconds(begin, end));
    return 0;
}

static int run_jit_like_original(int single_key_mode) {
    easyjit_function_t handles[NUM_KEYS];
    jit_fn_t fn_ptrs[NUM_KEYS];
    clock_t warm_begin;
    clock_t warm_end;
    clock_t begin;
    clock_t end;
    volatile int sum = 0;
    int i;

    memset(handles, 0, sizeof(handles));
    memset(fn_ptrs, 0, sizeof(fn_ptrs));

    warm_begin = clock();
    for (i = 0; i < NUM_KEYS; ++i) {
        SmallConfig *cfg;
        easyjit_context_t ctx = NULL;
        easyjit_function_t fn = NULL;
        void *raw = NULL;

        update_config(g_keys[i].config_index, g_keys[i].group_index);
        cfg = get_config(g_keys[i].config_index);

        easyjit_context_create(&ctx);
        easyjit_context_set_snapshot(ctx, cfg, sizeof(SmallConfig));
        easyjit_context_set_opt_level(ctx, 3, 0);

        if (easyjit_compile((void *)process_small_jit, ctx, &fn) != EASYJIT_OK) {
            fprintf(stderr, "compile failed for key=%d: %s\n",
                    g_keys[i].key, easyjit_get_last_error());
            easyjit_context_destroy(ctx);
            free(g_configs);
            return 1;
        }
        easyjit_context_destroy(ctx);

        if (easyjit_get_function_pointer(fn, &raw) != EASYJIT_OK) {
            fprintf(stderr, "get_function_pointer failed: %s\n",
                    easyjit_get_last_error());
            easyjit_function_destroy(fn);
            free(g_configs);
            return 1;
        }

        handles[i] = fn;
        fn_ptrs[i] = (jit_fn_t)raw;
    }
    warm_end = clock();

    begin = clock();
    for (i = 0; i < LOOP_COUNT; ++i) {
        int config_index = single_key_mode ? 0 : (i % CONFIG_MAX);
        int group_index = single_key_mode ? 0 : (i % GROUP_MAX);
        update_config(config_index, group_index);
        sum += fn_ptrs[config_index]();
    }
    end = clock();

    printf("mode=%d sum=%d\n", single_key_mode ? 4 : 3, (int)sum);
    printf("warm-up: %.6f sec\n", elapsed_seconds(warm_begin, warm_end));
    printf("steady-state: %.6f sec\n", elapsed_seconds(begin, end));

    for (i = 0; i < NUM_KEYS; ++i) {
        easyjit_function_destroy(handles[i]);
    }
    return 0;
}

static void usage(const char *argv0) {
    printf("usage: %s <mode>\n", argv0);
    printf("  1 - aot direct baseline\n");
    printf("  2 - aot multi-target fnptr baseline\n");
    printf("  3 - jit all keys (mirror config_process_easyjit)\n");
    printf("  4 - jit all keys, steady-state key 0 only\n");
}

int main(int argc, char **argv) {
    int mode;

    printf("============================================================\n");
    printf("  retconst_jit_probe (config_process_easyjit-style)\n");
    printf("  sizeof(SmallConfig) = %zu bytes\n", sizeof(SmallConfig));
    printf("  LOOP_COUNT = %d   GROUP_MAX = %d   CONFIG_MAX = %d\n",
           LOOP_COUNT, GROUP_MAX, CONFIG_MAX);
    printf("============================================================\n\n");

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    init_configs();
    init_groups();
    prepare_keys();

    mode = atoi(argv[1]);
    switch (mode) {
    case 1:
        return run_aot_direct();
    case 2:
        return run_aot_multifptr();
    case 3:
        return run_jit_like_original(0);
    case 4:
        return run_jit_like_original(1);
    default:
        usage(argv[0]);
        return 1;
    }
}
