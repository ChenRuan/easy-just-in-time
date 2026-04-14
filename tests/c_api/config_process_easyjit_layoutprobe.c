/*
 * config_process_easyjit_layoutprobe.c
 *
 * Probe whether JIT function address layout / call order materially affects
 * steady-state performance on constrained targets.
 *
 * Modes:
 *   0 (default): original key order
 *   1: address-sorted call order
 *
 * This test compiles the same 12 JIT specializations as config_process_easyjit
 * and prints their raw addresses plus adjacent deltas.
 */

#include <easy/attributes.h>
#include <easy/easyjit_c.h>
#include <stdbool.h>
#include <stdint.h>
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

typedef struct {
    int original_index;
    int key;
    uintptr_t addr;
} AddrInfo;

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

static int cmp_addrinfo(const void *lhs, const void *rhs) {
    const AddrInfo *a = (const AddrInfo *)lhs;
    const AddrInfo *b = (const AddrInfo *)rhs;
    if (a->addr < b->addr) return -1;
    if (a->addr > b->addr) return 1;
    return 0;
}

int main(int argc, char **argv) {
    typedef int (*jit_fn_t)(void);
    easyjit_function_t handles[NUM_KEYS];
    jit_fn_t fn_ptrs[NUM_KEYS];
    AddrInfo addrs[NUM_KEYS];
    int call_order[NUM_KEYS];
    int order_mode = 0;
    clock_t warmup_begin;
    clock_t warmup_end;
    clock_t start;
    clock_t end;
    int sum = 0;
    int i;

    if (argc > 1) {
        order_mode = atoi(argv[1]);
    }

    memset(handles, 0, sizeof(handles));
    memset(fn_ptrs, 0, sizeof(fn_ptrs));
    memset(addrs, 0, sizeof(addrs));

    printf("============================================================\n");
    printf("  EasyJIT Layout Probe\n");
    printf("  sizeof(ConfigRecord) = %zu bytes\n", sizeof(ConfigRecord));
    printf("  LOOP_COUNT = %d   ORDER_MODE = %d (%s)\n",
           LOOP_COUNT, order_mode, order_mode == 1 ? "address-sorted" : "original");
    printf("============================================================\n\n");

    init_configs();
    init_groups();
    prepare_keys();

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
            free(g_configs);
            return 1;
        }
        fn_ptrs[i] = (jit_fn_t)raw;
        addrs[i].original_index = i;
        addrs[i].key = g_keys[i].key;
        addrs[i].addr = (uintptr_t)raw;
        call_order[i] = i;
    }
    warmup_end = clock();

    printf("Warm-up (JIT all %d keys): %.4f ms\n",
           NUM_KEYS,
           (double)(warmup_end - warmup_begin) * 1000.0 / CLOCKS_PER_SEC);
    printf("\nAddress table (original order):\n");
    for (i = 0; i < NUM_KEYS; ++i) {
        long long delta = 0;
        if (i > 0) {
            delta = (long long)(addrs[i].addr - addrs[i - 1].addr);
        }
        printf("  slot=%d key=%d addr=%p delta_from_prev=%lld\n",
               i, addrs[i].key, (void *)addrs[i].addr, delta);
    }
    printf("\n");

    if (order_mode == 1) {
        qsort(addrs, NUM_KEYS, sizeof(addrs[0]), cmp_addrinfo);
        printf("Address table (sorted order):\n");
        for (i = 0; i < NUM_KEYS; ++i) {
            long long delta = 0;
            if (i > 0) {
                delta = (long long)(addrs[i].addr - addrs[i - 1].addr);
            }
            printf("  rank=%d original_slot=%d key=%d addr=%p delta_from_prev=%lld\n",
                   i, addrs[i].original_index, addrs[i].key,
                   (void *)addrs[i].addr, delta);
            call_order[i] = addrs[i].original_index;
        }
        printf("\n");
    }

    start = clock();
    for (i = 0; i < LOOP_COUNT; ++i) {
        int slot = call_order[i % NUM_KEYS];
        update_config(g_keys[slot].config_index, g_keys[slot].group_index);
        sum += fn_ptrs[slot]();
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
