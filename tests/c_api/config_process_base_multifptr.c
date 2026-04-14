/*
 * config_process_base_multifptr.c
 *
 * Baseline control that mirrors the EasyJIT benchmark's dispatch shape:
 * build a table of 12 function pointers and dispatch via fn_ptrs[config_index]().
 * Each slot still calls the original process_config logic for the matching
 * config index, so this stays close to the baseline semantics while stressing
 * multi-target indirect calls.
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

typedef int (*config_slot_fn_t)(void);

static ConfigRecord *g_configs = NULL;
static GroupConfig g_groups[GROUP_MAX];

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

__attribute__((noinline))
static int process_config(int config_index) {
    ConfigRecord *cfg = get_config(config_index);
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

#define DEFINE_SLOT_FN(N)                 \
    static int process_config_slot_##N(void) { \
        return process_config(N);         \
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

static void init_fn_ptrs(config_slot_fn_t fn_ptrs[CONFIG_MAX]) {
    fn_ptrs[0] = process_config_slot_0;
    fn_ptrs[1] = process_config_slot_1;
    fn_ptrs[2] = process_config_slot_2;
    fn_ptrs[3] = process_config_slot_3;
    fn_ptrs[4] = process_config_slot_4;
    fn_ptrs[5] = process_config_slot_5;
    fn_ptrs[6] = process_config_slot_6;
    fn_ptrs[7] = process_config_slot_7;
    fn_ptrs[8] = process_config_slot_8;
    fn_ptrs[9] = process_config_slot_9;
    fn_ptrs[10] = process_config_slot_10;
    fn_ptrs[11] = process_config_slot_11;
}

int main(void) {
    config_slot_fn_t fn_ptrs[CONFIG_MAX];
    clock_t start;
    clock_t end;
    int sum = 0;
    int i;

    init_configs();
    init_groups();
    init_fn_ptrs(fn_ptrs);

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
