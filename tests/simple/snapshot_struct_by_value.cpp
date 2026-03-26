// RUN: %clangxx %cxxflags %include_flags %ld_flags %s -Xclang -fpass-plugin=%lib_pass -o %t
// RUN: %t > %t.out
// RUN: %FileCheck %s < %t.out

#include <easy/jit.h>

#include <cstdio>
#include <functional>

using namespace std::placeholders;

struct Inner {
  int values[4];
};

struct Config {
  int base;
  Inner inner;
};

static int eval(int x, Config cfg) {
  return x + cfg.base + cfg.inner.values[2];
}

int main() {
  Config cfg{};
  cfg.base = 10;
  cfg.inner.values[0] = 1;
  cfg.inner.values[1] = 2;
  cfg.inner.values[2] = 30;
  cfg.inner.values[3] = 4;
  auto f = easy::jit(eval, _1, easy::snapshot(cfg));
  // CHECK: result=45
  std::printf("result=%d\n", f(5));
  return 0;
}