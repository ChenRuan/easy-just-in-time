// RUN: %clangxx %cxxflags %include_flags %ld_flags %s -Xclang -fpass-plugin=%lib_pass -o %t
// RUN: %t > %t.out
// RUN: %FileCheck %s < %t.out

#include <easy/jit.h>

#include <cstdio>
#include <functional>

using namespace std::placeholders;

struct PartialGlobalConfig {
  int enabled;
  int bias;
};

struct PartialGlobalArrayConfig {
  int enabled;
  int bias;
  const int* values;
};

PartialGlobalConfig g_partial_cfg{1, 7};
static int g_values[] = {10, 20, 30};
PartialGlobalArrayConfig g_partial_array_cfg{1, 2, g_values};

int eval_global_partial(int x) {
  if (g_partial_cfg.enabled)
    return x + g_partial_cfg.bias;
  return x - 99;
}

int eval_global_partial_array(int idx) {
  if (g_partial_array_cfg.enabled)
    return g_partial_array_cfg.values[idx] + g_partial_array_cfg.bias;
  return -1;
}

int main() {
  auto partial_fn = easy::jit(
      eval_global_partial,
      _1,
      easy::options::global_partial_snapshot(
          g_partial_cfg,
          easy::bind_field(&PartialGlobalConfig::enabled)));

  g_partial_cfg.enabled = 0;
  g_partial_cfg.bias = 1000;

  // CHECK: global_partial_snapshot.result=1005
  std::printf("global_partial_snapshot.result=%d\n", partial_fn(5));

  auto partial_array_fn = easy::jit(
      eval_global_partial_array,
      _1,
      easy::options::global_partial_snapshot(
          g_partial_array_cfg,
          easy::bind_field(&PartialGlobalArrayConfig::enabled),
          easy::bind_array(&PartialGlobalArrayConfig::values, 3)));

  g_partial_array_cfg.enabled = 0;
  g_partial_array_cfg.bias = 50;
  g_values[0] = 100;
  g_values[1] = 200;
  g_values[2] = 300;

  // CHECK: global_partial_snapshot.array_result=70
  std::printf("global_partial_snapshot.array_result=%d\n", partial_array_fn(1));
  return 0;
}
