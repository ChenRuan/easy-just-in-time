#ifndef NOINLINE
#define NOINLINE

#define CI_SECTION "segment,compiler-if"
#define JIT_SECTION "segment,easy-jit"
#define LAYOUT_SECTION "segment,layout"
#define KEEP_NATIVE_SECTION "easy-jit-keep-native"

// mark functions in the easy::jit interface as no inline.
// it's easier for the pass to find the original functions to be jitted.
#define EASY_JIT_COMPILER_INTERFACE \
  __attribute__((noinline)) __attribute__((section(CI_SECTION)))

#define EASY_JIT_EXPOSE \
  __attribute__((section(JIT_SECTION)))

#define EASY_JIT_LAYOUT\
  __attribute__((section(LAYOUT_SECTION)))

#define EASY_JIT_KEEP_NATIVE_INTERFACE \
  __attribute__((noinline)) __attribute__((section(KEEP_NATIVE_SECTION)))

#ifdef __cplusplus
namespace easy {
namespace detail {

EASY_JIT_KEEP_NATIVE_INTERFACE
inline void keep_native_scope_begin() noexcept {
  asm volatile("" ::: "memory");
}

EASY_JIT_KEEP_NATIVE_INTERFACE
inline void keep_native_scope_end() noexcept {
  asm volatile("" ::: "memory");
}

struct KeepNativeScope {
  KeepNativeScope() noexcept { keep_native_scope_begin(); }
  ~KeepNativeScope() noexcept { keep_native_scope_end(); }
};

} // namespace detail
} // namespace easy

#define EASY_JIT_DETAIL_CONCAT_INNER(a, b) a##b
#define EASY_JIT_DETAIL_CONCAT(a, b) EASY_JIT_DETAIL_CONCAT_INNER(a, b)

#define EASY_JIT_KEEP_NATIVE_SCOPE() \
  ::easy::detail::KeepNativeScope \
      EASY_JIT_DETAIL_CONCAT(__easyjit_keep_native_scope_, __LINE__)
#endif

#endif // NOINLINE
