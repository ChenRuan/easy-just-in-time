/*
 * config_process_easyjit_keyprofile.c
 *
 * Variant of config_process_easyjit.c that profiles each JIT-specialized key
 * individually after the normal warm-up path.
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
    int nested_id;
    bool nested_enabled;
    int nested_values[8];
} NestedConfig;

typedef struct {
    int config_id;
    bool enabled;
    int priority;
    int weight;
    bool flag_a;
    bool flag_b;
    int values[16];
    int counters[8];
    NestedConfig nested;
    int reserved[32];
} ConfigRecord;

typedef struct {
    int base_value;
    bool feature_enabled;
    int offset;
    int table[16];
} GroupConfig;

typedef struct {
    int config_index;
    int group_index;
    int key;
} KeyInfo;

typedef int (*jit_fn_t)(void);

static ConfigRecord *g_configs = NULL;
static GroupConfig g_groups[GROUP_MAX];
static KeyInfo g_keys[NUM_KEYS];

static void init_configs(void) {
    int i;

    g_configs = (ConfigRecord *)malloc(sizeof(ConfigRecord) * CONFIG_MAX);
    if (!g_configs) {
        fprintf(stderr, "malloc failed\n");
        exit(1);
    }
    memset(g_configs, 0, sizeof(ConfigRecord) * CONFIG_MAX);
    for (i = 0; i < CONFIG_MAX; ++i) {
        g_configs[i].config_id = i;
        g_configs[i].enabled = true;
        g_configs[i].priority = i % 3;
        g_configs[i].weight = i * 2;
    }
}

static void init_groups(void) {
    int i;
    int j;

    for (i = 0; i < GROUP_MAX; ++i) {
        g_groups[i].base_value = 100 + i * 10;
        g_groups[i].feature_enabled = (i % 2) == 0;
        g_groups[i].offset = i * 5;
        for (j = 0; j < 16; ++j) {
            g_groups[i].table[j] = i * j;
        }
    }
}

static inline ConfigRecord *get_config(int index) {
    return &g_configs[index];
}

static void update_config(int config_index, int group_index) {
    ConfigRecord *cfg = get_config(config_index);
    GroupConfig *grp = &g_groups[group_index];

    cfg->priority = grp->base_value;
    cfg->enabled = grp->feature_enabled;
    cfg->weight = grp->offset;
    cfg->values[0] = grp->table[0];
    cfg->values[1] = grp->table[1];
    cfg->values[2] = grp->table[2];
    cfg->values[3] = grp->table[3];
    cfg->nested.nested_id = grp->table[4];
    cfg->nested.nested_enabled = (grp->table[5] % 2) != 0;
    cfg->nested.nested_values[0] = grp->table[6];
}

int EASY_JIT_EXPOSE process_config_jit(ConfigRecord *cfg) {
    int result = 0;

    if (cfg->enabled) {
        result += cfg->priority;
        result += cfg->values[0];
    } else {
        result -= cfg->weight;
    }

    if (cfg->flag_a) {
        result += cfg->counters[0];
    } else {
        result += cfg->nested.nested_id;
    }

    if (cfg->nested.nested_enabled) {
        result += cfg->nested.nested_values[0];
    } else {
        result += cfg->reserved[0];
    }

    return result;
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
    easyjit_function_t handles[NUM_KEYS];
    jit_fn_t fn_ptrs[NUM_KEYS];
    const char *dump_ir = NULL;
    int sum = 0;
    int i;
    clock_t warmup_begin;
    clock_t warmup_end;
    clock_t start;
    clock_t end;
    const int per_key_loops = LOOP_COUNT / CONFIG_MAX;

    printf("============================================================\n");
    printf("  EasyJIT C Snapshot + Raw Pointer Key Profile Example\n");
    printf("  sizeof(ConfigRecord) = %zu bytes\n", sizeof(ConfigRecord));
    printf("  LOOP_COUNT = %d   GROUP_MAX = %d   CONFIG_MAX = %d\n",
           LOOP_COUNT, GROUP_MAX, CONFIG_MAX);
    printf("  PER_KEY_LOOPS = %d\n", per_key_loops);
    printf("============================================================\n\n");

    init_configs();
    init_groups();
    prepare_keys();

    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        dump_ir = argv[1];
    } else {
        dump_ir = getenv("EASYJIT_DUMP_IR");
    }

    if (dump_ir && dump_ir[0] != '\0') {
        printf("  IR dump prefix = %s\n", dump_ir);
    }

    warmup_begin = clock();
    for (i = 0; i < NUM_KEYS; ++i) {
        ConfigRecord *cfg;
        easyjit_context_t ctx = NULL;
        easyjit_function_t fn = NULL;
        void *raw = NULL;
        char dump_path[256];

        update_config(g_keys[i].config_index, g_keys[i].group_index);
        cfg = get_config(g_keys[i].config_index);

        easyjit_context_create(&ctx);
        easyjit_context_set_snapshot(ctx, cfg, sizeof(ConfigRecord));
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
        int config_index = i % CONFIG_MAX;
        int group_index = i % GROUP_MAX;
        update_config(config_index, group_index);
        sum += fn_ptrs[config_index]();
    }
    end = clock();

    printf("sum=%d\n", sum);
    printf("steady-state: %.6f sec\n", (double)(end - start) / CLOCKS_PER_SEC);
    printf("total wall:   %.6f sec\n\n", (double)(end - warmup_begin) / CLOCKS_PER_SEC);

    printf("Per-key profile:\n");
    for (i = 0; i < NUM_KEYS; ++i) {
        volatile int key_sum = 0;
        clock_t key_begin;
        clock_t key_end;
        int iter;

        printf("  key=%d cfg=%d grp=%d addr=%p\n",
               g_keys[i].key,
               g_keys[i].config_index,
               g_keys[i].group_index,
               (void *)fn_ptrs[i]);

        key_begin = clock();
        for (iter = 0; iter < per_key_loops; ++iter) {
            update_config(g_keys[i].config_index, iter % GROUP_MAX);
            key_sum += fn_ptrs[i]();
        }
        key_end = clock();

        printf("    key_sum=%d elapsed=%.6f sec\n",
               (int)key_sum,
               (double)(key_end - key_begin) / CLOCKS_PER_SEC);
    }

    for (i = 0; i < NUM_KEYS; ++i) {
        easyjit_function_destroy(handles[i]);
    }
    free(g_configs);

    return 0;
}
