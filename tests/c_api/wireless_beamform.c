/**
 * @file wireless_beamform.c
 * @brief Wireless-style example: beamforming kernel specialized via EasyJIT C API.
 *
 * Simulates a common wireless pattern where a signal-processing kernel
 * has parameters (e.g. number of antennas, number of subcarriers) that
 * are fixed at runtime configuration time but vary between deployments.
 *
 * Without JIT: the loop bounds and multiplier are runtime variables.
 * With JIT:    they become compile-time constants, enabling the LLVM
 *              optimizer to unroll loops, propagate constants, and
 *              vectorize the inner loop.
 *
 * This demonstrates the "wireless/" use-case for the EasyJIT C API.
 */

#include <easy/easyjit_c.h>
#include <easy/attributes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------- The kernel to be specialized ---------- */

/**
 * Simple beamforming-like weighted sum:
 *   out[s] = sum_{a=0}^{n_ant-1} weights[a] * samples[a * n_sub + s]
 *
 * Parameters:
 *   samples  — input sample buffer  (n_ant * n_sub floats)
 *   weights  — per-antenna weights  (n_ant floats)
 *   out      — output buffer        (n_sub floats)
 *   n_ant    — number of antennas   ← specialize this
 *   n_sub    — number of subcarriers ← specialize this
 */
void EASY_JIT_EXPOSE beamform(const float* samples,
                               const float* weights,
                               float* out,
                               int n_ant,
                               int n_sub) {
    for (int s = 0; s < n_sub; s++) {
        float acc = 0.0f;
        for (int a = 0; a < n_ant; a++) {
            acc += weights[a] * samples[a * n_sub + s];
        }
        out[s] = acc;
    }
}

/* ---------- Reference (non-JIT) version for verification ---------- */

static void beamform_ref(const float* samples,
                          const float* weights,
                          float* out,
                          int n_ant,
                          int n_sub) {
    for (int s = 0; s < n_sub; s++) {
        float acc = 0.0f;
        for (int a = 0; a < n_ant; a++) {
            acc += weights[a] * samples[a * n_sub + s];
        }
        out[s] = acc;
    }
}

int main(void) {
    /* --- Runtime configuration (would come from config file, etc.) --- */
    const int N_ANT = 4;
    const int N_SUB = 8;

    printf("=== Wireless Beamforming JIT Example ===\n");
    printf("Antennas: %d, Subcarriers: %d\n\n", N_ANT, N_SUB);

    /* --- Prepare test data --- */
    float samples[N_ANT * N_SUB];
    float weights[N_ANT];
    float out_jit[N_SUB];
    float out_ref[N_SUB];

    for (int a = 0; a < N_ANT; a++) {
        weights[a] = 1.0f / N_ANT;
        for (int s = 0; s < N_SUB; s++)
            samples[a * N_SUB + s] = (float)(a * N_SUB + s + 1);
    }

    /* --- JIT specialization via C API --- */
    easyjit_context_t ctx = NULL;
    easyjit_error_t err = easyjit_context_create(&ctx);
    if (err != EASYJIT_OK) {
        fprintf(stderr, "context_create failed: %s\n", easyjit_get_last_error());
        return 1;
    }

    /* param 0: samples  → forward (runtime arg 0) */
    easyjit_context_set_forward(ctx, 0);
    /* param 1: weights  → forward (runtime arg 1) */
    easyjit_context_set_forward(ctx, 1);
    /* param 2: out      → forward (runtime arg 2) */
    easyjit_context_set_forward(ctx, 2);
    /* param 3: n_ant    → specialize to N_ANT */
    easyjit_context_set_int(ctx, N_ANT);
    /* param 4: n_sub    → specialize to N_SUB */
    easyjit_context_set_int(ctx, N_SUB);

    /* Set optimization level to O3 for maximum loop unrolling */
    easyjit_context_set_opt_level(ctx, 3, 0);

    easyjit_function_t fn = NULL;
    err = easyjit_compile((void*)beamform, ctx, &fn);
    if (err != EASYJIT_OK) {
        fprintf(stderr, "compile failed: %s\n", easyjit_get_last_error());
        easyjit_context_destroy(ctx);
        return 1;
    }
    easyjit_context_destroy(ctx);

    void* raw = NULL;
    easyjit_get_function_pointer(fn, &raw);

    /* The specialized function signature:
     * void beamform_specialized(const float*, const float*, float*)
     * — n_ant and n_sub are baked in as constants.
     */
    typedef void (*beamform_spec_t)(const float*, const float*, float*);
    beamform_spec_t beamform_jit = (beamform_spec_t)raw;

    /* --- Run both versions --- */
    memset(out_jit, 0, sizeof(out_jit));
    memset(out_ref, 0, sizeof(out_ref));

    beamform_jit(samples, weights, out_jit);
    beamform_ref(samples, weights, out_ref, N_ANT, N_SUB);

    /* --- Verify results --- */
    printf("Subcarrier | JIT result | Reference | Match\n");
    printf("-----------+------------+-----------+------\n");
    int all_ok = 1;
    for (int s = 0; s < N_SUB; s++) {
        int ok = (fabsf(out_jit[s] - out_ref[s]) < 1e-5f);
        printf("    %2d     | %10.4f | %9.4f |  %s\n",
               s, out_jit[s], out_ref[s], ok ? "OK" : "FAIL");
        if (!ok) all_ok = 0;
    }

    printf("\n%s\n", all_ok ? "ALL PASSED" : "SOME FAILED");

    easyjit_function_destroy(fn);
    return all_ok ? 0 : 1;
}
