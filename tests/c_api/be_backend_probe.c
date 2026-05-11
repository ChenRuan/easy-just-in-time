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
    int32_t bias;
    uint16_t gain;
    uint8_t small;
    float fscale;
    double dscale;
    const int32_t* extra;
} BEStressConfig;

/*
 * Round 11 scalar loop-unroll probe config.
 *
 * `n` and `scale` are baked in via the snapshot so InlineParameters +
 * ConstStructPropagate fold them to constants. With OptLevel >= 2 the
 * runtime pipeline's LoopUnroll pass should fully unroll the body into
 * straight-line scalar IR (no vector IR), which is the only loop shape
 * the light AArch64 backend can lower.
 */
typedef struct {
    int32_t n;
    int32_t scale;
    int32_t bias;
} BEUnrollConfig;

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
    long long stackMix = a8 + a5 - a0 + (a3 ^ a7);
    double fp = (double)f + d + cfg->dscale;
    int32_t cell = grid[i][j];
    int32_t mirror = grid[j][i];
    int32_t v = cell + mirror + cfg->bias + cfg->table[3] + (int32_t)fp;
    *out = v;
    return (long long)v + stackMix + a1 - a2 + a4 - a6;
}

/*
 * Heavier mixed-shape probe: snapshot sub-word fields + bound pointee array
 * + two dynamic 2D GEPs + computed dynamic index + stack-passed GPR/FP args
 * + f32/f64 conversions + two stores + enough straight-line SSA pressure to
 * exercise local scratch reuse.
 */
long long EASY_JIT_EXPOSE be_stress_kernel(const BEStressConfig* cfg,
                                           const int32_t grid[8][8],
                                           const double coeff[8],
                                           int i, int j, int k,
                                           long long a0, long long a1,
                                           long long a2, long long a3,
                                           long long a4, long long a5,
                                           long long a6, long long a7,
                                           long long a8, long long a9,
                                           float f, double d0, double d1,
                                           int32_t* out_i,
                                           double* out_d) {
    int idx = (i + j + k) & 7;
    int32_t cell0 = grid[i][j];
    int32_t cell1 = grid[j][k];
    int32_t cell2 = grid[k][i];
    int32_t ext = cfg->extra[idx];
    int32_t sub = (int32_t)cfg->gain - (int32_t)cfg->small;
    int32_t base = cell0 + cell1 - cell2 + cfg->bias + ext + sub;

    double fp0 = (double)f * (double)cfg->fscale;
    double fp1 = d0 + d1 + cfg->dscale + coeff[idx];
    double fp = fp0 + fp1 + (double)base;
    int32_t narrowed = (int32_t)(float)fp;

    long long mix0 = a8 + a9 + (a3 ^ a7);
    long long mix1 = (a0 - a1) + (a2 * 3LL) - (a4 * 2LL);
    long long mix2 = (a5 + 17LL) ^ (a6 - 23LL);
    int32_t stored = narrowed + (int32_t)(mix0 + mix1 - mix2);
    *out_i = stored;
    *out_d = fp + (double)(mix0 - mix1 + mix2);
    return (long long)stored + mix0 + mix1 - mix2;
}

/*
 * Higher-pressure gauntlet: 20 GPR-class runtime parameters after snapshot
 * specialization, forcing 12 AAPCS64 GPR overflow slots. It also mixes bound
 * arrays, three 2-D dynamic GEPs, FP
 * conversions, wide integer constants, two stores, and local scratch reuse.
 */
long long EASY_JIT_EXPOSE be_gauntlet_kernel(const BEStressConfig* cfg,
                                             const int32_t grid[8][8],
                                             const double coeff[8],
                                             int i, int j, int k,
                                             long long a0, long long a1,
                                             long long a2, long long a3,
                                             long long a4, long long a5,
                                             long long a6, long long a7,
                                             long long a8, long long a9,
                                             long long a10, long long a11,
                                             float f0, float f1,
                                             double d0, double d1, double d2,
                                             int32_t* out_i,
                                             long long* out_l,
                                             double* out_d) {
    int idx0 = (i + j + k) & 7;
    int idx1 = (i + (k << 1)) & 7;
    int idx2 = (j + (i << 1)) & 7;

    int32_t g0 = grid[i][j];
    int32_t g1 = grid[j][k];
    int32_t g2 = grid[k][i];
    int32_t e0 = cfg->extra[idx0];
    int32_t e1 = cfg->extra[idx1];
    int32_t sub = (int32_t)cfg->gain - (int32_t)cfg->small;
    int32_t ibase = g0 - g1 + g2 + cfg->bias + e0 - e1 + sub;

    double fp0 = (double)(f0 + f1) * (double)cfg->fscale;
    double fp1 = d0 - d1 + d2 + cfg->dscale + coeff[idx2];
    double fp2 = fp0 + fp1 + (double)ibase;
    int32_t fp_i = (int32_t)(float)fp2;

    long long p0 = (a8 + a9) - (a10 + a11);
    long long p1 = (a10 ^ a3) + (a11 - a4);
    long long p2 = (a8 * 3LL) - (a9 * 2LL);
    long long p3 = (a0 - a1) + (a2 ^ a5) - (a6 + 0x12345LL) + a7;
    long long total = p0 + p1 + p2 + p3;
    int32_t stored_i = fp_i + (int32_t)total;
    long long stored_l = total + (long long)ibase + (long long)fp_i;
    double stored_d = fp2 + (double)(total - stored_l);

    *out_i = stored_i;
    *out_l = stored_l;
    *out_d = stored_d;
    return stored_l + (long long)stored_i + total - 12345LL;
}

/*
 * Round 11 scalar loop-unroll probe kernel.
 *
 * The loop trip count `cfg->n` and per-iteration constant `cfg->scale`
 * are baked in via the snapshot. After the runtime Optimize pipeline
 * (OptLevel >= 2) the loop must be fully unrolled into straight-line
 * scalar IR -- the light backend cannot lower vector IR or NEON. The
 * runtime args are the two input arrays plus a writeback pointer. The
 * cross-iteration accumulator becomes a tree of adds after unroll, so
 * we deliberately keep `n` small (4 or 8) to avoid stretching GPR
 * pressure / spill capacity.
 *
 * Reduction is i32 to stay in the scalar GPR path.
 */
int EASY_JIT_EXPOSE be_unroll_kernel(const BEUnrollConfig* cfg,
                                     const int32_t* a,
                                     const int32_t* b,
                                     int32_t* out) {
    int acc = cfg->bias;
    for (int i = 0; i < cfg->n; ++i) {
        acc += (a[i] + b[i]) * cfg->scale;
    }
    *out = acc;
    return acc;
}

/*
 * Multi-BB / PHI / select / branch / writeback case (round 8m).
 *
 * Existing kernels were all single-block straight-line. This adds the
 * control-flow shape the user asked for explicitly:
 *   - i32 icmp + select
 *   - br on icmp -> two BBs producing a value -> phi at merge
 *   - fcmp + fp select
 *   - mixed i32/double arithmetic that lives across the merge phi
 *   - sub-word load via cfg->small (snapshot-folded), keeping the snapshot
 *     coverage shape exercised
 *   - writeback through *out and validation in the host code
 *
 * The design is deliberately small (one if/else + two selects) so the
 * light backend's existing branch/phi/select paths get a real test
 * without piling on more pressure than the gauntlet case.
 */
int EASY_JIT_EXPOSE be_branch_kernel(const BEProbeConfig* cfg,
                                     int x, int y, int n, double f,
                                     int32_t* out) {
    int sign = (x < 0) ? -1 : 1;            /* select i32           */
    int v;
    if (n > 0) {                            /* br i1                */
        v = x * 3 + y + cfg->bias;          /* then BB              */
    } else {
        v = x - y * 2 - (int)cfg->small;    /* else BB              */
    }
    /* phi i32 [v.then, v.else] at merge   */
    double g = (f > 0.0) ? f * 2.0 : -f;    /* fcmp + fp select     */
    int gi = (int)g;                        /* fptosi               */
    int picked = v + gi + sign;
    *out = picked + 7;
    return picked - sign;
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
        long long stackMix = a[8] + a[5] - a[0] + (a[3] ^ a[7]);
        double fp = (double)f + d + cfg.dscale;
        int32_t wantOut = grid[i][j] + grid[j][i] + cfg.bias +
                          cfg.table[3] + (int32_t)fp;
        long long want = (long long)wantOut + stackMix + a[1] - a[2] +
                         a[4] - a[6];
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

static int run_stress(const Options* opt) {
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    int rc = 1;
    BEStressConfig cfg;
    int32_t extra[8];
    int32_t grid[8][8];
    double coeff[8];
    int32_t out_i = 0;
    double out_d = 0.0;

    cfg.bias = -313;
    cfg.gain = 4091;
    cfg.small = 37;
    cfg.fscale = 1.75f;
    cfg.dscale = -2.625;
    cfg.extra = extra;
    for (int i = 0; i < 8; ++i) {
        extra[i] = 700 + i * 19;
        coeff[i] = (double)i * 0.625 - 1.25;
        for (int j = 0; j < 8; ++j)
            grid[i][j] = 5000 + i * 131 - j * 17;
    }

    if (new_ctx(&ctx)) goto done;
    if (easyjit_context_set_snapshot(ctx, &cfg, sizeof(cfg)) != EASYJIT_OK) {
        fprintf(stderr, "stress set_snapshot failed: %s\n",
                easyjit_get_last_error());
        goto done;
    }
    if (easyjit_context_bind_array(ctx, offsetof(BEStressConfig, extra),
                                   extra, 8, sizeof(extra[0])) != EASYJIT_OK) {
        fprintf(stderr, "stress bind_array failed: %s\n",
                easyjit_get_last_error());
        goto done;
    }
    for (unsigned i = 0; i < 20; ++i)
        if (add_forward(ctx, i)) goto done;
    if (set_common_opts(ctx, opt, "stress")) goto done;
    if (compile_fn((void*)be_stress_kernel, ctx, &fn)) goto done;
    void* raw = NULL;
    if (get_ptr(fn, &raw)) goto done;

    typedef long long (*Fn)(const int32_t[8][8], const double[8],
                            int, int, int,
                            long long, long long, long long, long long,
                            long long, long long, long long, long long,
                            long long, long long,
                            float, double, double,
                            int32_t*, double*);
    Fn jit = (Fn)raw;
    for (int it = 0; it < opt->iters; ++it) {
        int i = (it * 3 + 1) & 7;
        int j = (it * 5 + 2) & 7;
        int k = (it * 7 + 3) & 7;
        long long a[10];
        for (int n = 0; n < 10; ++n)
            a[n] = (long long)(it * 29 + n * 31 - 90);
        float f = (float)(it % 17) * 0.375f - 2.0f;
        double d0 = (double)(it % 19) * 1.125 - 5.0;
        double d1 = (double)(it % 23) * -0.75 + 4.5;

        out_i = 0;
        out_d = 0.0;
        long long got = jit(grid, coeff, i, j, k,
                            a[0], a[1], a[2], a[3], a[4],
                            a[5], a[6], a[7], a[8], a[9],
                            f, d0, d1, &out_i, &out_d);

        int idx = (i + j + k) & 7;
        int32_t cell0 = grid[i][j];
        int32_t cell1 = grid[j][k];
        int32_t cell2 = grid[k][i];
        int32_t ext = extra[idx];
        int32_t sub = (int32_t)cfg.gain - (int32_t)cfg.small;
        int32_t base = cell0 + cell1 - cell2 + cfg.bias + ext + sub;
        double fp0 = (double)f * (double)cfg.fscale;
        double fp1 = d0 + d1 + cfg.dscale + coeff[idx];
        double fp = fp0 + fp1 + (double)base;
        int32_t narrowed = (int32_t)(float)fp;
        long long mix0 = a[8] + a[9] + (a[3] ^ a[7]);
        long long mix1 = (a[0] - a[1]) + (a[2] * 3LL) - (a[4] * 2LL);
        long long mix2 = (a[5] + 17LL) ^ (a[6] - 23LL);
        int32_t wantOutI = narrowed + (int32_t)(mix0 + mix1 - mix2);
        double wantOutD = fp + (double)(mix0 - mix1 + mix2);
        long long want = (long long)wantOutI + mix0 + mix1 - mix2;

        if (got != want || out_i != wantOutI ||
            fabs(out_d - wantOutD) > 1e-9) {
            fprintf(stderr,
                    "stress FAIL it=%d got=%lld want=%lld out_i=%d want_i=%d "
                    "out_d=%.12f want_d=%.12f\n",
                    it, got, want, out_i, wantOutI, out_d, wantOutD);
            goto done;
        }
    }
    if (opt->verbose) printf("  stress OK (%d iters)\n", opt->iters);
    rc = 0;
done:
    if (fn) easyjit_function_destroy(fn);
    if (ctx) easyjit_context_destroy(ctx);
    return rc;
}

static int run_unroll_with_n(const Options* opt, int32_t n, int32_t scale,
                             int32_t bias, const char* tag) {
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    int rc = 1;
    BEUnrollConfig cfg;
    int32_t a[16];
    int32_t b[16];
    int32_t out = 0;

    cfg.n = n;
    cfg.scale = scale;
    cfg.bias = bias;
    for (int i = 0; i < 16; ++i) {
        a[i] = 1000 + i * 7;
        b[i] = -50 + i * 11;
    }

    if (new_ctx(&ctx)) goto done;
    if (easyjit_context_set_snapshot(ctx, &cfg, sizeof(cfg)) != EASYJIT_OK) {
        fprintf(stderr, "unroll(%s) set_snapshot failed: %s\n",
                tag, easyjit_get_last_error());
        goto done;
    }
    /* Three runtime args after cfg: a, b, out. */
    for (unsigned i = 0; i < 3; ++i)
        if (add_forward(ctx, i)) goto done;
    if (set_common_opts(ctx, opt, tag)) goto done;
    if (compile_fn((void*)be_unroll_kernel, ctx, &fn)) goto done;
    void* raw = NULL;
    if (get_ptr(fn, &raw)) goto done;

    typedef int (*Fn)(const int32_t*, const int32_t*, int32_t*);
    Fn jit = (Fn)raw;
    for (int it = 0; it < opt->iters; ++it) {
        /* Rotate inputs per iter so runtime values vary while
           n/scale/bias stay baked. */
        int32_t a_local[16];
        int32_t b_local[16];
        for (int i = 0; i < 16; ++i) {
            a_local[i] = a[i] + it;
            b_local[i] = b[i] - it;
        }
        out = 0;
        int got = jit(a_local, b_local, &out);
        int want = bias;
        for (int i = 0; i < n; ++i)
            want += (a_local[i] + b_local[i]) * scale;
        if (got != want || out != want) {
            fprintf(stderr,
                    "unroll(%s) FAIL it=%d n=%d scale=%d got=%d want=%d "
                    "out=%d\n",
                    tag, it, n, scale, got, want, out);
            goto done;
        }
    }
    if (opt->verbose)
        printf("  unroll(%s) OK n=%d scale=%d (%d iters)\n",
               tag, n, scale, opt->iters);
    rc = 0;
done:
    if (fn) easyjit_function_destroy(fn);
    if (ctx) easyjit_context_destroy(ctx);
    return rc;
}

static int run_unroll(const Options* opt) {
    /* Cover both a small (4) and a medium (8) constant trip count. */
    int rc = 0;
    rc += run_unroll_with_n(opt, 4, 3, -25, "unroll_n4");
    rc += run_unroll_with_n(opt, 8, 5,  17, "unroll_n8");
    return rc;
}

static int run_branch(const Options* opt) {
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    int rc = 1;
    BEProbeConfig cfg;
    int32_t table[8] = {0};

    cfg.bias = 11;
    cfg.gain = 333;
    cfg.small = 5;
    cfg.scale = 1.0;
    cfg.table = table;

    if (new_ctx(&ctx)) goto done;
    if (easyjit_context_set_snapshot(ctx, &cfg, sizeof(cfg)) != EASYJIT_OK) {
        fprintf(stderr, "branch set_snapshot failed: %s\n",
                easyjit_get_last_error());
        goto done;
    }
    /* Five runtime args after cfg: x, y, n, f, *out. */
    for (unsigned i = 0; i < 5; ++i)
        if (add_forward(ctx, i)) goto done;
    if (set_common_opts(ctx, opt, "branch")) goto done;
    if (compile_fn((void*)be_branch_kernel, ctx, &fn)) goto done;
    void* raw = NULL;
    if (get_ptr(fn, &raw)) goto done;

    typedef int (*Fn)(int, int, int, double, int32_t*);
    Fn jit = (Fn)raw;
    int32_t out = 0;
    for (int it = 0; it < opt->iters; ++it) {
        int x = (it & 1) ? -(it + 3) : (it + 3);
        int y = (it * 7) - 11;
        int n = ((it % 5) - 2);                /* spans <=0 and >0 */
        double f = (it & 2) ? -((double)it * 0.75 + 0.25)
                            :  ((double)it * 0.5 + 1.0);
        int sign = (x < 0) ? -1 : 1;
        int v = (n > 0) ? (x * 3 + y + cfg.bias)
                        : (x - y * 2 - (int)cfg.small);
        double g = (f > 0.0) ? (f * 2.0) : -f;
        int gi = (int)g;
        int picked = v + gi + sign;
        int want_out = picked + 7;
        int want_ret = picked - sign;

        out = 0;
        int got = jit(x, y, n, f, &out);
        if (got != want_ret || out != want_out) {
            fprintf(stderr,
                    "branch FAIL it=%d x=%d y=%d n=%d f=%.3f "
                    "got=%d want=%d out=%d want_out=%d\n",
                    it, x, y, n, f, got, want_ret, out, want_out);
            goto done;
        }
    }
    if (opt->verbose) printf("  branch OK (%d iters)\n", opt->iters);
    rc = 0;
done:
    if (fn) easyjit_function_destroy(fn);
    if (ctx) easyjit_context_destroy(ctx);
    return rc;
}

static int run_gauntlet(const Options* opt) {
    easyjit_context_t ctx = NULL;
    easyjit_function_t fn = NULL;
    int rc = 1;
    BEStressConfig cfg;
    int32_t extra[8];
    int32_t grid[8][8];
    double coeff[8];
    int32_t out_i = 0;
    long long out_l = 0;
    double out_d = 0.0;

    cfg.bias = 917;
    cfg.gain = 5309;
    cfg.small = 41;
    cfg.fscale = -1.25f;
    cfg.dscale = 3.875;
    cfg.extra = extra;
    for (int i = 0; i < 8; ++i) {
        extra[i] = -300 + i * 43;
        coeff[i] = 9.5 - (double)i * 0.875;
        for (int j = 0; j < 8; ++j)
            grid[i][j] = -2000 + i * 211 + j * 29;
    }

    if (new_ctx(&ctx)) goto done;
    if (easyjit_context_set_snapshot(ctx, &cfg, sizeof(cfg)) != EASYJIT_OK) {
        fprintf(stderr, "gauntlet set_snapshot failed: %s\n",
                easyjit_get_last_error());
        goto done;
    }
    if (easyjit_context_bind_array(ctx, offsetof(BEStressConfig, extra),
                                   extra, 8, sizeof(extra[0])) != EASYJIT_OK) {
        fprintf(stderr, "gauntlet bind_array failed: %s\n",
                easyjit_get_last_error());
        goto done;
    }
    for (unsigned i = 0; i < 25; ++i)
        if (add_forward(ctx, i)) goto done;
    if (set_common_opts(ctx, opt, "gauntlet")) goto done;
    if (compile_fn((void*)be_gauntlet_kernel, ctx, &fn)) goto done;
    void* raw = NULL;
    if (get_ptr(fn, &raw)) goto done;

    typedef long long (*Fn)(const int32_t[8][8], const double[8],
                            int, int, int,
                            long long, long long, long long, long long,
                            long long, long long, long long, long long,
                            long long, long long, long long, long long,
                            float, float, double, double, double,
                            int32_t*, long long*, double*);
    Fn jit = (Fn)raw;
    for (int it = 0; it < opt->iters; ++it) {
        int i = (it * 5 + 1) & 7;
        int j = (it * 3 + 6) & 7;
        int k = (it * 7 + 2) & 7;
        long long a[12];
        for (int n = 0; n < 12; ++n)
            a[n] = (long long)(it * 37 + n * 17 - 120);
        float f0 = (float)(it % 13) * 0.5f - 3.0f;
        float f1 = (float)(it % 11) * -0.25f + 1.5f;
        double d0 = (double)(it % 17) * 1.25 - 8.0;
        double d1 = (double)(it % 19) * -0.5 + 2.25;
        double d2 = (double)(it % 23) * 0.375 - 1.125;

        out_i = 0;
        out_l = 0;
        out_d = 0.0;
        long long got = jit(grid, coeff, i, j, k,
                            a[0], a[1], a[2], a[3], a[4], a[5],
                            a[6], a[7], a[8], a[9], a[10], a[11],
                            f0, f1, d0, d1, d2,
                            &out_i, &out_l, &out_d);

        int idx0 = (i + j + k) & 7;
        int idx1 = (i + (k << 1)) & 7;
        int idx2 = (j + (i << 1)) & 7;
        int32_t g0 = grid[i][j];
        int32_t g1 = grid[j][k];
        int32_t g2 = grid[k][i];
        int32_t e0 = extra[idx0];
        int32_t e1 = extra[idx1];
        int32_t sub = (int32_t)cfg.gain - (int32_t)cfg.small;
        int32_t ibase = g0 - g1 + g2 + cfg.bias + e0 - e1 + sub;
        double fp0 = (double)(f0 + f1) * (double)cfg.fscale;
        double fp1 = d0 - d1 + d2 + cfg.dscale + coeff[idx2];
        double fp2 = fp0 + fp1 + (double)ibase;
        int32_t fp_i = (int32_t)(float)fp2;
        long long p0 = (a[8] + a[9]) - (a[10] + a[11]);
        long long p1 = (a[10] ^ a[3]) + (a[11] - a[4]);
        long long p2 = (a[8] * 3LL) - (a[9] * 2LL);
        long long p3 = (a[0] - a[1]) + (a[2] ^ a[5]) -
                       (a[6] + 0x12345LL) + a[7];
        long long total = p0 + p1 + p2 + p3;
        int32_t wantOutI = fp_i + (int32_t)total;
        long long wantOutL = total + (long long)ibase + (long long)fp_i;
        double wantOutD = fp2 + (double)(total - wantOutL);
        long long want = wantOutL + (long long)wantOutI + total - 12345LL;

        if (got != want || out_i != wantOutI || out_l != wantOutL ||
            fabs(out_d - wantOutD) > 1e-9) {
            fprintf(stderr,
                    "gauntlet FAIL it=%d got=%lld want=%lld out_i=%d want_i=%d "
                    "out_l=%lld want_l=%lld out_d=%.12f want_d=%.12f\n",
                    it, got, want, out_i, wantOutI, out_l, wantOutL,
                    out_d, wantOutD);
            goto done;
        }
    }
    if (opt->verbose) printf("  gauntlet OK (%d iters)\n", opt->iters);
    rc = 0;
done:
    if (fn) easyjit_function_destroy(fn);
    if (ctx) easyjit_context_destroy(ctx);
    return rc;
}

static void usage(const char* argv0) {
    printf("Usage: %s [--case all|mem,stack,fp,snapshot,pressure,combo,stress,branch,unroll,gauntlet] "
           "[--iters N] [--opt 0..3] [--dump-ir PREFIX] [--verbose]\n",
           argv0);
    printf("  note: gauntlet is an explicit high-pressure case and is not included in all\n");
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
    if (has_case(&opt, "stress")) {
        printf("[case] stress\n");
        failures += run_stress(&opt);
    }
    if (has_case(&opt, "branch")) {
        printf("[case] branch\n");
        failures += run_branch(&opt);
    }
    if (has_case(&opt, "unroll")) {
        printf("[case] unroll\n");
        failures += run_unroll(&opt);
    }
    if (strcmp(opt.cases, "all") != 0 && has_case(&opt, "gauntlet")) {
        printf("[case] gauntlet\n");
        failures += run_gauntlet(&opt);
    }

    if (failures) {
        printf("BE_PROBE_RESULT FAIL failures=%d\n", failures);
        return 1;
    }
    printf("BE_PROBE_RESULT PASS\n");
    return 0;
}
