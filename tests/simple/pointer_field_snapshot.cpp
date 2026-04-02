// RUN: %clangxx %cxxflags %include_flags %ld_flags %s -Xclang -fpass-plugin=%lib_pass -o %t
// RUN: %t > %t.out
// RUN: %FileCheck %s < %t.out

#include <easy/jit.h>
#include <easy/snapshot.h>

#include <cstdio>

struct PointerConfig {
  int base;
  const int *array;
  int unused;
};

int eval_pointer_snapshot(int x, PointerConfig const &cfg) {
  const int *local = cfg.array;
  return x + cfg.base + local[2];
}

int main() {
  int data[4] = {11, 22, 33, 44};
  PointerConfig cfg = {7, data, 99};

  auto fn = easy::jit(
      eval_pointer_snapshot,
      5,
      easy::snapshot(cfg, easy::bind_array(&PointerConfig::array, 4)));

  // CHECK: pointer_field.result=45
  std::printf("pointer_field.result=%d\n", fn());
  return 0;
}
