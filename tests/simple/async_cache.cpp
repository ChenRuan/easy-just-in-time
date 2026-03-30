// RUN: %clangxx %cxxflags %include_flags %ld_flags %s -Xclang -fpass-plugin=%lib_pass -lpthread -o %t
// RUN: %t > %t.out
// RUN: %FileCheck %s < %t.out

#include <easy/async_code_cache.h>

#include <cstdio>
#include <functional>

using namespace std::placeholders;

int add(int a, int b) {
  return a + b;
}

int fallback_add_one(int a) {
  return add(a, 1);
}

int main() {
  easy::AsyncCache<int> cache;

  auto first = cache.jit(7, fallback_add_one, add, _1, 1);
  std::printf("phase1: state=%s is_jit=%d value=%d\n",
              easy::to_string(cache.state(7)),
              first.is_jit(),
              first(41));

  cache.wait(7);

  auto second = cache.jit(7, fallback_add_one, add, _1, 1);
  std::printf("phase2: state=%s is_jit=%d value=%d ready=%d\n",
              easy::to_string(cache.state(7)),
              second.is_jit(),
              second(41),
              cache.ready(7));

  cache.seal();
  auto third = cache.jit(7, fallback_add_one, add, _1, 1);
  std::printf("phase3: sealed=%d is_jit=%d value=%d\n",
              cache.ready(7),
              third.is_jit(),
              third(41));

  return 0;
}

// CHECK: phase1: state=compiling is_jit=0 value=42
// CHECK: phase2: state=ready is_jit=1 value=42 ready=1
// CHECK: phase3: sealed=1 is_jit=1 value=42
