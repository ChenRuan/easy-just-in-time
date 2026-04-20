// RUN: %clangxx %cxxflags %include_flags %ld_flags %s -Xclang -fpass-plugin=%lib_pass -o %t
// RUN: %t > %t.out
// RUN: %FileCheck %s < %t.out

#include <easy/jit.h>

#include <cstdio>
#include <functional>

using namespace std::placeholders;

struct GlobalConfig {
  int enabled;
  int bias;
};

GlobalConfig g_cfg{1, 7};

int eval_global_snapshot(int x) {
  if (g_cfg.enabled)
    return x + g_cfg.bias;
  return x - 99;
}

int main() {
  auto fn = easy::jit(eval_global_snapshot,
                      _1,
                      easy::options::global_snapshot(g_cfg));

  g_cfg.enabled = 0;
  g_cfg.bias = 1000;

  // CHECK: global_snapshot.result=12
  std::printf("global_snapshot.result=%d\n", fn(5));
  return 0;
}
