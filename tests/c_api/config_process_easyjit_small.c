/*
 * config_process_easyjit_small.c
 *
 * Near-copy of config_process_easyjit.c with the same warm-up / dispatch
 * structure, but using a tiny snapshot payload and a trivial JIT body.
 *
 * Usage:
 *   ./config_process_easyjit_small
 *   ./config_process_easyjit_small single
 *   ./config_process_easyjit_small <dump_ir_prefix>
 *   ./config_process_easyjit_small single <dump_ir_prefix>
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
    int base_value;
} GroupConfig;

typedef struct {
    int config_index;
    int group_index;
    int key;
} KeyInfo;

static SmallConfig *g_configs = NULL;
static GroupConfig g_groups[GROUP_MAX];
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
        g_groups[i].base_value = 15 + i * 8;
    }
}

static inline SmallConfig *get_config(int index) {
    return &g_configs[index];
}

static void update_config(int config_index, int group_index) {
    SmallConfig *cfg = get_config(config_index);
    GroupConfig *grp = &g_groups[group_index];
    cfg->value = grp->base_value;
}

int EASY_JIT_EXPOSE process_config_jit(SmallConfig *cfg) {
    return cfg->value;
}

static void prepare_keys(void) {
    int i;

    for (i = 0; i < NUM_KEYS; ++i) {
        g_keys[i].config_index = i;
        g_keys[i].group_index = i % GROUP_MAX;
        g_keys[i].key = MAKE_KEY(i, i % GROUP_MAX);
    }
}

int main(int argc, char **argv) {
    typedef int (*jit_fn_t)(void);
    easyjit_function_t handles[NUM_KEYS];
    jit_fn_t fn_ptrs[NUM_KEYS];
    const char *dump_ir = NULL;
    int single_key_mode = 0;
    int sum = 0;
    int i;
    clock_t warmup_begin;
    clock_t warmup_end;
    clock_t start;
    clock_t end;

    printf("============================================================\n");
    printf("  EasyJIT Small Snapshot + Raw Pointer Example\n");
    printf("  sizeof(SmallConfig) = %zu bytes\n", sizeof(SmallConfig));
    printf("  LOOP_COUNT = %d   GROUP_MAX = %d   CONFIG_MAX = %d\n",
           LOOP_COUNT, GROUP_MAX, CONFIG_MAX);
    printf("============================================================\n\n");

    init_configs();
    init_groups();
    prepare_keys();

    if (argc > 1 && strcmp(argv[1], "single") == 0) {
        single_key_mode = 1;
        if (argc > 2 && argv[2] && argv[2][0] != '\0') {
            dump_ir = argv[2];
        }
    } else if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        dump_ir = argv[1];
    } else {
        dump_ir = getenv("EASYJIT_DUMP_IR");
    }

    if (single_key_mode) {
        printf("  steady-state mode = single key\n");
    }
    if (dump_ir && dump_ir[0] != '\0') {
        printf("  IR dump prefix = %s\n", dump_ir);
    }

    warmup_begin = clock();
    for (i = 0; i < NUM_KEYS; ++i) {
        SmallConfig *cfg;
        easyjit_context_t ctx = NULL;
        easyjit_function_t fn = NULL;
        void *raw = NULL;
        char dump_path[256];

        update_config(g_keys[i].config_index, g_keys[i].group_index);
        cfg = get_config(g_keys[i].config_index);

        easyjit_context_create(&ctx);
        easyjit_context_set_snapshot(ctx, cfg, sizeof(SmallConfig));
        easyjit_context_set_opt_level(ctx, 3, 0);
        if (dump_ir && dump_ir[0] != '\0') {
            snprintf(dump_path, sizeof(dump_path), "%s.key%d.ll", dump_ir, g_keys[i].key);
            if (easyjit_context_set_dump_ir(ctx, dump_path) != EASYJIT_OK) {
                fprintf(stderr, "set_dump_ir failed for key=%d: %s\n",
                        g_keys[i].key, easyjit_get_last_error());
                easyjit_context_destroy(ctx);
                free(g_configs);
                return 1;
            }
        }

        if (easyjit_compile((void *)process_config_jit, ctx, &fn) != EASYJIT_OK) {
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
    warmup_end = clock();

    printf("Warm-up (JIT all %d keys): %.4f ms\n",
           NUM_KEYS,
           (double)(warmup_end - warmup_begin) * 1000.0 / CLOCKS_PER_SEC);

    start = clock();
    for (i = 0; i < LOOP_COUNT; ++i) {
        int config_index = single_key_mode ? 0 : (i % CONFIG_MAX);
        int group_index = single_key_mode ? 0 : (i % GROUP_MAX);
        update_config(config_index, group_index);
        sum += fn_ptrs[config_index]();
    }
    end = clock();

    printf("sum=%d\n", sum);
    printf("steady-state: %.6f sec\n", (double)(end - start) / CLOCKS_PER_SEC);
    printf("total wall:   %.6f sec\n\n", (double)(end - warmup_begin) / CLOCKS_PER_SEC);

    for (i = 0; i < NUM_KEYS; ++i) {
        easyjit_function_destroy(handles[i]);
    }
    free(g_configs);

    return 0;
}
