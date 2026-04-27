// RUN: %clangxx %cxxflags %include_flags %ld_flags %s -Xclang -fpass-plugin=%lib_pass -o %t
// RUN: %t > %t.out
// RUN: %FileCheck %s < %t.out

#include <easy/code_cache.h>

#include <functional>
#include <cstdio>

using namespace std::placeholders;

int add (int a, int b) {
  return a+b;
}

int mul(int a, int b) {
  return a*b;
}

int main() {
  easy::Cache<> C;
  easy::Cache<int> ExplicitCache;

  // CHECK: inc(4) is 5
  // CHECK: inc(5) is 6
  // CHECK: inc(6) is 7
  // CHECK: inc(7) is 8
  // CHECK: triple(4) is 12
  // CHECK: triple(5) is 15

  for(int i = 0; i != 16; ++i) {
    auto const &inc = C.jit(add, _1, 1);

    if(!C.has(add, _1, 1)) {
      printf("code not in cache!\n");
      return -1;
    }

    for(int v = 4; v != 8; ++v)
      printf("inc(%d) is %d\n", v, inc(v));
  }

  if(ExplicitCache.has(3)) {
    printf("explicit cache unexpectedly populated!\n");
    return -1;
  }

  auto const &triple = ExplicitCache.jit(3, mul, _1, 3);

  if(!ExplicitCache.has(3)) {
    printf("explicit key missing from cache!\n");
    return -1;
  }

  auto const &triple_hit = ExplicitCache.jit(3, mul, _1, 3);
  for(int v = 4; v != 6; ++v) {
    if(&triple != &triple_hit) {
      printf("explicit cache entry changed across hit!\n");
      return -1;
    }
    printf("triple(%d) is %d\n", v, triple_hit(v));
  }

  ExplicitCache.clear();
  if(ExplicitCache.has(3)) {
    printf("explicit cache still populated after clear!\n");
    return -1;
  }

  auto const &triple_recompiled = ExplicitCache.jit(3, mul, _1, 3);
  if(!ExplicitCache.has(3) || triple_recompiled(4) != 12) {
    printf("explicit cache did not repopulate after clear!\n");
    return -1;
  }

  return 0;
}
