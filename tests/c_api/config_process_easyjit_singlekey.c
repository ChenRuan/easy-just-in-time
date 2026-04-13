/*
 * config_process_easyjit_singlekey.c
 *
 * Single-key-use EasyJIT benchmark used to isolate the cost of executing one
 * repeatedly-called JITed function from the cost of switching among many JIT
 * entry points, while keeping warm-up behavior close to
 * config_process_easyjit.c.
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

static void prepare_keys(void) {
    int i;

    for (i = 0; i < NUM_KEYS; ++i) {
        g_keys[i].config_index = i;
        g_keys[i].group_index = i % GROUP_MAX;
        g_keys[i].key = MAKE_KEY(i, i % GROUP_MAX);
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

int main(int argc, char **argv) {
    typedef int (*jit_fn_t)(void);
    easyjit_function_t handles[NUM_KEYS];
    jit_fn_t fn_ptrs[NUM_KEYS];
    const int selected_index = 0;
    const char *dump_ir = NULL;
    char dump_path[256];
    clock_t warmup_begin;
    clock_t warmup_end;
    clock_t start;
    clock_t end;
    int sum = 0;
    int i;

    printf("============================================================\n");
    printf("  EasyJIT Single-Key-Use Snapshot + Raw Pointer Example\n");
    printf("  sizeof(ConfigRecord) = %zu bytes\n", sizeof(ConfigRecord));
    printf("  LOOP_COUNT = %d   SELECTED_INDEX = %d   SELECTED_KEY = %d\n",
           LOOP_COUNT, selected_index, MAKE_KEY(selected_index, selected_index % GROUP_MAX));
    printf("============================================================\n\n");

    init_configs();
    init_groups();
    prepare_keys();
    memset(handles, 0, sizeof(handles));
    memset(fn_ptrs, 0, sizeof(fn_ptrs));

    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        dump_ir = argv[1];
    } else {
        dump_ir = getenv("EASYJIT_DUMP_IR");
    }

    warmup_begin = clock();
    for (i = 0; i < NUM_KEYS; ++i) {
        easyjit_context_t ctx = NULL;
        void *raw = NULL;

        update_config(g_keys[i].config_index, g_keys[i].group_index);

        if (easyjit_context_create(&ctx) != EASYJIT_OK) {
            fprintf(stderr, "context_create failed: %s\n", easyjit_get_last_error());
            free(g_configs);
            return 1;
        }

        if (easyjit_context_set_snapshot(ctx, get_config(g_keys[i].config_index), sizeof(ConfigRecord)) != EASYJIT_OK) {
            fprintf(stderr, "set_snapshot failed for key=%d: %s\n",
                    g_keys[i].key, easyjit_get_last_error());
            easyjit_context_destroy(ctx);
            free(g_configs);
            return 1;
        }

        if (easyjit_context_set_opt_level(ctx, 3, 0) != EASYJIT_OK) {
            fprintf(stderr, "set_opt_level failed for key=%d: %s\n",
                    g_keys[i].key, easyjit_get_last_error());
            easyjit_context_destroy(ctx);
            free(g_configs);
            return 1;
        }

        if (dump_ir && dump_ir[0] != '\0' && i == selected_index) {
            snprintf(dump_path, sizeof(dump_path), "%s.key%d.ll", dump_ir, g_keys[i].key);
            if (easyjit_context_set_dump_ir(ctx, dump_path) != EASYJIT_OK) {
                fprintf(stderr, "set_dump_ir failed for key=%d: %s\n",
                        g_keys[i].key, easyjit_get_last_error());
                easyjit_context_destroy(ctx);
                free(g_configs);
                return 1;
            }
            printf("IR dump path = %s\n", dump_path);
        }

        if (easyjit_compile((void *)process_config_jit, ctx, &handles[i]) != EASYJIT_OK) {
            fprintf(stderr, "compile failed for key=%d: %s\n",
                    g_keys[i].key, easyjit_get_last_error());
            easyjit_context_destroy(ctx);
            free(g_configs);
            return 1;
        }
        easyjit_context_destroy(ctx);

        if (easyjit_get_function_pointer(handles[i], &raw) != EASYJIT_OK) {
            fprintf(stderr, "get_function_pointer failed for key=%d: %s\n",
                    g_keys[i].key, easyjit_get_last_error());
            easyjit_function_destroy(handles[i]);
            free(g_configs);
            return 1;
        }
        fn_ptrs[i] = (jit_fn_t)raw;
    }

    warmup_end = clock();
    printf("Warm-up (JIT all %d keys): %.4f ms\n",
           NUM_KEYS,
           (double)(warmup_end - warmup_begin) * 1000.0 / CLOCKS_PER_SEC);

    start = clock();
    for (i = 0; i < LOOP_COUNT; ++i) {
        update_config(g_keys[selected_index].config_index, g_keys[selected_index].group_index);
        sum += fn_ptrs[selected_index]();
    }
    end = clock();

    printf("sum=%d\n", sum);
    printf("steady-state: %.6f sec\n", (double)(end - start) / CLOCKS_PER_SEC);
    printf("total wall:   %.6f sec\n\n", (double)(end - warmup_begin) / CLOCKS_PER_SEC);

    for (i = 0; i < NUM_KEYS; ++i) {
        if (handles[i]) {
            easyjit_function_destroy(handles[i]);
        }
    }
    free(g_configs);
    return 0;
}
