// examples/easyjit_cpp_minimal/main.cpp
//
// Minimal EasyJIT C++ demo. A trimmed subset of
// tools/easyjit_light_selfcheck.cpp suitable as a starting point for
// downstream integrations. Three cases are demonstrated:
//
//   1. Plain scalar `int` kernel specialized via a snapshot argument.
//   2. A struct-snapshot kernel (the typical "bind a config object at
//      JIT time" pattern).
//   3. A small unrolled scalar loop (exercises the OptLevel >= 2
//      scalar unroll path introduced in EasyJIT round-11).
//
// See README.md in this directory for build instructions.

#include <easy/attributes.h>
#include <easy/code_cache.h>
#include <easy/jit.h>
#include <easy/options.h>

#include <cstdint>
#include <cstdio>
#include <functional>

using namespace std::placeholders;

// ---- 1. Plain int kernel ----------------------------------------------
int EASY_JIT_EXPOSE add_int(int a, int b) { return a + b; }

// ---- 2. Snapshot kernel -----------------------------------------------
struct DemoCfg {
  int32_t bias;
  int32_t scale;
};
int EASY_JIT_EXPOSE apply_cfg(const DemoCfg *cfg, int x) {
  return (x + cfg->bias) * cfg->scale;
}

// ---- 3. Small scalar loop kernel --------------------------------------
struct DemoUnrollCfg {
  int32_t n;
  int32_t scale;
};
int EASY_JIT_EXPOSE dot_like(const DemoUnrollCfg *cfg, const int *a,
                             const int *b) {
  int acc = 0;
  for (int i = 0; i < cfg->n; ++i)
    acc += (a[i] + b[i]) * cfg->scale;
  return acc;
}

int main() {
  std::printf("EasyJIT C++ minimal demo\n");

  // Case 1: bind b = 5.
  auto inc = easy::jit(add_int, _1, 5);
  std::printf("  add_int(10, 5)    = %d (expect 15)\n", inc(10));

  // Case 2: bind cfg at JIT time; constants flow through the optimizer.
  DemoCfg cfg = {/*bias=*/-2, /*scale=*/3};
  auto fn2 = easy::jit(apply_cfg, &cfg, _1);
  std::printf("  apply_cfg(7)      = %d (expect %d)\n", fn2(7),
              (7 + cfg.bias) * cfg.scale);

  // Case 3: bind a 4-iter loop config, then call with runtime data.
  DemoUnrollCfg ucfg = {/*n=*/4, /*scale=*/2};
  auto fn3 = easy::jit(dot_like, &ucfg, _1, _2);
  int A[4] = {1, 2, 3, 4};
  int B[4] = {10, 20, 30, 40};
  int got = fn3(A, B);
  int want = 0;
  for (int i = 0; i < ucfg.n; ++i) want += (A[i] + B[i]) * ucfg.scale;
  std::printf("  dot_like(A, B)    = %d (expect %d)\n", got, want);

  // Case 4: cache reuse — second jit() with the same key hits the cache.
  easy::Cache<> C;
  const auto &f1 = C.jit(add_int, _1, 5);
  bool hit = C.has(add_int, _1, 5);
  const auto &f2 = C.jit(add_int, _1, 5);
  std::printf("  cache hit=%d, f1(2)=%d f2(2)=%d (both expect 7)\n",
              hit ? 1 : 0, f1(2), f2(2));

  std::printf("OK\n");
  return 0;
}
