// RUN: %clangxx %cxxflags %include_flags %ld_flags %s -Xclang -fpass-plugin=%lib_pass -o %t
// RUN: %t > %t.out
// RUN: %FileCheck %s < %t.out

#include <easy/jit.h>

#include <cstdio>
#include <functional>

using namespace std::placeholders;

static int eval(int x, int const *data) {
  return x + data[0] + data[3];
}

int main() {
  int data[4] = {10, 20, 30, 40};
  auto f = easy::jit(eval, _1, easy::snapshot_array(data, 4));
  // CHECK: result=57
  std::printf("result=%d\n", f(7));
  return 0;
}