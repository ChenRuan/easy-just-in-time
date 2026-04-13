/*
 * config_process_base_fnptr.c
 *
 * Baseline C benchmark matching config_process_easyjit.c's indirect-call shape
 * without using EasyJIT. This helps isolate the cost of function-pointer
 * dispatch from the cost/benefit of JIT specialization itself.
 */

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

__attribute__((noinline))
static int process_config_core(ConfigRecord const *cfg) {
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

__attribute__((noinline))
static int process_config_slot_0(void) { return process_config_core(get_config(0)); }
__attribute__((noinline))
static int process_config_slot_1(void) { return process_config_core(get_config(1)); }
__attribute__((noinline))
static int process_config_slot_2(void) { return process_config_core(get_config(2)); }
__attribute__((noinline))
static int process_config_slot_3(void) { return process_config_core(get_config(3)); }
__attribute__((noinline))
static int process_config_slot_4(void) { return process_config_core(get_config(4)); }
__attribute__((noinline))
static int process_config_slot_5(void) { return process_config_core(get_config(5)); }
__attribute__((noinline))
static int process_config_slot_6(void) { return process_config_core(get_config(6)); }
__attribute__((noinline))
static int process_config_slot_7(void) { return process_config_core(get_config(7)); }
__attribute__((noinline))
static int process_config_slot_8(void) { return process_config_core(get_config(8)); }
__attribute__((noinline))
static int process_config_slot_9(void) { return process_config_core(get_config(9)); }
__attribute__((noinline))
static int process_config_slot_10(void) { return process_config_core(get_config(10)); }
__attribute__((noinline))
static int process_config_slot_11(void) { return process_config_core(get_config(11)); }

int main(void) {
    typedef int (*fn_t)(void);
    fn_t fn_ptrs[NUM_KEYS] = {
        process_config_slot_0,
        process_config_slot_1,
        process_config_slot_2,
        process_config_slot_3,
        process_config_slot_4,
        process_config_slot_5,
        process_config_slot_6,
        process_config_slot_7,
        process_config_slot_8,
        process_config_slot_9,
        process_config_slot_10,
        process_config_slot_11
    };
    clock_t start;
    clock_t end;
    int sum = 0;
    int i;

    init_configs();
    init_groups();
    prepare_keys();

    start = clock();
    for (i = 0; i < LOOP_COUNT; ++i) {
        int config_index = i % CONFIG_MAX;
        int group_index = i % GROUP_MAX;
        update_config(config_index, group_index);
        sum += fn_ptrs[config_index]();
    }
    end = clock();

    printf("sum=%d\n", sum);
    printf("elapsed time: %.6f sec\n",
           (double)(end - start) / CLOCKS_PER_SEC);

    free(g_configs);
    return 0;
}
