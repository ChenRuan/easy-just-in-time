/*
 * be_backend_probe.c
 *
 * One-file board-side probe for the EasyJIT light AArch64 backend.
 *
 * Default run is intentionally small so the same binary is easy to copy to
 * an aarch64_be board and iterate with command-line switches:
 *
 *   EASYJIT_LIGHT=force ./be_backend_probe
 *   EASYJIT_LIGHT=force ./be_backend_probe --case mem,fp --iters 1000
 *   EASYJIT_LIGHT=force ./be_backend_probe --case all --verbose
 *   EASYJIT_LIGHT=force ./be_backend_probe --dump-ir /tmp/be_probe
 *
 * The exposed kernels avoid loops and vector source constructs. Coverage is
 * focused on scalar paths that are endian-sensitive or have grown recently:
 * sub-word loads, stores, stack-passed args, f32/f64
 * arithmetic/conversions, snapshot/private-global materialization, and high
 * GPR pressure. The combo case intentionally mixes several supported shapes
 * in one function so regressions at feature boundaries are easier to catch.
 */

#include <easy/attributes.h>
#include <easy/easyjit_c.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int32_t bias;
    uint16_t gain;
    uint8_t small;
    double scale;
    const int32_t* table;
} BEProbeConfig;

typedef struct {
    int32_t bias;
    int32_t table[4];
    double dscale;
} BEComboConfig;

typedef struct {
    int iters;
    int verbose;
    unsigned opt_level;
    const char* dump_prefix;
    const char* cases;
} Options;

/* LDRB + LDRH + LDR W + STR W + negative wide constant. */
int EASY_JIT_EXPOSE be_mem_kernel(const uint8_t* bytep,
                                  const uint16_t* halfp,
                                  const int32_t* cell,
                                  const int32_t* other,
                                  int32_t* out) {
    int v = (int)(*bytep) + (int)(*halfp) + *cell - 12345;
    *out = v;
    return *out + *other;
}

/* 9th GPR-class arg comes from the AAPCS64 stack overflow area. */
long long EASY_JIT_EXPOSE be_stack9_kernel(long long a0, long long a1,
                                           long long a2, long long a3,
                                           long long a4, long long a5,
                                           long long a6, long long a7,
                                           long long a8) {
    return a8 + a0 - a7 + 0x12345LL - 12345LL + (a3 ^ a5);
}

/* f32/f64 arithmetic plus fpext/sitofp/uitofp/fptosi/fptrunc. */
int EASY_JIT_EXPOSE be_fp_kernel(float f, double d, int x, unsigned u) {
    float sf = (f + 1.5f) * 2.0f;
    double dd = (double)sf + d / 2.0 + (double)x - (double)u;
    float narrowed = (float)dd;
    return (int)narrowed;
}

/* Snapshot/private-global materialization plus bound pointee array. */
int EASY_JIT_EXPOSE be_snapshot_kernel(const BEProbeConfig* cfg, int x) {
    double v = (double)(x + cfg->bias + (int)cfg->gain +
                        (int)cfg->small + cfg->table[3]) * cfg->scale;
    return (int)v;
}

/* GPR pressure: old caller-saved-only scratch pool rejected this shape. */
long long EASY_JIT_EXPOSE be_gpr_pressure_kernel(long long a0, long long a1,
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

/* Mixed-shape probe: snapshot + 2D GEP + stack args + FP conversion + store. */
long long EASY_JIT_EXPOSE be_combo_kernel(const BEComboConfig* cfg,
                                          const int32_t grid[8][8],
                                          int i, int j,
                                          long long a0, long long a1,
                                          long long a2, long long a3,
                                          long long a4, long long a5,
                                          long long a6, long long a7,
                                          long long a8,
                                          float f, double d,
                                          int32_t* out) {
    long long stackMix = a8 + a5 - a0;
    double fp = (double)f + d + cfg->dscale;
    int32_t cell = grid[i][j];
    int32_t v = cell + cfg->bias + cfg->table[3] + (int32_t)fp;
    *out = v;
    return (long long)v + stackMix + a1;
}

static int has_case(const Options* opt, const char* name) {
    if (!opt->cases || strcmp(opt->cases, "all") == 0) return 1;
    const char* p = opt->cases;
    size_t n = strlen(name);
    while (*p) {
        while (*p == ',' || *p == ' ') ++p;
        if (strncmp(p, name, n) == 0 &&
            (p[n] == '\0' || p[n] == ',' || p[n] == ' ')) {
            return 1;
        }
        while (*p && *p != ',') ++p;
    }
    return 0;
}

static int set_dump(easyjit_context_t ctx, const Options* opt,
                    const char* suffix) {
    if (!opt->dump_prefix) return 0;
    char path[512];
    snprintf(path, sizeof(path), "%s_%s", opt->dump_prefix, suffix);
    if (easyjit_context_set_dump_ir(ctx, path) != EASYJIT_OK) {
        fprintf(stderr, "set_dump_ir(%s) failed: %s\n",
                path, easyjit_get_last_error());
        return 1;
    }
    return 0;
}

static int set_common_opts(easyjit_context_t ctx, const Options* opt,
                           const char* suffix) {
    if (easyjit_context_set_opt_level(ctx, opt->opt_level, 0) != EASYJIT_OK) {
        fprintf(stderr, "set_opt_level failed: %s\n", easyjit_get_last_error());
        return 1;
    }
    return set_dump(ctx, opt, suffix);
}

static int compile_fn(void* exposed, easyjit_context_t ctx,
                      easyjit_function_t* out) {
    easyjit_error_t err = easyjit_compile(exposed, ctx, out);
    if (err != EASYJIT_OK) {
        fprintf(stderr, "compile failed: %s\n", easyjit_get_last_error());
        return 1;
    }
    return 0;
}

static int get_ptr(easyjit_function_t fn, void** out) {
    easyjit_error_t err = easyjit_get_function_pointer(fn, out);
    if (err != EASYJIT_OK) {
        fprintf(stderr, "get_function_pointer failed: %s\n",
                easyjit_get_last_error());
        return 1;
    }
    return 0;
}

static int new_ctx(easyjit_context_t* ctx) {
    if (easyjit_context_create(ctx) != EASYJIT_OK) {
        fprintf(stderr, "context_create failed: %s\n", easyjit_get_last_error());
        return 1;
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

static int run_mem(const Options* opt) {
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    int rc = 1;
    if (new_ctx(&ctx)) goto done;
    for (unsigned i = 0; i < 5; ++i)
        if (add_forward(ctx, i)) goto done;
    if (set_common_opts(ctx, opt, "mem")) goto done;
    if (compile_fn((void*)be_mem_kernel, ctx, &fn)) goto done;
    void* raw = NULL;
    if (get_ptr(fn, &raw)) goto done;

    typedef int (*Fn)(const uint8_t*, const uint16_t*, const int32_t*,
                      const int32_t*, int32_t*);
    Fn jit = (Fn)raw;
    uint8_t bytes[8];
    uint16_t halves[8];
    int32_t matrix[8][8];
    int32_t out[8];
    for (int i = 0; i < 8; ++i) {
        bytes[i] = (uint8_t)(3 + i * 7);
        halves[i] = (uint16_t)(1000 + i * 11);
        out[i] = 0;
        for (int j = 0; j < 8; ++j)
            matrix[i][j] = 10000 + i * 100 + j;
    }
    for (int it = 0; it < opt->iters; ++it) {
        int i = (it * 3 + 1) & 7;
        int j = (it * 5 + 2) & 7;
        int got = jit(&bytes[i], &halves[j], &matrix[i][j],
                      &matrix[j][i], &out[j]);
        int v = (int)bytes[i] + (int)halves[j] + matrix[i][j] - 12345;
        int want = v + matrix[j][i];
        if (got != want || out[j] != v) {
            fprintf(stderr, "mem FAIL it=%d got=%d want=%d out=%d want_out=%d\n",
                    it, got, want, out[j], v);
            goto done;
        }
    }
    if (opt->verbose) printf("  mem OK (%d iters)\n", opt->iters);
    rc = 0;
done:
    if (fn) easyjit_function_destroy(fn);
    if (ctx) easyjit_context_destroy(ctx);
    return rc;
}

static int run_stack(const Options* opt) {
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    int rc = 1;
    if (new_ctx(&ctx)) goto done;
    for (unsigned i = 0; i < 9; ++i)
        if (add_forward(ctx, i)) goto done;
    if (set_common_opts(ctx, opt, "stack")) goto done;
    if (compile_fn((void*)be_stack9_kernel, ctx, &fn)) goto done;
    void* raw = NULL;
    if (get_ptr(fn, &raw)) goto done;

    typedef long long (*Fn)(long long, long long, long long, long long,
                            long long, long long, long long, long long,
                            long long);
    Fn jit = (Fn)raw;
    for (int it = 0; it < opt->iters; ++it) {
        long long a[9];
        for (int i = 0; i < 9; ++i)
            a[i] = (long long)(it * 17 + i * 13 - 40);
        long long got = jit(a[0], a[1], a[2], a[3], a[4],
                            a[5], a[6], a[7], a[8]);
        long long want = a[8] + a[0] - a[7] + 0x12345LL - 12345LL +
                         (a[3] ^ a[5]);
        if (got != want) {
            fprintf(stderr, "stack FAIL it=%d got=%lld want=%lld\n",
                    it, got, want);
            goto done;
        }
    }
    if (opt->verbose) printf("  stack OK (%d iters)\n", opt->iters);
    rc = 0;
done:
    if (fn) easyjit_function_destroy(fn);
    if (ctx) easyjit_context_destroy(ctx);
    return rc;
}

static int run_fp(const Options* opt) {
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    int rc = 1;
    if (new_ctx(&ctx)) goto done;
    for (unsigned i = 0; i < 4; ++i)
        if (add_forward(ctx, i)) goto done;
    if (set_common_opts(ctx, opt, "fp")) goto done;
    if (compile_fn((void*)be_fp_kernel, ctx, &fn)) goto done;
    void* raw = NULL;
    if (get_ptr(fn, &raw)) goto done;

    typedef int (*Fn)(float, double, int, unsigned);
    Fn jit = (Fn)raw;
    for (int it = 0; it < opt->iters; ++it) {
        float f = (float)(it % 13) * 0.25f - 1.0f;
        double d = (double)(it % 17) * 1.125 - 3.0;
        int x = it * 3 - 20;
        unsigned u = (unsigned)(it % 9);
        int got = jit(f, d, x, u);
        float sf = (f + 1.5f) * 2.0f;
        double dd = (double)sf + d / 2.0 + (double)x - (double)u;
        int want = (int)((float)dd);
        if (got != want) {
            fprintf(stderr, "fp FAIL it=%d got=%d want=%d\n", it, got, want);
            goto done;
        }
    }
    if (opt->verbose) printf("  fp OK (%d iters)\n", opt->iters);
    rc = 0;
done:
    if (fn) easyjit_function_destroy(fn);
    if (ctx) easyjit_context_destroy(ctx);
    return rc;
}

static int run_snapshot(const Options* opt) {
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    int rc = 1;
    BEProbeConfig cfg;
    int32_t table[8];
    cfg.bias = -17;
    cfg.gain = 1234;
    cfg.small = 9;
    cfg.scale = 1.5;
    cfg.table = table;
    for (int i = 0; i < 8; ++i) table[i] = 200 + i * 3;

    if (new_ctx(&ctx)) goto done;
    if (easyjit_context_set_snapshot(ctx, &cfg, sizeof(cfg)) != EASYJIT_OK) {
        fprintf(stderr, "set_snapshot failed: %s\n", easyjit_get_last_error());
        goto done;
    }
    if (easyjit_context_bind_array(ctx, offsetof(BEProbeConfig, table),
                                   table, 8, sizeof(table[0])) != EASYJIT_OK) {
        fprintf(stderr, "bind_array failed: %s\n", easyjit_get_last_error());
        goto done;
    }
    if (add_forward(ctx, 0)) goto done;
    if (set_common_opts(ctx, opt, "snapshot")) goto done;
    if (compile_fn((void*)be_snapshot_kernel, ctx, &fn)) goto done;
    void* raw = NULL;
    if (get_ptr(fn, &raw)) goto done;

    typedef int (*Fn)(int);
    Fn jit = (Fn)raw;
    for (int it = 0; it < opt->iters; ++it) {
        int x = it * 7 - 30;
        int got = jit(x);
        double v = (double)(x + cfg.bias + (int)cfg.gain +
                            (int)cfg.small + table[3]) * cfg.scale;
        int want = (int)v;
        if (got != want) {
            fprintf(stderr, "snapshot FAIL it=%d got=%d want=%d\n",
                    it, got, want);
            goto done;
        }
    }
    if (opt->verbose) printf("  snapshot OK (%d iters)\n", opt->iters);
    rc = 0;
done:
    if (fn) easyjit_function_destroy(fn);
    if (ctx) easyjit_context_destroy(ctx);
    return rc;
}

static int run_pressure(const Options* opt) {
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    int rc = 1;
    if (new_ctx(&ctx)) goto done;
    for (unsigned i = 0; i < 8; ++i)
        if (add_forward(ctx, i)) goto done;
    if (set_common_opts(ctx, opt, "pressure")) goto done;
    if (compile_fn((void*)be_gpr_pressure_kernel, ctx, &fn)) goto done;
    void* raw = NULL;
    if (get_ptr(fn, &raw)) goto done;

    typedef long long (*Fn)(long long, long long, long long, long long,
                            long long, long long, long long, long long);
    Fn jit = (Fn)raw;
    for (int it = 0; it < opt->iters; ++it) {
        long long a[8];
        for (int i = 0; i < 8; ++i)
            a[i] = (long long)(it * 5 + i * 11 - 25);
        long long got = jit(a[0], a[1], a[2], a[3],
                            a[4], a[5], a[6], a[7]);
        long long want = (a[0] + 101) + (a[1] + 102) + (a[2] + 103) +
                         (a[3] + 104) + (a[4] + 105) + (a[5] + 106) +
                         (a[6] + 107) + (a[7] + 108) + (a[0] + 109);
        if (got != want) {
            fprintf(stderr, "pressure FAIL it=%d got=%lld want=%lld\n",
                    it, got, want);
            goto done;
        }
    }
    if (opt->verbose) printf("  pressure OK (%d iters)\n", opt->iters);
    rc = 0;
done:
    if (fn) easyjit_function_destroy(fn);
    if (ctx) easyjit_context_destroy(ctx);
    return rc;
}

static int run_combo(const Options* opt) {
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    int rc = 1;
    BEComboConfig cfg;
    int32_t grid[8][8];
    int32_t out = 0;

    cfg.bias = -77;
    cfg.table[0] = 11;
    cfg.table[1] = 22;
    cfg.table[2] = 33;
    cfg.table[3] = 44;
    cfg.dscale = 2.75;
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
            grid[r][c] = 1000 + r * 100 + c * 7;

    if (new_ctx(&ctx)) goto done;
    if (easyjit_context_set_snapshot(ctx, &cfg, sizeof(cfg)) != EASYJIT_OK) {
        fprintf(stderr, "combo set_snapshot failed: %s\n", easyjit_get_last_error());
        goto done;
    }
    for (unsigned i = 0; i < 15; ++i)
        if (add_forward(ctx, i)) goto done;
    if (set_common_opts(ctx, opt, "combo")) goto done;
    if (compile_fn((void*)be_combo_kernel, ctx, &fn)) goto done;
    void* raw = NULL;
    if (get_ptr(fn, &raw)) goto done;

    typedef long long (*Fn)(const int32_t[8][8], int, int,
                            long long, long long, long long, long long,
                            long long, long long, long long, long long,
                            long long, float, double, int32_t*);
    Fn jit = (Fn)raw;
    for (int it = 0; it < opt->iters; ++it) {
        int i = (it * 3 + 1) & 7;
        int j = (it * 5 + 2) & 7;
        long long a[9];
        for (int k = 0; k < 9; ++k)
            a[k] = (long long)(it * 19 + k * 23 - 50);
        float f = (float)(it % 11) * 0.5f - 1.25f;
        double d = (double)(it % 13) * 1.125 - 3.5;

        out = 0;
        long long got = jit(grid, i, j,
                            a[0], a[1], a[2], a[3], a[4],
                            a[5], a[6], a[7], a[8],
                            f, d, &out);
        long long stackMix = a[8] + a[5] - a[0];
        double fp = (double)f + d + cfg.dscale;
        int32_t wantOut = grid[i][j] + cfg.bias + cfg.table[3] + (int32_t)fp;
        long long want = (long long)wantOut + stackMix + a[1];
        if (got != want || out != wantOut) {
            fprintf(stderr,
                    "combo FAIL it=%d got=%lld want=%lld out=%d want_out=%d\n",
                    it, got, want, out, wantOut);
            goto done;
        }
    }
    if (opt->verbose) printf("  combo OK (%d iters)\n", opt->iters);
    rc = 0;
done:
    if (fn) easyjit_function_destroy(fn);
    if (ctx) easyjit_context_destroy(ctx);
    return rc;
}

static void usage(const char* argv0) {
    printf("Usage: %s [--case all|mem,stack,fp,snapshot,pressure,combo] "
           "[--iters N] [--opt 0..3] [--dump-ir PREFIX] [--verbose]\n",
           argv0);
}

static int parse_args(int argc, char** argv, Options* opt) {
    opt->iters = 3;
    opt->verbose = 0;
    opt->opt_level = 3;
    opt->dump_prefix = NULL;
    opt->cases = "all";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            opt->cases = argv[++i];
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            opt->iters = atoi(argv[++i]);
            if (opt->iters <= 0) opt->iters = 1;
        } else if (strcmp(argv[i], "--opt") == 0 && i + 1 < argc) {
            opt->opt_level = (unsigned)atoi(argv[++i]);
            if (opt->opt_level > 3) opt->opt_level = 3;
        } else if (strcmp(argv[i], "--dump-ir") == 0 && i + 1 < argc) {
            opt->dump_prefix = argv[++i];
        } else if (strcmp(argv[i], "--verbose") == 0 ||
                   strcmp(argv[i], "-v") == 0) {
            opt->verbose = 1;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
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

    int failures = 0;
    printf("=== EasyJIT light BE backend probe ===\n");
    printf("cases=%s iters=%d opt=O%u\n", opt.cases, opt.iters, opt.opt_level);

    if (has_case(&opt, "mem")) {
        printf("[case] mem\n");
        failures += run_mem(&opt);
    }
    if (has_case(&opt, "stack")) {
        printf("[case] stack\n");
        failures += run_stack(&opt);
    }
    if (has_case(&opt, "fp")) {
        printf("[case] fp\n");
        failures += run_fp(&opt);
    }
    if (has_case(&opt, "snapshot")) {
        printf("[case] snapshot\n");
        failures += run_snapshot(&opt);
    }
    if (has_case(&opt, "pressure")) {
        printf("[case] pressure\n");
        failures += run_pressure(&opt);
    }
    if (has_case(&opt, "combo")) {
        printf("[case] combo\n");
        failures += run_combo(&opt);
    }

    if (failures) {
        printf("BE_PROBE_RESULT FAIL failures=%d\n", failures);
        return 1;
    }
    printf("BE_PROBE_RESULT PASS\n");
    return 0;
}
