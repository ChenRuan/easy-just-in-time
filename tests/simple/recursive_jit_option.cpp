// RUN: %clangxx %cxxflags %include_flags %ld_flags %s -Xclang -fpass-plugin=%lib_pass -o %t
// RUN: %t %t.default.ll %t.recursive.ll > %t.out
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --check-prefix=CHECK-DEFAULT %s < %t.default.ll
// RUN: %FileCheck --check-prefix=CHECK-RECURSIVE %s < %t.recursive.before.ll

#include <easy/jit.h>

#include <cstdio>

using namespace std::placeholders;

__attribute__((noinline))
int helper_default_native(int x) {
  return x * 2;
}

__attribute__((noinline))
int outer_default(int x) {
  return helper_default_native(x) + 1;
}

__attribute__((noinline))
int helper_recursive_jit(int x) {
  return x * 3;
}

__attribute__((noinline))
int outer_recursive(int x) {
  return helper_recursive_jit(x) + 1;
}

int main(int argc, char** argv) {
  auto default_fn = easy::jit(
      outer_default,
      _1,
      easy::options::dump_ir(argv[1]));

  auto recursive_fn = easy::jit(
      outer_recursive,
      _1,
      easy::options::recursive_jit(),
      easy::options::dump_ir(argv[2]));

  std::printf("default=%d recursive=%d\n", default_fn(5), recursive_fn(5));
  return 0;
}

// CHECK: default=11 recursive=16
// CHECK-DEFAULT-NOT: define {{.*}}@_Z21helper_default_nativei
// CHECK-DEFAULT: declare {{.*}}@_Z21helper_default_nativei
// CHECK-RECURSIVE: define {{.*}}@_Z20helper_recursive_jiti
