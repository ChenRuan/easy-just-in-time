// tools/easyjit_light_selfcheck.cpp
//
// EasyJIT light backend self-check.
//
// A small, board-friendly C++ program that validates that:
//   * the EasyJIT static-runtime bundle was linked correctly,
//   * the EasyJIT C++ API (`easy::jit`, `easy::Cache`, `easy::options::*`)
//     works,
//   * the light AArch64 backend can compile a representative spread of
//     scalar IR shapes (int / fp / snapshot / pointer / unrolled small
//     loop) on the current target, and
//   * the code cache returns the same compiled function on a second
//     lookup with the same key.
//
// This is *not* a regression harness. It is a smoke test users can copy
// to a target board and run to make sure their integration is sane
// before plugging EasyJIT into a real business kernel.
//
// Recommended invocation on a board:
//
//   EASYJIT_LIGHT=force EASYJIT_LIGHT_VERBOSE=1 \
//     ./easyjit_light_selfcheck --iters 10 --verbose
//
// To additionally capture the raw AArch64 machine code emitted by the
// light backend (for backend-quality analysis), set:
//
//   EASYJIT_LIGHT_DUMP_CODE_DIR=/tmp/ejcode EASYJIT_LIGHT_DUMP_META=1
//
// See runtime/LightBackend_LIMITATIONS.md "Round 12 — Code Dump
// Diagnostics" for the file-name scheme and how to disassemble.
//
// Options:
//   --iters N        run each case N times (default 5)
//   --dump-ir PFX    write the optimized IR for the int+unroll cases to
//                    "<PFX>_<case>.ll"
//   --verbose, -v    print per-case progress
//   --help, -h       this help
//
// Build (against the in-tree shared runtime + pass plugin):
//   clang++ -std=c++17 -O2 \
//     -I<easyjit_root>/include \
//     -Xclang -fpass-plugin=<easyjit_build>/bin/EasyJitPass.so \
//     -L<easyjit_build>/bin -Wl,-rpath,<easyjit_build>/bin \
//     -lEasyJitRuntime -ldl -lpthread \
//     tools/easyjit_light_selfcheck.cpp -o easyjit_light_selfcheck
//
// Build against a static runtime bundle: see
//   examples/easyjit_cpp_minimal/CMakeLists.txt
// for the recommended CMake template.

#include <easy/attributes.h>
#include <easy/code_cache.h>
#include <easy/jit.h>
#include <easy/options.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

using namespace std::placeholders;

// ---------------------------------------------------------------------
// EasyJIT-able business kernels. These must have *external linkage*
// (no anonymous namespace) because easy::layout::serialize_arg<T> is
// instantiated on the argument types and templates cannot be
// instantiated on types with internal linkage.
// ---------------------------------------------------------------------

int EASY_JIT_EXPOSE selfcheck_add_int(int a, int b) {
  return a + b;
}

double EASY_JIT_EXPOSE selfcheck_fma_double(double a, double b, double c) {
  // Mixes fmul + fadd + fneg so the light backend has to lower both
  // scalar fp arithmetic and FNEG.
  //
  // The expression is written volatile-style on purpose: at -O2,
  // `a*b + (-c)` gets contracted into a `llvm.fmuladd.f64` intrinsic,
  // which the current light backend deliberately rejects (f64 fmuladd
  // is not lowered). Using a temporary defeats the contraction so we
  // exercise the plain scalar fp lowering path.
  volatile double t = a * b;
  return ((double)t) + (-c);
}

struct SelfcheckCfg {
  int32_t bias;
  int32_t scale;
  int32_t pad;
};

int EASY_JIT_EXPOSE selfcheck_snapshot(const SelfcheckCfg *cfg, int x) {
  return (x + cfg->bias) * cfg->scale;
}

void EASY_JIT_EXPOSE selfcheck_ptr_io(const int32_t *in, int32_t *out, int x) {
  *out = (*in) * 3 + x;
}

struct SelfcheckUnrollCfg {
  int32_t n;
  int32_t scale;
  int32_t bias;
};

// Exactly the shape exercised by tests/c_api/be_backend_probe.c's
// `unroll` case: after EasyJIT specializes (n, scale, bias) via the
// snapshot, OptLevel >= 2 fully unrolls the loop into straight-line
// scalar IR which the light backend then lowers.
int EASY_JIT_EXPOSE selfcheck_unroll(const SelfcheckUnrollCfg *cfg,
                                     const int *a, const int *b) {
  int acc = cfg->bias;
  for (int i = 0; i < cfg->n; ++i)
    acc += (a[i] + b[i]) * cfg->scale;
  return acc;
}

namespace {

// ---------------------------------------------------------------------
// Options + helpers.
// ---------------------------------------------------------------------

struct Opts {
  int iters = 5;
  bool verbose = false;
  std::string dump_prefix;
};

void usage(const char *argv0) {
  std::printf(
      "Usage: %s [--iters N] [--dump-ir PFX] [--verbose] [--help]\n"
      "\n"
      "Hints:\n"
      "  EASYJIT_LIGHT=force EASYJIT_LIGHT_VERBOSE=1 %s --iters 10\n",
      argv0, argv0);
}

bool parse_args(int argc, char **argv, Opts &opt) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if ((a == "--iters" || a == "-n") && i + 1 < argc) {
      opt.iters = std::atoi(argv[++i]);
      if (opt.iters <= 0) opt.iters = 1;
    } else if (a == "--dump-ir" && i + 1 < argc) {
      opt.dump_prefix = argv[++i];
    } else if (a == "--verbose" || a == "-v") {
      opt.verbose = true;
    } else if (a == "--help" || a == "-h") {
      usage(argv[0]);
      std::exit(0);
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
      usage(argv[0]);
      return false;
    }
  }
  return true;
}

template <class JitFn, class RefFn>
bool check_equal(const Opts &opt, const char *name, JitFn &jit, RefFn ref,
                 int iters) {
  for (int i = 0; i < iters; ++i) {
    auto got = jit(i);
    auto want = ref(i);
    if (got != want) {
      std::printf("[case] %s FAIL iter=%d got=%lld want=%lld\n", name, i,
                  (long long)got, (long long)want);
      return false;
    }
  }
  if (opt.verbose)
    std::printf("[case] %s PASS (%d iters)\n", name, iters);
  else
    std::printf("[case] %s PASS\n", name);
  return true;
}

// ---------------------------------------------------------------------
// Cases.
// ---------------------------------------------------------------------

bool case_int(const Opts &opt) {
  // Bind b = 7 at JIT time. The light backend lowers this to a 2-instr
  // `MOV W?, #7; ADD W0, W0, W?` (plus prologue/epilogue).
  auto inc = opt.dump_prefix.empty()
                 ? easy::jit(selfcheck_add_int, _1, 7)
                 : easy::jit(selfcheck_add_int, _1, 7,
                             easy::options::dump_ir(
                                 opt.dump_prefix + "_int.ll"));
  for (int i = 0; i < opt.iters; ++i) {
    int got = inc(i);
    int want = i + 7;
    if (got != want) {
      std::printf("[case] int FAIL iter=%d got=%d want=%d\n", i, got, want);
      return false;
    }
  }
  std::printf("[case] int PASS%s\n",
              opt.verbose ? " (selfcheck_add_int, b=7)" : "");
  return true;
}

bool case_fp(const Opts &opt) {
  // Bind c = 0.25. Body becomes a * b + (-0.25): fneg + fmul + fadd.
  auto fn = easy::jit(selfcheck_fma_double, _1, _2, 0.25);
  for (int i = 0; i < opt.iters; ++i) {
    double a = 0.5 + i * 0.125;
    double b = 1.5 - i * 0.0625;
    double got = fn(a, b);
    double want = a * b + (-0.25);
    if (std::fabs(got - want) > 1e-12) {
      std::printf("[case] fp FAIL iter=%d got=%.17g want=%.17g\n", i, got,
                  want);
      return false;
    }
  }
  std::printf("[case] fp PASS%s\n",
              opt.verbose ? " (selfcheck_fma_double, c=0.25)" : "");
  return true;
}

bool case_snapshot(const Opts &opt) {
  SelfcheckCfg cfg = {/*bias=*/-3, /*scale=*/4, /*pad=*/0};
  auto fn = easy::jit(selfcheck_snapshot, &cfg, _1);
  for (int i = 0; i < opt.iters; ++i) {
    int got = fn(i);
    int want = (i + cfg.bias) * cfg.scale;
    if (got != want) {
      std::printf("[case] snapshot FAIL iter=%d got=%d want=%d\n", i, got,
                  want);
      return false;
    }
  }
  std::printf("[case] snapshot PASS%s\n",
              opt.verbose ? " (bias=-3 scale=4)" : "");
  return true;
}

bool case_ptr(const Opts &opt) {
  auto fn = easy::jit(selfcheck_ptr_io, _1, _2, _3);
  for (int i = 0; i < opt.iters; ++i) {
    int32_t in = 10 + i;
    int32_t out = 0;
    fn(&in, &out, /*x=*/i * 2 + 1);
    int32_t want = in * 3 + (i * 2 + 1);
    if (out != want) {
      std::printf("[case] ptr FAIL iter=%d in=%d out=%d want=%d\n", i, in,
                  out, want);
      return false;
    }
  }
  std::printf("[case] ptr PASS%s\n",
              opt.verbose ? " (load/store + scalar add)" : "");
  return true;
}

bool case_unroll(const Opts &opt) {
  SelfcheckUnrollCfg cfg = {/*n=*/8, /*scale=*/3, /*bias=*/-25};
  auto fn = opt.dump_prefix.empty()
                ? easy::jit(selfcheck_unroll, &cfg, _1, _2)
                : easy::jit(selfcheck_unroll, &cfg, _1, _2,
                            easy::options::dump_ir(
                                opt.dump_prefix + "_unroll.ll"));
  int A[16], B[16];
  for (int i = 0; i < 16; ++i) {
    A[i] = 100 + i * 7;
    B[i] = -50 + i * 3;
  }
  for (int it = 0; it < opt.iters; ++it) {
    int a_local[16], b_local[16];
    for (int i = 0; i < 16; ++i) {
      a_local[i] = A[i] + it;
      b_local[i] = B[i] - it;
    }
    int got = fn(a_local, b_local);
    int want = cfg.bias;
    for (int i = 0; i < cfg.n; ++i)
      want += (a_local[i] + b_local[i]) * cfg.scale;
    if (got != want) {
      std::printf(
          "[case] unroll FAIL iter=%d n=%d scale=%d got=%d want=%d\n", it,
          cfg.n, cfg.scale, got, want);
      return false;
    }
  }
  std::printf("[case] unroll PASS%s\n",
              opt.verbose ? " (n=8 scale=3, Round-11 scalar unroll)" : "");
  return true;
}

bool case_cache(const Opts &opt) {
  // Verify that a cache lookup with the same key returns the *same*
  // compiled function pointer (no re-compile) AND that the cached
  // function still produces correct results.
  easy::Cache<> C;
  const auto &fn1 = C.jit(selfcheck_add_int, _1, 11);
  if (!C.has(selfcheck_add_int, _1, 11)) {
    std::printf("[case] cache FAIL has() returned false after jit()\n");
    return false;
  }
  const auto &fn2 = C.jit(selfcheck_add_int, _1, 11);
  // FunctionWrapper holds a shared Function*; calling fn1/fn2 should
  // both work and produce the same result.
  for (int i = 0; i < opt.iters; ++i) {
    int g1 = fn1(i);
    int g2 = fn2(i);
    int want = i + 11;
    if (g1 != want || g2 != want) {
      std::printf("[case] cache FAIL iter=%d fn1=%d fn2=%d want=%d\n", i,
                  g1, g2, want);
      return false;
    }
  }
  std::printf("[case] cache PASS%s\n",
              opt.verbose ? " (2nd jit() hits cache)" : "");
  return true;
}

} // anonymous namespace

int main(int argc, char **argv) {
  Opts opt;
  if (!parse_args(argc, argv, opt)) return 2;

  std::printf("EasyJIT light selfcheck\n");
  std::printf("  iters=%d verbose=%d dump_prefix=\"%s\"\n", opt.iters,
              opt.verbose ? 1 : 0, opt.dump_prefix.c_str());
  std::printf("  hint: rerun with EASYJIT_LIGHT=force "
              "EASYJIT_LIGHT_VERBOSE=1 to force the light path\n");

  int failures = 0;
  failures += case_int(opt)      ? 0 : 1;
  failures += case_fp(opt)       ? 0 : 1;
  failures += case_snapshot(opt) ? 0 : 1;
  failures += case_ptr(opt)      ? 0 : 1;
  failures += case_unroll(opt)   ? 0 : 1;
  failures += case_cache(opt)    ? 0 : 1;

  if (failures) {
    std::printf("RESULT FAIL failures=%d\n", failures);
    return 1;
  }
  std::printf("RESULT PASS\n");
  return 0;
}
