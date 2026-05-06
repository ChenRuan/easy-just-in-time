/*
 * light_backend_perf.c
 *
 * One-file benchmark for comparing EasyJIT full ORC runtime against the
 * lightweight AArch64 backend. Build once with the normal C API pass, then
 * run the same target binary with different runtime policy:
 *
 *   EASYJIT_LIGHT=off   ./light_backend_perf --case all --run-iters 1000000
 *   EASYJIT_LIGHT=force ./light_backend_perf --case all --run-iters 1000000
 *
 * Default run is intentionally modest so tests/c_api/run_c_api_tests.sh stays
 * usable. For board measurements increase --compile-iters and --run-iters.
 */

#include <easy/attributes.h>
#include <easy/easyjit_c.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int32_t bias;
    uint16_t gain;
    uint8_t small;
    double scale;
    const int32_t* table;
} PerfConfig;

typedef struct {
    int compile_iters;
    int run_iters;
    int verbose;
    int csv;
    unsigned opt_level;
    const char* cases;
} Options;

typedef struct {
    const char* name;
    int compile_iters;
    int run_iters;
    double compile_ms;
    double run_ms;
    long long checksum;
} BenchResult;

int EASY_JIT_EXPOSE perf_mem_kernel(const uint8_t* bytep,
                                    const uint16_t* halfp,
                                    const int32_t* cell,
                                    const int32_t* other,
                                    int32_t* out) {
    int v = (int)(*bytep) + (int)(*halfp) + *cell - 12345;
    *out = v;
    return *out + *other;
}

long long EASY_JIT_EXPOSE perf_stack9_kernel(long long a0, long long a1,
                                             long long a2, long long a3,
                                             long long a4, long long a5,
                                             long long a6, long long a7,
                                             long long a8) {
    return a8 + a0 - a7 + 0x12345LL - 12345LL + (a3 ^ a5);
}

int EASY_JIT_EXPOSE perf_fp_kernel(float f, double d, int x, unsigned u) {
    float sf = (f + 1.5f) * 2.0f;
    double dd = (double)sf + d / 2.0 + (double)x - (double)u;
    float narrowed = (float)dd;
    return (int)narrowed;
}

int EASY_JIT_EXPOSE perf_snapshot_kernel(const PerfConfig* cfg, int x) {
    double v = (double)(x + cfg->bias + (int)cfg->gain +
                        (int)cfg->small + cfg->table[3]) * cfg->scale;
    return (int)v;
}

long long EASY_JIT_EXPOSE perf_gpr_pressure_kernel(long long a0, long long a1,
                                                   long long a2, long long a3,
                                                   long long a4, long long a5,
                                                   long long a6, long long a7) {
    long long t0 = a0 + 101;
    long long t1 = a1 + 102;
    long long t2 = a2 + 103;
    long long t3 = a3 + 104;
    long long t4 = a4 + 105;
    long long t5 = a5 + 106;
    long long t6 = a6 + 107;
    long long t7 = a7 + 108;
    long long t8 = a0 + 109;
    return ((((((((t0 + t1) + t2) + t3) + t4) + t5) + t6) + t7) + t8);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static int has_case(const Options* opt, const char* name) {
    if (!opt->cases || strcmp(opt->cases, "all") == 0) return 1;
    const char* p = opt->cases;
    size_t n = strlen(name);
    while (*p) {
        while (*p == ',' || *p == ' ') ++p;
        if (strncmp(p, name, n) == 0 &&
            (p[n] == '\0' || p[n] == ',' || p[n] == ' '))
            return 1;
        while (*p && *p != ',') ++p;
    }
    return 0;
}

static int add_forward(easyjit_context_t ctx, unsigned idx) {
    if (easyjit_context_set_forward(ctx, idx) != EASYJIT_OK) {
        fprintf(stderr, "set_forward(%u) failed: %s\n",
                idx, easyjit_get_last_error());
        return 1;
    }
    return 0;
}

static int set_common(easyjit_context_t ctx, const Options* opt) {
    if (easyjit_context_set_opt_level(ctx, opt->opt_level, 0) != EASYJIT_OK) {
        fprintf(stderr, "set_opt_level failed: %s\n", easyjit_get_last_error());
        return 1;
    }
    return 0;
}

static int get_ptr(easyjit_function_t fn, void** raw) {
    if (easyjit_get_function_pointer(fn, raw) != EASYJIT_OK) {
        fprintf(stderr, "get_function_pointer failed: %s\n",
                easyjit_get_last_error());
        return 1;
    }
    return 0;
}

static int compile_mem(const Options* opt, easyjit_function_t* out) {
    easyjit_context_t ctx = NULL;
    if (easyjit_context_create(&ctx) != EASYJIT_OK) return 1;
    for (unsigned i = 0; i < 5; ++i)
        if (add_forward(ctx, i)) { easyjit_context_destroy(ctx); return 1; }
    if (set_common(ctx, opt)) { easyjit_context_destroy(ctx); return 1; }
    int rc = easyjit_compile((void*)perf_mem_kernel, ctx, out) != EASYJIT_OK;
    easyjit_context_destroy(ctx);
    return rc;
}

static int compile_stack(const Options* opt, easyjit_function_t* out) {
    easyjit_context_t ctx = NULL;
    if (easyjit_context_create(&ctx) != EASYJIT_OK) return 1;
    for (unsigned i = 0; i < 9; ++i)
        if (add_forward(ctx, i)) { easyjit_context_destroy(ctx); return 1; }
    if (set_common(ctx, opt)) { easyjit_context_destroy(ctx); return 1; }
    int rc = easyjit_compile((void*)perf_stack9_kernel, ctx, out) != EASYJIT_OK;
    easyjit_context_destroy(ctx);
    return rc;
}

static int compile_fp(const Options* opt, easyjit_function_t* out) {
    easyjit_context_t ctx = NULL;
    if (easyjit_context_create(&ctx) != EASYJIT_OK) return 1;
    for (unsigned i = 0; i < 4; ++i)
        if (add_forward(ctx, i)) { easyjit_context_destroy(ctx); return 1; }
    if (set_common(ctx, opt)) { easyjit_context_destroy(ctx); return 1; }
    int rc = easyjit_compile((void*)perf_fp_kernel, ctx, out) != EASYJIT_OK;
    easyjit_context_destroy(ctx);
    return rc;
}

static void init_config(PerfConfig* cfg, int32_t table[8]) {
    cfg->bias = -17;
    cfg->gain = 1234;
    cfg->small = 9;
    cfg->scale = 1.5;
    cfg->table = table;
    for (int i = 0; i < 8; ++i) table[i] = 200 + i * 3;
}

static int compile_snapshot(const Options* opt, easyjit_function_t* out) {
    easyjit_context_t ctx = NULL;
    PerfConfig cfg;
    int32_t table[8];
    init_config(&cfg, table);
    if (easyjit_context_create(&ctx) != EASYJIT_OK) return 1;
    if (easyjit_context_set_snapshot(ctx, &cfg, sizeof(cfg)) != EASYJIT_OK) {
        easyjit_context_destroy(ctx); return 1;
    }
    if (easyjit_context_bind_array(ctx, offsetof(PerfConfig, table),
                                   table, 8, sizeof(table[0])) != EASYJIT_OK) {
        easyjit_context_destroy(ctx); return 1;
    }
    if (add_forward(ctx, 0)) { easyjit_context_destroy(ctx); return 1; }
    if (set_common(ctx, opt)) { easyjit_context_destroy(ctx); return 1; }
    int rc = easyjit_compile((void*)perf_snapshot_kernel, ctx, out) != EASYJIT_OK;
    easyjit_context_destroy(ctx);
    return rc;
}

static int compile_pressure(const Options* opt, easyjit_function_t* out) {
    easyjit_context_t ctx = NULL;
    if (easyjit_context_create(&ctx) != EASYJIT_OK) return 1;
    for (unsigned i = 0; i < 8; ++i)
        if (add_forward(ctx, i)) { easyjit_context_destroy(ctx); return 1; }
    if (set_common(ctx, opt)) { easyjit_context_destroy(ctx); return 1; }
    int rc = easyjit_compile((void*)perf_gpr_pressure_kernel, ctx, out) != EASYJIT_OK;
    easyjit_context_destroy(ctx);
    return rc;
}

typedef int (*CompileFn)(const Options*, easyjit_function_t*);
typedef long long (*RunFn)(void*, int, int);

static int measure_compile(const Options* opt, CompileFn compile, double* ms) {
    double t0 = now_ms();
    for (int i = 0; i < opt->compile_iters; ++i) {
        easyjit_function_t fn = NULL;
        if (compile(opt, &fn)) {
            fprintf(stderr, "compile failed: %s\n", easyjit_get_last_error());
            return 1;
        }
        easyjit_function_destroy(fn);
    }
    *ms = now_ms() - t0;
    return 0;
}

static int compile_for_run(const Options* opt, CompileFn compile, void** raw,
                           easyjit_function_t* fn) {
    if (compile(opt, fn)) {
        fprintf(stderr, "compile failed: %s\n", easyjit_get_last_error());
        return 1;
    }
    return get_ptr(*fn, raw);
}

static long long run_mem(void* raw, int iters, int verify) {
    typedef int (*Fn)(const uint8_t*, const uint16_t*, const int32_t*,
                      const int32_t*, int32_t*);
    Fn jit = (Fn)raw;
    uint8_t bytes[8];
    uint16_t halves[8];
    int32_t matrix[8][8];
    int32_t out[8];
    long long sum = 0;
    for (int i = 0; i < 8; ++i) {
        bytes[i] = (uint8_t)(3 + i * 7);
        halves[i] = (uint16_t)(1000 + i * 11);
        out[i] = 0;
        for (int j = 0; j < 8; ++j) matrix[i][j] = 10000 + i * 100 + j;
    }
    for (int it = 0; it < iters; ++it) {
        int i = (it * 3 + 1) & 7;
        int j = (it * 5 + 2) & 7;
        int got = jit(&bytes[i], &halves[j], &matrix[i][j],
                      &matrix[j][i], &out[j]);
        if (verify) {
            int v = (int)bytes[i] + (int)halves[j] + matrix[i][j] - 12345;
            int want = v + matrix[j][i];
            if (got != want || out[j] != v) return INT64_MIN;
        }
        sum += got;
    }
    return sum;
}

static long long run_stack(void* raw, int iters, int verify) {
    (void)verify;
    typedef long long (*Fn)(long long, long long, long long, long long,
                            long long, long long, long long, long long,
                            long long);
    Fn jit = (Fn)raw;
    long long sum = 0;
    for (int it = 0; it < iters; ++it) {
        long long a[9];
        for (int i = 0; i < 9; ++i) a[i] = (long long)(it * 17 + i * 13 - 40);
        sum += jit(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
    }
    return sum;
}

static long long run_fp(void* raw, int iters, int verify) {
    (void)verify;
    typedef int (*Fn)(float, double, int, unsigned);
    Fn jit = (Fn)raw;
    long long sum = 0;
    for (int it = 0; it < iters; ++it) {
        float f = (float)(it % 13) * 0.25f - 1.0f;
        double d = (double)(it % 17) * 1.125 - 3.0;
        int x = it * 3 - 20;
        unsigned u = (unsigned)(it % 9);
        sum += jit(f, d, x, u);
    }
    return sum;
}

static long long run_snapshot(void* raw, int iters, int verify) {
    (void)verify;
    typedef int (*Fn)(int);
    Fn jit = (Fn)raw;
    long long sum = 0;
    for (int it = 0; it < iters; ++it)
        sum += jit(it * 7 - 30);
    return sum;
}

static long long run_pressure(void* raw, int iters, int verify) {
    (void)verify;
    typedef long long (*Fn)(long long, long long, long long, long long,
                            long long, long long, long long, long long);
    Fn jit = (Fn)raw;
    long long sum = 0;
    for (int it = 0; it < iters; ++it) {
        long long a[8];
        for (int i = 0; i < 8; ++i) a[i] = (long long)(it * 5 + i * 11 - 25);
        sum += jit(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    }
    return sum;
}

static int bench_one(const Options* opt, const char* name, CompileFn compile,
                     RunFn run, BenchResult* out) {
    out->name = name;
    out->compile_iters = opt->compile_iters;
    out->run_iters = opt->run_iters;
    if (measure_compile(opt, compile, &out->compile_ms)) return 1;

    easyjit_function_t fn = NULL;
    void* raw = NULL;
    if (compile_for_run(opt, compile, &raw, &fn)) return 1;

    long long check = run(raw, 32, 1);
    if (check == INT64_MIN) {
        fprintf(stderr, "%s verification failed\n", name);
        easyjit_function_destroy(fn);
        return 1;
    }

    double t0 = now_ms();
    out->checksum = run(raw, opt->run_iters, 0);
    out->run_ms = now_ms() - t0;
    easyjit_function_destroy(fn);
    return 0;
}

static void print_result(const Options* opt, const BenchResult* r) {
    double compile_us = r->compile_ms * 1000.0 / (double)r->compile_iters;
    double ns_call = r->run_ms * 1000000.0 / (double)r->run_iters;
    if (opt->csv) {
        printf("%s,%d,%.3f,%.3f,%d,%.3f,%.3f,%lld\n",
               r->name, r->compile_iters, r->compile_ms, compile_us,
               r->run_iters, r->run_ms, ns_call, r->checksum);
    } else {
        printf("%-10s compile: %4d iters %9.3f ms %9.3f us/compile | "
               "run: %9d calls %9.3f ms %9.3f ns/call | checksum=%lld\n",
               r->name, r->compile_iters, r->compile_ms, compile_us,
               r->run_iters, r->run_ms, ns_call, r->checksum);
    }
}

static int run_selected_case(const Options* opt, const char* name,
                             CompileFn compile, RunFn run) {
    BenchResult r;
    int rc = bench_one(opt, name, compile, run, &r);
    if (rc == 0) print_result(opt, &r);
    return rc;
}

static void usage(const char* argv0) {
    printf("Usage: %s [--case all|mem,stack,fp,snapshot,pressure] "
           "[--compile-iters N] [--run-iters N] [--opt 0..3] [--csv] [--verbose]\n",
           argv0);
    printf("Compare backends by running the same binary with:\n");
    printf("  EASYJIT_LIGHT=off   %s --case all --run-iters 1000000\n", argv0);
    printf("  EASYJIT_LIGHT=force %s --case all --run-iters 1000000\n", argv0);
}

static int parse_args(int argc, char** argv, Options* opt) {
    opt->compile_iters = 3;
    opt->run_iters = 10000;
    opt->verbose = 0;
    opt->csv = 0;
    opt->opt_level = 3;
    opt->cases = "all";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            opt->cases = argv[++i];
        } else if (strcmp(argv[i], "--compile-iters") == 0 && i + 1 < argc) {
            opt->compile_iters = atoi(argv[++i]);
            if (opt->compile_iters <= 0) opt->compile_iters = 1;
        } else if (strcmp(argv[i], "--run-iters") == 0 && i + 1 < argc) {
            opt->run_iters = atoi(argv[++i]);
            if (opt->run_iters <= 0) opt->run_iters = 1;
        } else if (strcmp(argv[i], "--opt") == 0 && i + 1 < argc) {
            opt->opt_level = (unsigned)atoi(argv[++i]);
            if (opt->opt_level > 3) opt->opt_level = 3;
        } else if (strcmp(argv[i], "--csv") == 0) {
            opt->csv = 1;
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

int main(int argc, char** argv) {
    Options opt;
    if (parse_args(argc, argv, &opt)) return 2;

    if (!opt.csv) {
        printf("=== EasyJIT backend perf ===\n");
        printf("cases=%s compile_iters=%d run_iters=%d opt=O%u\n",
               opt.cases, opt.compile_iters, opt.run_iters, opt.opt_level);
        printf("policy is selected by EASYJIT_LIGHT=off|try|force\n");
    } else {
        printf("case,compile_iters,compile_ms,compile_us,run_iters,run_ms,ns_per_call,checksum\n");
    }

    int failures = 0;
    if (has_case(&opt, "mem")) {
        failures += run_selected_case(&opt, "mem", compile_mem, run_mem);
    }
    if (has_case(&opt, "stack")) {
        failures += run_selected_case(&opt, "stack", compile_stack, run_stack);
    }
    if (has_case(&opt, "fp")) {
        failures += run_selected_case(&opt, "fp", compile_fp, run_fp);
    }
    if (has_case(&opt, "snapshot")) {
        failures += run_selected_case(&opt, "snapshot", compile_snapshot, run_snapshot);
    }
    if (has_case(&opt, "pressure")) {
        failures += run_selected_case(&opt, "pressure", compile_pressure, run_pressure);
    }

    if (failures) {
        printf("PERF_RESULT FAIL failures=%d\n", failures);
        return 1;
    }
    if (!opt.csv) printf("PERF_RESULT PASS\n");
    return 0;
}
