#ifndef NOINLINE
#define NOINLINE

#define CI_SECTION "segment,compiler-if"
#define JIT_SECTION "segment,easy-jit"
#define LAYOUT_SECTION "segment,layout"

// mark functions in the easy::jit interface as no inline.
// it's easier for the pass to find the original functions to be jitted.
#define EASY_JIT_COMPILER_INTERFACE \
  __attribute__((noinline)) __attribute__((section(CI_SECTION)))

#define EASY_JIT_EXPOSE \
  __attribute__((section(JIT_SECTION)))

#define EASY_JIT_LAYOUT\
  __attribute__((section(LAYOUT_SECTION)))

#endif // NOINLINE
