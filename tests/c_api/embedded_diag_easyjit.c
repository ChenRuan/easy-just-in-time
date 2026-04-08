/**
 * @file embedded_diag_easyjit.c
 * @brief Embedded-friendly EasyJIT diagnostic harness with mode control.
 *
 * This file is meant for boards with limited tooling. It prints a detailed
 * phase log, flushes after every line, and installs a basic SIGSEGV/SIGBUS
 * handler that reports the last known stage before aborting.
 *
 * Usage:
 *   ./embedded_diag_easyjit <mode>
 *
 * Modes:
 *   0  - print usage and current defaults
 *   1  - baseline only, no EasyJIT
 *   2  - context create/destroy only
 *   3  - small snapshot + compile, no execute
 *   4  - small snapshot + compile + execute
 *   5  - complex snapshot + compile, no execute
 *   6  - complex snapshot + compile + execute once
 *   7  - compile all config keys, no execute
 *   8  - compile all config keys + fetch raw pointers, no execute
 *   9  - compile one key + call specialized function in a short loop
 *   10 - compile all keys + short steady-state loop
 *
 * Environment variables:
 *   EASYJIT_DIAG_OPT_LEVEL    default: 3
 *   EASYJIT_DIAG_OPT_SIZE     default: 0
 *   EASYJIT_DIAG_LOOP_COUNT   default: 1000
 *   EASYJIT_DIAG_MODE9_KEY    default: 0
 *   EASYJIT_DIAG_DUMP_IR      optional path prefix
 */

#include <easy/attributes.h>
#include <easy/easyjit_c.h>

#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONFIG_MAX 12
#define GROUP_MAX 4
#define KEY_STRIDE 13
#define MAKE_KEY(cfg, grp) ((cfg) * KEY_STRIDE + (grp))
#define NUM_KEYS CONFIG_MAX

typedef struct {
    int enabled;
    int gain;
    int bias;
} SmallConfig;

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

static volatile sig_atomic_t g_last_mode = -1;
static volatile sig_atomic_t g_last_key = -1;
static volatile sig_atomic_t g_last_step = -1;

static const char *step_name(int step) {
    switch (step) {
    case 0: return "startup";
    case 1: return "init";
    case 2: return "context_create";
    case 3: return "set_snapshot";
    case 4: return "set_opt_level";
    case 5: return "set_dump_ir";
    case 6: return "compile";
    case 7: return "context_destroy";
    case 8: return "get_function_pointer";
    case 9: return "execute";
    case 10: return "cleanup";
    default: return "unknown";
    }
}

static void log_line(const char *msg) {
    puts(msg);
    fflush(stdout);
}

static void log_stage(int mode, int key, int step, const char *detail) {
    g_last_mode = mode;
    g_last_key = key;
    g_last_step = step;
    printf("[diag] mode=%d key=%d step=%s %s\n",
           mode, key, step_name(step), detail ? detail : "");
    fflush(stdout);
}

static void crash_handler(int sig) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
                       "\n[diag] fatal signal=%d mode=%d key=%d step=%s(%d)\n",
                       sig, (int)g_last_mode, (int)g_last_key,
                       step_name((int)g_last_step), (int)g_last_step);
    if (len > 0) {
        write(STDERR_FILENO, buf, (size_t)len);
    }
    _exit(128 + sig);
}

static void install_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

static int get_env_int(const char *name, int default_value) {
    const char *value = getenv(name);
    char *end = NULL;
    long parsed;

    if (!value || !*value) {
        return default_value;
    }

    parsed = strtol(value, &end, 10);
    if (!end || *end != '\0') {
        fprintf(stderr, "[diag] invalid integer for %s: %s\n", name, value);
        fflush(stderr);
        return default_value;
    }
    return (int)parsed;
}

static const char *get_env_str(const char *name) {
    const char *value = getenv(name);
    return (value && *value) ? value : NULL;
}

static void prepare_keys(void) {
    int i;
    for (i = 0; i < NUM_KEYS; ++i) {
        g_keys[i].config_index = i;
        g_keys[i].group_index = i % GROUP_MAX;
        g_keys[i].key = MAKE_KEY(i, i % GROUP_MAX);
    }
}

static void init_groups(void) {
    int i, j;
    for (i = 0; i < GROUP_MAX; ++i) {
        g_groups[i].base_value = 100 + i * 10;
        g_groups[i].feature_enabled = (i % 2) == 0;
        g_groups[i].offset = i * 5;
        for (j = 0; j < 16; ++j) {
            g_groups[i].table[j] = i * j;
        }
    }
}

static int init_configs(void) {
    int i;

    g_configs = (ConfigRecord *)malloc(sizeof(ConfigRecord) * CONFIG_MAX);
    if (!g_configs) {
        fprintf(stderr, "[diag] malloc failed for configs\n");
        fflush(stderr);
        return 0;
    }

    memset(g_configs, 0, sizeof(ConfigRecord) * CONFIG_MAX);
    for (i = 0; i < CONFIG_MAX; ++i) {
        g_configs[i].config_id = i;
        g_configs[i].enabled = true;
        g_configs[i].priority = i % 3;
        g_configs[i].weight = i * 2;
        g_configs[i].flag_a = false;
        g_configs[i].flag_b = false;
    }
    return 1;
}

static void cleanup_configs(void) {
    free(g_configs);
    g_configs = NULL;
}

static ConfigRecord *get_config(int index) {
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
    cfg->counters[0] = grp->table[7];
    cfg->reserved[0] = grp->table[8];
}

int EASY_JIT_EXPOSE eval_small_config(const SmallConfig *cfg, int x) {
    if (cfg->enabled) {
        return x + cfg->gain;
    }
    return x - cfg->bias;
}

int EASY_JIT_EXPOSE process_config_diag(const ConfigRecord *cfg) {
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

static int create_context_with_small_snapshot(int mode,
                                              easyjit_context_t *out_ctx,
                                              const char *dump_ir) {
    SmallConfig cfg = {1, 7, 99};
    int opt_level = get_env_int("EASYJIT_DIAG_OPT_LEVEL", 3);
    int opt_size = get_env_int("EASYJIT_DIAG_OPT_SIZE", 0);

    log_stage(mode, -1, 2, "before easyjit_context_create");
    if (easyjit_context_create(out_ctx) != EASYJIT_OK) {
        fprintf(stderr, "[diag] context_create failed: %s\n", easyjit_get_last_error());
        fflush(stderr);
        return 0;
    }

    log_stage(mode, -1, 3, "before small set_snapshot");
    if (easyjit_context_set_snapshot(*out_ctx, &cfg, sizeof(cfg)) != EASYJIT_OK) {
        fprintf(stderr, "[diag] set_snapshot failed: %s\n", easyjit_get_last_error());
        fflush(stderr);
        return 0;
    }

    if (easyjit_context_set_forward(*out_ctx, 0) != EASYJIT_OK) {
        fprintf(stderr, "[diag] set_forward failed: %s\n", easyjit_get_last_error());
        fflush(stderr);
        return 0;
    }

    log_stage(mode, -1, 4, "before small set_opt_level");
    if (easyjit_context_set_opt_level(*out_ctx, (unsigned)opt_level, (unsigned)opt_size) != EASYJIT_OK) {
        fprintf(stderr, "[diag] set_opt_level failed: %s\n", easyjit_get_last_error());
        fflush(stderr);
        return 0;
    }

    if (dump_ir) {
        log_stage(mode, -1, 5, dump_ir);
        if (easyjit_context_set_dump_ir(*out_ctx, dump_ir) != EASYJIT_OK) {
            fprintf(stderr, "[diag] set_dump_ir failed: %s\n", easyjit_get_last_error());
            fflush(stderr);
            return 0;
        }
    }

    return 1;
}

static int create_context_with_complex_snapshot(int mode,
                                                int key_index,
                                                easyjit_context_t *out_ctx,
                                                const char *dump_ir) {
    int opt_level = get_env_int("EASYJIT_DIAG_OPT_LEVEL", 3);
    int opt_size = get_env_int("EASYJIT_DIAG_OPT_SIZE", 0);
    ConfigRecord *cfg;
    char detail[128];

    update_config(g_keys[key_index].config_index, g_keys[key_index].group_index);
    cfg = get_config(g_keys[key_index].config_index);

    snprintf(detail, sizeof(detail),
             "cfg=%p sizeof=%zu enabled=%d priority=%d nested_id=%d",
             (void *)cfg, sizeof(ConfigRecord), cfg->enabled,
             cfg->priority, cfg->nested.nested_id);
    log_stage(mode, g_keys[key_index].key, 2, "before easyjit_context_create");
    if (easyjit_context_create(out_ctx) != EASYJIT_OK) {
        fprintf(stderr, "[diag] context_create failed for key=%d: %s\n",
                g_keys[key_index].key, easyjit_get_last_error());
        fflush(stderr);
        return 0;
    }

    log_stage(mode, g_keys[key_index].key, 3, detail);
    if (easyjit_context_set_snapshot(*out_ctx, cfg, sizeof(ConfigRecord)) != EASYJIT_OK) {
        fprintf(stderr, "[diag] set_snapshot failed for key=%d: %s\n",
                g_keys[key_index].key, easyjit_get_last_error());
        fflush(stderr);
        return 0;
    }

    log_stage(mode, g_keys[key_index].key, 4, "before complex set_opt_level");
    if (easyjit_context_set_opt_level(*out_ctx, (unsigned)opt_level, (unsigned)opt_size) != EASYJIT_OK) {
        fprintf(stderr, "[diag] set_opt_level failed for key=%d: %s\n",
                g_keys[key_index].key, easyjit_get_last_error());
        fflush(stderr);
        return 0;
    }

    if (dump_ir) {
        char path[256];
        snprintf(path, sizeof(path), "%s.key%d.ll", dump_ir, g_keys[key_index].key);
        log_stage(mode, g_keys[key_index].key, 5, path);
        if (easyjit_context_set_dump_ir(*out_ctx, path) != EASYJIT_OK) {
            fprintf(stderr, "[diag] set_dump_ir failed for key=%d: %s\n",
                    g_keys[key_index].key, easyjit_get_last_error());
            fflush(stderr);
            return 0;
        }
    }

    return 1;
}

static int mode_baseline_only(void) {
    int loop_count = get_env_int("EASYJIT_DIAG_LOOP_COUNT", 1000);
    int i;
    int sum = 0;

    log_stage(1, -1, 1, "baseline setup");
    for (i = 0; i < loop_count; ++i) {
        SmallConfig cfg = {1, 7, 99};
        sum += eval_small_config(&cfg, i & 7);
    }

    printf("[diag] baseline sum=%d loop_count=%d\n", sum, loop_count);
    fflush(stdout);
    return 0;
}

static int mode_context_only(void) {
    easyjit_context_t ctx = NULL;

    log_stage(2, -1, 2, "context create");
    if (easyjit_context_create(&ctx) != EASYJIT_OK) {
        fprintf(stderr, "[diag] context_create failed: %s\n", easyjit_get_last_error());
        fflush(stderr);
        return 1;
    }

    log_stage(2, -1, 7, "context destroy");
    easyjit_context_destroy(ctx);
    log_line("[diag] context create/destroy ok");
    return 0;
}

static int mode_small_snapshot(int execute) {
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    void *raw = NULL;
    const char *dump_ir = get_env_str("EASYJIT_DIAG_DUMP_IR");

    if (!create_context_with_small_snapshot(execute ? 4 : 3, &ctx, dump_ir)) {
        if (ctx) {
            easyjit_context_destroy(ctx);
        }
        return 1;
    }

    log_stage(execute ? 4 : 3, -1, 6, "before small compile");
    if (easyjit_compile((void *)eval_small_config, ctx, &fn) != EASYJIT_OK) {
        fprintf(stderr, "[diag] compile failed: %s\n", easyjit_get_last_error());
        fflush(stderr);
        easyjit_context_destroy(ctx);
        return 1;
    }

    log_stage(execute ? 4 : 3, -1, 7, "destroy small context");
    easyjit_context_destroy(ctx);

    if (!execute) {
        easyjit_function_destroy(fn);
        log_line("[diag] small snapshot compile ok");
        return 0;
    }

    log_stage(4, -1, 8, "before small get_function_pointer");
    if (easyjit_get_function_pointer(fn, &raw) != EASYJIT_OK) {
        fprintf(stderr, "[diag] get_function_pointer failed: %s\n", easyjit_get_last_error());
        fflush(stderr);
        easyjit_function_destroy(fn);
        return 1;
    }

    log_stage(4, -1, 9, "before small execute");
    {
        typedef int (*small_spec_t)(int);
        small_spec_t spec = (small_spec_t)raw;
        int result = spec(5);
        printf("[diag] small execute result=%d raw=%p\n", result, raw);
        fflush(stdout);
    }

    easyjit_function_destroy(fn);
    return 0;
}

static int mode_complex_one(int mode, int execute) {
    int key_index = get_env_int("EASYJIT_DIAG_MODE9_KEY", 0);
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    void *raw = NULL;
    const char *dump_ir = get_env_str("EASYJIT_DIAG_DUMP_IR");

    if (key_index < 0 || key_index >= NUM_KEYS) {
        fprintf(stderr, "[diag] invalid EASYJIT_DIAG_MODE9_KEY=%d\n", key_index);
        fflush(stderr);
        return 1;
    }

    if (!create_context_with_complex_snapshot(mode, key_index, &ctx, dump_ir)) {
        if (ctx) {
            easyjit_context_destroy(ctx);
        }
        return 1;
    }

    log_stage(mode, g_keys[key_index].key, 6, "before complex compile");
    if (easyjit_compile((void *)process_config_diag, ctx, &fn) != EASYJIT_OK) {
        fprintf(stderr, "[diag] compile failed for key=%d: %s\n",
                g_keys[key_index].key, easyjit_get_last_error());
        fflush(stderr);
        easyjit_context_destroy(ctx);
        return 1;
    }

    log_stage(mode, g_keys[key_index].key, 7, "destroy complex context");
    easyjit_context_destroy(ctx);

    if (!execute) {
        easyjit_function_destroy(fn);
        printf("[diag] complex compile ok for key=%d\n", g_keys[key_index].key);
        fflush(stdout);
        return 0;
    }

    log_stage(mode, g_keys[key_index].key, 8, "before complex get_function_pointer");
    if (easyjit_get_function_pointer(fn, &raw) != EASYJIT_OK) {
        fprintf(stderr, "[diag] get_function_pointer failed for key=%d: %s\n",
                g_keys[key_index].key, easyjit_get_last_error());
        fflush(stderr);
        easyjit_function_destroy(fn);
        return 1;
    }

    log_stage(mode, g_keys[key_index].key, 9, "before complex execute");
    {
        typedef int (*jit_fn_t)(void);
        jit_fn_t spec = (jit_fn_t)raw;
        int result = spec();
        printf("[diag] complex execute result=%d raw=%p key=%d\n",
               result, raw, g_keys[key_index].key);
        fflush(stdout);
    }

    easyjit_function_destroy(fn);
    return 0;
}

static int mode_complex_one_loop(void) {
    int key_index = get_env_int("EASYJIT_DIAG_MODE9_KEY", 0);
    int loop_count = get_env_int("EASYJIT_DIAG_LOOP_COUNT", 1000);
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    void *raw = NULL;
    const char *dump_ir = get_env_str("EASYJIT_DIAG_DUMP_IR");
    int i;
    int sum = 0;

    if (key_index < 0 || key_index >= NUM_KEYS) {
        fprintf(stderr, "[diag] invalid EASYJIT_DIAG_MODE9_KEY=%d\n", key_index);
        fflush(stderr);
        return 1;
    }

    if (!create_context_with_complex_snapshot(9, key_index, &ctx, dump_ir)) {
        if (ctx) {
            easyjit_context_destroy(ctx);
        }
        return 1;
    }

    log_stage(9, g_keys[key_index].key, 6, "before mode9 compile");
    if (easyjit_compile((void *)process_config_diag, ctx, &fn) != EASYJIT_OK) {
        fprintf(stderr, "[diag] compile failed for key=%d: %s\n",
                g_keys[key_index].key, easyjit_get_last_error());
        fflush(stderr);
        easyjit_context_destroy(ctx);
        return 1;
    }

    log_stage(9, g_keys[key_index].key, 7, "destroy mode9 context");
    easyjit_context_destroy(ctx);

    log_stage(9, g_keys[key_index].key, 8, "before mode9 get_function_pointer");
    if (easyjit_get_function_pointer(fn, &raw) != EASYJIT_OK) {
        fprintf(stderr, "[diag] get_function_pointer failed for key=%d: %s\n",
                g_keys[key_index].key, easyjit_get_last_error());
        fflush(stderr);
        easyjit_function_destroy(fn);
        return 1;
    }

    {
        typedef int (*jit_fn_t)(void);
        jit_fn_t spec = (jit_fn_t)raw;
        for (i = 0; i < loop_count; ++i) {
            log_stage(9, g_keys[key_index].key, 9, "mode9 execute iteration");
            sum += spec();
        }
    }

    printf("[diag] mode9 loop done key=%d raw=%p sum=%d loop_count=%d\n",
           g_keys[key_index].key, raw, sum, loop_count);
    fflush(stdout);

    easyjit_function_destroy(fn);
    return 0;
}

static int mode_compile_many(int mode, int fetch_pointer, int execute_loop) {
    easyjit_function_t handles[NUM_KEYS];
    void *raw_ptrs[NUM_KEYS];
    int loop_count = get_env_int("EASYJIT_DIAG_LOOP_COUNT", 1000);
    const char *dump_ir = get_env_str("EASYJIT_DIAG_DUMP_IR");
    int i;
    int sum = 0;

    memset(handles, 0, sizeof(handles));
    memset(raw_ptrs, 0, sizeof(raw_ptrs));

    for (i = 0; i < NUM_KEYS; ++i) {
        easyjit_context_t ctx = NULL;

        if (!create_context_with_complex_snapshot(mode, i, &ctx, dump_ir)) {
            if (ctx) {
                easyjit_context_destroy(ctx);
            }
            goto fail;
        }

        log_stage(mode, g_keys[i].key, 6, "before compile in compile_many");
        if (easyjit_compile((void *)process_config_diag, ctx, &handles[i]) != EASYJIT_OK) {
            fprintf(stderr, "[diag] compile failed for key=%d: %s\n",
                    g_keys[i].key, easyjit_get_last_error());
            fflush(stderr);
            easyjit_context_destroy(ctx);
            goto fail;
        }

        log_stage(mode, g_keys[i].key, 7, "destroy context in compile_many");
        easyjit_context_destroy(ctx);

        if (fetch_pointer || execute_loop) {
            log_stage(mode, g_keys[i].key, 8, "before get_function_pointer in compile_many");
            if (easyjit_get_function_pointer(handles[i], &raw_ptrs[i]) != EASYJIT_OK) {
                fprintf(stderr, "[diag] get_function_pointer failed for key=%d: %s\n",
                        g_keys[i].key, easyjit_get_last_error());
                fflush(stderr);
                goto fail;
            }
            printf("[diag] key=%d raw=%p\n", g_keys[i].key, raw_ptrs[i]);
            fflush(stdout);
        }
    }

    if (execute_loop) {
        for (i = 0; i < loop_count; ++i) {
            int config_index = i % CONFIG_MAX;
            int group_index = i % GROUP_MAX;
            typedef int (*jit_fn_t)(void);
            jit_fn_t fn;

            update_config(config_index, group_index);
            fn = (jit_fn_t)raw_ptrs[config_index];

            log_stage(mode, g_keys[config_index].key, 9, "execute steady-state iteration");
            sum += fn();
        }
        printf("[diag] steady-state sum=%d loop_count=%d\n", sum, loop_count);
        fflush(stdout);
    } else {
        printf("[diag] compile_many ok mode=%d\n", mode);
        fflush(stdout);
    }

    for (i = 0; i < NUM_KEYS; ++i) {
        if (handles[i]) {
            easyjit_function_destroy(handles[i]);
        }
    }
    return 0;

fail:
    for (i = 0; i < NUM_KEYS; ++i) {
        if (handles[i]) {
            easyjit_function_destroy(handles[i]);
        }
    }
    return 1;
}

static void print_usage(const char *argv0) {
    printf("Usage: %s <mode>\n", argv0);
    printf("Modes:\n");
    printf("  0  print usage and defaults\n");
    printf("  1  baseline only, no EasyJIT\n");
    printf("  2  context create/destroy only\n");
    printf("  3  small snapshot + compile, no execute\n");
    printf("  4  small snapshot + compile + execute\n");
    printf("  5  complex snapshot + compile, no execute\n");
    printf("  6  complex snapshot + compile + execute once\n");
    printf("  7  compile all config keys, no execute\n");
    printf("  8  compile all config keys + fetch raw pointers, no execute\n");
    printf("  9  compile one key + call specialized function in a short loop\n");
    printf("  10 compile all keys + short steady-state loop\n");
    printf("Env:\n");
    printf("  EASYJIT_DIAG_OPT_LEVEL   default=3\n");
    printf("  EASYJIT_DIAG_OPT_SIZE    default=0\n");
    printf("  EASYJIT_DIAG_LOOP_COUNT  default=1000\n");
    printf("  EASYJIT_DIAG_MODE9_KEY   default=0\n");
    printf("  EASYJIT_DIAG_DUMP_IR     optional path prefix\n");
    fflush(stdout);
}

int main(int argc, char **argv) {
    int mode = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    install_handlers();

    log_stage(0, -1, 0, "embedded_diag_easyjit start");
    printf("[diag] sizeof(SmallConfig)=%zu sizeof(ConfigRecord)=%zu endian_probe=0x%02x\n",
           sizeof(SmallConfig), sizeof(ConfigRecord), ((unsigned char *)&(uint32_t){1})[0]);
    fflush(stdout);

    init_groups();
    prepare_keys();
    if (!init_configs()) {
        return 1;
    }

    if (argc > 1) {
        mode = atoi(argv[1]);
    }

    if (mode == 0) {
        print_usage(argv[0]);
        cleanup_configs();
        return 0;
    }

    log_stage(mode, -1, 1, "dispatch mode");

    switch (mode) {
    case 1:
        mode = mode_baseline_only();
        break;
    case 2:
        mode = mode_context_only();
        break;
    case 3:
        mode = mode_small_snapshot(0);
        break;
    case 4:
        mode = mode_small_snapshot(1);
        break;
    case 5:
        mode = mode_complex_one(5, 0);
        break;
    case 6:
        mode = mode_complex_one(6, 1);
        break;
    case 7:
        mode = mode_compile_many(7, 0, 0);
        break;
    case 8:
        mode = mode_compile_many(8, 1, 0);
        break;
    case 9:
        mode = mode_complex_one_loop();
        break;
    case 10:
        mode = mode_compile_many(10, 1, 1);
        break;
    default:
        fprintf(stderr, "[diag] unknown mode=%d\n", mode);
        fflush(stderr);
        print_usage(argv[0]);
        mode = 1;
        break;
    }

    log_stage(0, -1, 10, "cleanup");
    cleanup_configs();
    return mode;
}
