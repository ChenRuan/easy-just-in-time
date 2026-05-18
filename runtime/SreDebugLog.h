#pragma once

#include <cstdio>

extern "C" int SRE_printf(const char *fmt, ...) __attribute__((weak));

#define EASYJIT_SRE_LOG(...)                                                    \
  do {                                                                         \
    if (SRE_printf) {                                                          \
      SRE_printf("[easyjit][sre] " __VA_ARGS__);                               \
    } else {                                                                   \
      std::fprintf(stderr, "[easyjit][sre] " __VA_ARGS__);                     \
      std::fflush(stderr);                                                     \
    }                                                                          \
  } while (0)
