// RUN: %clangxx %cxxflags %include_flags %ld_flags %s -Xclang -fpass-plugin=%lib_pass -o %t
// RUN: %t %t.ll > %t.out
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --check-prefix=CHECK-IR %s < %t.ll

#include <easy/jit.h>

#include <cstdio>

using namespace std::placeholders;

__attribute__((noinline))
int helper_jit(int x) {
  return x * 2;
}

__attribute__((noinline))
int helper_native(int x) {
  return x * 3;
}

__attribute__((noinline))
int outer(int x) {
  int result = helper_jit(x);
  {
    EASY_JIT_KEEP_NATIVE_SCOPE();
    result += helper_native(x);
  }
  return result;
}

int main(int argc, char** argv) {
  auto fn = easy::jit(outer,
                      _1,
                      easy::options::recursive_jit(),
                      easy::options::dump_ir(argv[1]));
  printf("result=%d\n", fn(5));
  return 0;
}

// CHECK: result=25
// CHECK-IR-NOT: define {{.*}}@_Z13helper_nativei
// CHECK-IR: declare {{.*}}@_Z13helper_nativei
