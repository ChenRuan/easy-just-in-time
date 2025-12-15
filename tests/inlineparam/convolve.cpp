// RUN: %clangxx %cxxflags %include_flags %ld_flags %s -Xclang -fpass-plugin=%lib_pass -o %t
// RUN: %t > %t.out

#include <easy/jit.h>

#include <functional>
#include <vector>
#include <cstdio>

using namespace std::placeholders;

void __attribute__((noinline)) kernel(int n, int m, int * image, int const * mask, int* out) {
  int i = 0;
  int j = 0;
  int k = 0;
  int l = 0;
  out[i * (n-m+1) + j] += image[(i+k) * n + j+l] * mask[k *m + l];
}

void test_convolve() {
  std::vector<int> image(16*16,0);
  std::vector<int> out((16-3)*(16-3),0);
  std::vector<int> out_nonjit((16-3)*(16-3), 0);

  static const int mask[3][3] = {{1,2,3},{0,0,0},{3,2,1}};

  auto my_kernel = easy::jit(kernel, 16, 3, _1, &mask[0][0], _2);

  my_kernel(image.data(), out.data());

  kernel(16, 3, image.data(), &mask[0][0], out_nonjit.data());

  for(int i = 0; i != out.size(); ++i) {
    if(out[i] != out_nonjit[i]) {
      printf("%d != %d\n", out[i], out_nonjit[i]);
      exit(-1);
    }
  }
}

int main() {
  test_convolve();
  return 0;
}
  