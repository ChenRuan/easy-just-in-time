/*
 * wireless_pointer_perf.c
 *
 * Board-friendly EasyJIT C API benchmark adapted from wireless-test/example1.
 * It measures whether runtime savings from specializing pointer-heavy wireless
 * config code can pay back the JIT compile cost.
 *
 * Compare backends by running the same binary with:
 *   EASYJIT_LIGHT=off   ./wireless_pointer_perf --run-iters 1000000
 *   EASYJIT_LIGHT=force ./wireless_pointer_perf --run-iters 1000000
 *
 * Output is plain text only, so it is usable on serial consoles.
 */

#include <easy/attributes.h>
#include <easy/easyjit_c.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__GNUC__) || defined(__clang__)
#define PERF_NOINLINE __attribute__((noinline))
#else
#define PERF_NOINLINE
#endif

#define TRP_MAX 12
#define CELL_MAX 4
#define KEY_STRIDE 13
#define MAKE_KEY(trp, cell) ((trp) * KEY_STRIDE + (cell))

typedef struct {
    int subId;
    bool subEnable;
    int subArray[8];
} SubConfig;

typedef struct {
    int trpId;
    bool enable;
    int priority;
    int weight;
    bool flagA;
    bool flagB;
    int data[16];
    int counters[8];
    SubConfig sub;
    int reserved[32];
} PdcchTrpConfig;

typedef struct {
    int baseValue;
    bool featureEnable;
    int offset;
    int table[16];
} McmCellConfig;

typedef struct {
    int trpIndex;
    int cellIndex;
    int key;
} KeyInfo;

typedef int (*jit_fn_t)(void);

typedef struct {
    int run_iters;
    int compile_rounds;
    int keys;
    int opt_level;
    int mode_snapshot;
    int mode_pointer;
    int verbose;
} Options;

typedef struct {
    const char *name;
    double compile_ms;
    double process_ms;
    double full_ms;
    long long process_sum;
    long long full_sum;
    easyjit_function_t handles[TRP_MAX];
    jit_fn_t fns[TRP_MAX];
} JitSet;

static PdcchTrpConfig *g_pdc = NULL;
static McmCellConfig g_mcm[CELL_MAX];
static KeyInfo g_keys[TRP_MAX];

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static void init_pdc_config(void) {
    g_pdc = (PdcchTrpConfig *)malloc(sizeof(PdcchTrpConfig) * TRP_MAX);
    if (!g_pdc) {
        fprintf(stderr, "malloc failed\n");
        exit(1);
    }
    memset(g_pdc, 0, sizeof(PdcchTrpConfig) * TRP_MAX);
    for (int i = 0; i < TRP_MAX; i++) {
        g_pdc[i].trpId = i;
        g_pdc[i].enable = true;
        g_pdc[i].priority = i % 3;
        g_pdc[i].weight = i * 2;
        g_pdc[i].flagA = (i % 5) == 0;
        g_pdc[i].flagB = (i % 7) == 0;
        g_pdc[i].counters[0] = 17 + i;
        g_pdc[i].reserved[0] = 23 + i;
    }
}

static void init_mcm(void) {
    for (int i = 0; i < CELL_MAX; i++) {
        g_mcm[i].baseValue = 100 + i * 10;
        g_mcm[i].featureEnable = (i % 2 == 0);
        g_mcm[i].offset = i * 5;
        for (int j = 0; j < 16; j++) {
            g_mcm[i].table[j] = i * j;
        }
    }
}

static void prepare_keys(void) {
    for (int i = 0; i < TRP_MAX; i++) {
        g_keys[i].trpIndex = i;
        g_keys[i].cellIndex = i % CELL_MAX;
        g_keys[i].key = MAKE_KEY(i, i % CELL_MAX);
    }
}

static inline PdcchTrpConfig *get_trp_config(int trpIndex) {
    return &g_pdc[trpIndex];
}

static int MCM_getBaseValue(int cellIndex) { return g_mcm[cellIndex].baseValue; }
static bool MCM_getFeatureEnable(int cellIndex) { return g_mcm[cellIndex].featureEnable; }
static int MCM_getOffset(int cellIndex) { return g_mcm[cellIndex].offset; }
static int MCM_getTableValue0(int cellIndex) { return g_mcm[cellIndex].table[0]; }
static int MCM_getTableValue1(int cellIndex) { return g_mcm[cellIndex].table[1]; }
static int MCM_getTableValue2(int cellIndex) { return g_mcm[cellIndex].table[2]; }
static int MCM_getTableValue3(int cellIndex) { return g_mcm[cellIndex].table[3]; }
static int MCM_getTableValue4(int cellIndex) { return g_mcm[cellIndex].table[4]; }
static int MCM_getTableValue5(int cellIndex) { return g_mcm[cellIndex].table[5]; }
static int MCM_getTableValue6(int cellIndex) { return g_mcm[cellIndex].table[6]; }

static PERF_NOINLINE void schConfig(int trpIndex, int cellIndex) {
    PdcchTrpConfig *cfg = get_trp_config(trpIndex);
    cfg->priority = MCM_getBaseValue(cellIndex);
    cfg->enable = MCM_getFeatureEnable(cellIndex);
    cfg->weight = MCM_getOffset(cellIndex);
    cfg->data[0] = MCM_getTableValue0(cellIndex);
    cfg->data[1] = MCM_getTableValue1(cellIndex);
    cfg->data[2] = MCM_getTableValue2(cellIndex);
    cfg->data[3] = MCM_getTableValue3(cellIndex);
    cfg->sub.subId = MCM_getTableValue4(cellIndex);
    cfg->sub.subEnable = (MCM_getTableValue5(cellIndex) % 2) != 0;
    cfg->sub.subArray[0] = MCM_getTableValue6(cellIndex);
}

/*
 * Keep this branch-free so the light backend measures live struct-pointer
 * field access instead of its current branch/select limitations.
 */
static PERF_NOINLINE int processTrp_ref(PdcchTrpConfig *cfg) {
    int result = cfg->priority + cfg->weight;
    result += cfg->data[0] + cfg->sub.subId;
    result += cfg->reserved[0] + (int)cfg->enable;
    return result;
}

int EASY_JIT_EXPOSE PERF_NOINLINE processTrp_jit(PdcchTrpConfig *cfg) {
    int result = cfg->priority + cfg->weight;
    result += cfg->data[0] + cfg->sub.subId;
    result += cfg->reserved[0] + (int)cfg->enable;
    return result;
}

static int set_opt(easyjit_context_t ctx, const Options *opt) {
    if (easyjit_context_set_opt_level(ctx, (unsigned)opt->opt_level, 0) != EASYJIT_OK) {
        fprintf(stderr, "set_opt_level failed: %s\n", easyjit_get_last_error());
        return 1;
    }
    return 0;
}

static void destroy_jit_set(JitSet *set, int keys) {
    for (int i = 0; i < keys; i++) {
        if (set->handles[i]) {
            easyjit_function_destroy(set->handles[i]);
            set->handles[i] = NULL;
            set->fns[i] = NULL;
        }
    }
}

static int compile_one(const Options *opt, int key_index, int snapshot,
                       easyjit_function_t *out_handle, jit_fn_t *out_fn) {
    int trp = g_keys[key_index].trpIndex;
    int cell = g_keys[key_index].cellIndex;
    PdcchTrpConfig *cfg;
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    void *raw = NULL;

    schConfig(trp, cell);
    cfg = get_trp_config(trp);

    if (easyjit_context_create(&ctx) != EASYJIT_OK) {
        fprintf(stderr, "context_create failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    if (snapshot) {
        if (easyjit_context_set_snapshot(ctx, cfg, sizeof(PdcchTrpConfig)) != EASYJIT_OK) {
            fprintf(stderr, "set_snapshot failed: %s\n", easyjit_get_last_error());
            easyjit_context_destroy(ctx);
            return 1;
        }
    } else {
        if (easyjit_context_set_pointer(ctx, cfg) != EASYJIT_OK) {
            fprintf(stderr, "set_pointer failed: %s\n", easyjit_get_last_error());
            easyjit_context_destroy(ctx);
            return 1;
        }
    }

    if (set_opt(ctx, opt)) {
        easyjit_context_destroy(ctx);
        return 1;
    }

    if (easyjit_compile((void *)processTrp_jit, ctx, &fn) != EASYJIT_OK) {
        fprintf(stderr, "compile failed key=%d trp=%d cell=%d mode=%s: %s\n",
                g_keys[key_index].key, trp, cell,
                snapshot ? "snapshot" : "pointer",
                easyjit_get_last_error());
        easyjit_context_destroy(ctx);
        return 1;
    }
    easyjit_context_destroy(ctx);

    if (easyjit_get_function_pointer(fn, &raw) != EASYJIT_OK) {
        fprintf(stderr, "get_function_pointer failed: %s\n", easyjit_get_last_error());
        easyjit_function_destroy(fn);
        return 1;
    }

    *out_handle = fn;
    *out_fn = (jit_fn_t)raw;
    return 0;
}

static int compile_jit_set(const Options *opt, const char *name, int snapshot,
                           JitSet *out) {
    memset(out, 0, sizeof(*out));
    out->name = name;

    double t0 = now_ms();
    for (int round = 0; round < opt->compile_rounds; round++) {
        destroy_jit_set(out, opt->keys);
        for (int i = 0; i < opt->keys; i++) {
            if (compile_one(opt, i, snapshot, &out->handles[i], &out->fns[i])) {
                destroy_jit_set(out, opt->keys);
                return 1;
            }
        }
    }
    out->compile_ms = (now_ms() - t0) / (double)opt->compile_rounds;
    return 0;
}

static long long run_baseline_process_only(const Options *opt) {
    long long sum = 0;
    for (int it = 0; it < opt->run_iters; it++) {
        int k = it % opt->keys;
        int trp = g_keys[k].trpIndex;
        sum += processTrp_ref(get_trp_config(trp));
    }
    return sum;
}

static long long run_baseline_full(const Options *opt) {
    long long sum = 0;
    for (int it = 0; it < opt->run_iters; it++) {
        int k = it % opt->keys;
        int trp = g_keys[k].trpIndex;
        int cell = g_keys[k].cellIndex;
        schConfig(trp, cell);
        sum += processTrp_ref(get_trp_config(trp));
    }
    return sum;
}

static long long run_jit_process_only(const Options *opt, const JitSet *set) {
    long long sum = 0;
    for (int it = 0; it < opt->run_iters; it++) {
        int k = it % opt->keys;
        sum += set->fns[k]();
    }
    return sum;
}

static long long run_jit_full(const Options *opt, const JitSet *set) {
    long long sum = 0;
    for (int it = 0; it < opt->run_iters; it++) {
        int k = it % opt->keys;
        int trp = g_keys[k].trpIndex;
        int cell = g_keys[k].cellIndex;
        schConfig(trp, cell);
        sum += set->fns[k]();
    }
    return sum;
}

static void reset_wireless_state(void) {
    free(g_pdc);
    g_pdc = NULL;
    init_pdc_config();
    init_mcm();
    prepare_keys();
    for (int i = 0; i < TRP_MAX; i++) {
        schConfig(g_keys[i].trpIndex, g_keys[i].cellIndex);
    }
}

static void print_break_even(const char *label, double compile_ms,
                             double baseline_ms, double jit_ms, int run_iters) {
    double saved_ms = baseline_ms - jit_ms;
    double saved_ns = saved_ms * 1000000.0 / (double)run_iters;
    printf("  %s baseline %.3f ms, jit %.3f ms, saved %.3f ms, %.3f ns/call\n",
           label, baseline_ms, jit_ms, saved_ms, saved_ns);
    if (saved_ns > 0.0) {
        double calls = compile_ms * 1000000.0 / saved_ns;
        double keyset_rounds = calls / (double)run_iters;
        printf("  %s break-even %.0f calls, %.3f x this run length\n",
               label, calls, keyset_rounds);
    } else {
        printf("  %s break-even: no positive runtime saving in this run\n", label);
    }
}

static int measure_baseline(const Options *opt, double *process_ms,
                            double *full_ms, long long *process_sum,
                            long long *full_sum) {
    reset_wireless_state();
    run_baseline_process_only(opt);
    double t0 = now_ms();
    *process_sum = run_baseline_process_only(opt);
    *process_ms = now_ms() - t0;

    reset_wireless_state();
    run_baseline_full(opt);
    t0 = now_ms();
    *full_sum = run_baseline_full(opt);
    *full_ms = now_ms() - t0;
    return 0;
}

static int measure_jit(const Options *opt, JitSet *set) {
    reset_wireless_state();
    run_jit_process_only(opt, set);
    double t0 = now_ms();
    set->process_sum = run_jit_process_only(opt, set);
    set->process_ms = now_ms() - t0;

    reset_wireless_state();
    run_jit_full(opt, set);
    t0 = now_ms();
    set->full_sum = run_jit_full(opt, set);
    set->full_ms = now_ms() - t0;
    return 0;
}

static int verify_sums(const char *name, long long base_process, long long base_full,
                       const JitSet *set, int snapshot) {
    if (set->process_sum != base_process || set->full_sum != base_full) {
        fprintf(stderr,
                "%s verification failed: base_process=%lld jit_process=%lld "
                "base_full=%lld jit_full=%lld\n",
                name, base_process, set->process_sum, base_full, set->full_sum);
        if (snapshot) {
            fprintf(stderr, "snapshot mode assumes each TRP is reconfigured to "
                            "the same cell value used at compile time\n");
        }
        return 1;
    }
    return 0;
}

static int run_mode(const Options *opt, const char *name, int snapshot,
                    double base_process_ms, double base_full_ms,
                    long long base_process_sum, long long base_full_sum) {
    JitSet set;
    if (compile_jit_set(opt, name, snapshot, &set)) return 1;
    if (measure_jit(opt, &set)) {
        destroy_jit_set(&set, opt->keys);
        return 1;
    }
    if (verify_sums(name, base_process_sum, base_full_sum, &set, snapshot)) {
        destroy_jit_set(&set, opt->keys);
        return 1;
    }

    printf("\n[%s]\n", name);
    printf("  compile %.3f ms/keyset avg over %d round(s), %.3f ms/key\n",
           set.compile_ms, opt->compile_rounds,
           set.compile_ms / (double)opt->keys);
    printf("  checksum process=%lld full=%lld\n", set.process_sum, set.full_sum);
    print_break_even("process-only", set.compile_ms, base_process_ms,
                     set.process_ms, opt->run_iters);
    print_break_even("full-flow", set.compile_ms, base_full_ms,
                     set.full_ms, opt->run_iters);

    if (opt->verbose) {
        for (int i = 0; i < opt->keys; i++) {
            printf("    key=%3d trp=%d cell=%d result=%d\n",
                   g_keys[i].key, g_keys[i].trpIndex, g_keys[i].cellIndex,
                   set.fns[i]());
        }
    }

    destroy_jit_set(&set, opt->keys);
    return 0;
}

static void usage(const char *argv0) {
    printf("Usage: %s [--mode both|snapshot|pointer] [--run-iters N]\n", argv0);
    printf("          [--compile-rounds N] [--keys 1..12] [--opt 0..3] [--verbose]\n");
    printf("\n");
    printf("Run the same binary twice to compare backend policies:\n");
    printf("  EASYJIT_LIGHT=off   %s --run-iters 1000000\n", argv0);
    printf("  EASYJIT_LIGHT=force %s --run-iters 1000000\n", argv0);
}

static int parse_args(int argc, char **argv, Options *opt) {
    opt->run_iters = 1000000;
    opt->compile_rounds = 1;
    opt->keys = TRP_MAX;
    opt->opt_level = 3;
    opt->mode_snapshot = 1;
    opt->mode_pointer = 1;
    opt->verbose = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char *m = argv[++i];
            opt->mode_snapshot = strcmp(m, "pointer") != 0;
            opt->mode_pointer = strcmp(m, "snapshot") != 0;
            if (strcmp(m, "both") != 0 && strcmp(m, "snapshot") != 0 &&
                strcmp(m, "pointer") != 0) {
                fprintf(stderr, "bad --mode: %s\n", m);
                return 1;
            }
        } else if (strcmp(argv[i], "--run-iters") == 0 && i + 1 < argc) {
            opt->run_iters = atoi(argv[++i]);
            if (opt->run_iters <= 0) opt->run_iters = 1;
        } else if (strcmp(argv[i], "--compile-rounds") == 0 && i + 1 < argc) {
            opt->compile_rounds = atoi(argv[++i]);
            if (opt->compile_rounds <= 0) opt->compile_rounds = 1;
        } else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            opt->keys = atoi(argv[++i]);
            if (opt->keys < 1) opt->keys = 1;
            if (opt->keys > TRP_MAX) opt->keys = TRP_MAX;
        } else if (strcmp(argv[i], "--opt") == 0 && i + 1 < argc) {
            opt->opt_level = atoi(argv[++i]);
            if (opt->opt_level < 0) opt->opt_level = 0;
            if (opt->opt_level > 3) opt->opt_level = 3;
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            opt->verbose = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    Options opt;
    double base_process_ms = 0.0;
    double base_full_ms = 0.0;
    long long base_process_sum = 0;
    long long base_full_sum = 0;
    int failures = 0;

    if (parse_args(argc, argv, &opt)) return 2;

    reset_wireless_state();
    printf("=== EasyJIT wireless pointer perf ===\n");
    printf("keys=%d/%d run_iters=%d compile_rounds=%d opt=O%d\n",
           opt.keys, TRP_MAX, opt.run_iters, opt.compile_rounds, opt.opt_level);
    printf("sizeof(PdcchTrpConfig)=%zu bytes, policy via EASYJIT_LIGHT=off|try|force\n",
           sizeof(PdcchTrpConfig));

    measure_baseline(&opt, &base_process_ms, &base_full_ms,
                     &base_process_sum, &base_full_sum);
    printf("\n[baseline direct C]\n");
    printf("  process-only %.3f ms, %.3f ns/call, checksum=%lld\n",
           base_process_ms,
           base_process_ms * 1000000.0 / (double)opt.run_iters,
           base_process_sum);
    printf("  full-flow    %.3f ms, %.3f ns/call, checksum=%lld\n",
           base_full_ms,
           base_full_ms * 1000000.0 / (double)opt.run_iters,
           base_full_sum);

    if (opt.mode_snapshot) {
        failures += run_mode(&opt, "snapshot raw-ptr", 1, base_process_ms,
                             base_full_ms, base_process_sum, base_full_sum);
    }
    if (opt.mode_pointer) {
        failures += run_mode(&opt, "set_pointer raw-ptr", 0, base_process_ms,
                             base_full_ms, base_process_sum, base_full_sum);
    }

    free(g_pdc);
    g_pdc = NULL;

    if (failures) {
        printf("\nWIRELESS_POINTER_PERF FAIL failures=%d\n", failures);
        return 1;
    }
    printf("\nWIRELESS_POINTER_PERF PASS\n");
    return 0;
}
