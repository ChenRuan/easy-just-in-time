#pragma once

#include <cstdio>

extern "C" int SRE_printf(const char *fmt, ...) __attribute__((weak));

#define EASYJIT_SRE_LOG(...) do { } while (0)
