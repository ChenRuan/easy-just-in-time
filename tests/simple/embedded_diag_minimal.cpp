// RUN: %clangxx %cxxflags %include_flags %ld_flags %s -Xclang -fpass-plugin=%lib_pass -o %t

#include <easy/jit.h>

#include <cstdio>
#include <cstdlib>
#include <functional>

using namespace std::placeholders;

struct SmallConfig {
  int enabled;
  int gain;
  int bias;
};

static int eval_small_cpp(int x, SmallConfig const &cfg) {
  if (cfg.enabled) {
    return x + cfg.gain;
  }
  return x - cfg.bias;
}

static void usage(char const *argv0) {
  std::printf("Usage: %s <mode>\n", argv0);
  std::printf("  0  print usage\n");
  std::printf("  1  baseline only\n");
  std::printf("  2  snapshot + jit compile only\n");
  std::printf("  3  snapshot + jit compile + execute\n");
  std::fflush(stdout);
}

int main(int argc, char **argv) {
  int mode = argc > 1 ? std::atoi(argv[1]) : 0;
  SmallConfig cfg{1, 7, 99};

  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);

  std::printf("[cpp-diag] start mode=%d sizeof(SmallConfig)=%zu\n", mode, sizeof(SmallConfig));

  if (mode == 0) {
    usage(argv[0]);
    return 0;
  }

  if (mode == 1) {
    std::printf("[cpp-diag] baseline begin\n");
    std::printf("[cpp-diag] baseline result=%d\n", eval_small_cpp(5, cfg));
    return 0;
  }

  std::printf("[cpp-diag] before easy::jit\n");
  auto f = easy::jit(eval_small_cpp, _1, easy::snapshot(cfg));
  std::printf("[cpp-diag] after easy::jit\n");

  if (mode == 2) {
    std::printf("[cpp-diag] compile-only ok\n");
    return 0;
  }

  if (mode == 3) {
    std::printf("[cpp-diag] before execute\n");
    std::printf("[cpp-diag] execute result=%d\n", f(5));
    std::printf("[cpp-diag] execute ok\n");
    return 0;
  }

  std::fprintf(stderr, "[cpp-diag] unknown mode=%d\n", mode);
  usage(argv[0]);
  return 1;
}
