#include <cstdint>
#include <cstdlib>
#include <new>

// Central replacement point for process-wide C++ allocation hooks used by
// EasyJIT's static/runtime builds. Keep the public operator signatures stable;
// compile this file only when EASYJIT_DEFINE_GLOBAL_NEW_DELETE=1. Define
// EASYJIT_USE_CUSTOM_NEW_DELETE=1 and replace the custom branch below when
// wiring in a platform allocator.
#ifndef EASYJIT_DEFINE_GLOBAL_NEW_DELETE
#define EASYJIT_DEFINE_GLOBAL_NEW_DELETE 1
#endif
#ifndef EASYJIT_USE_CUSTOM_NEW_DELETE
#define EASYJIT_USE_CUSTOM_NEW_DELETE 0
#endif

#if EASYJIT_DEFINE_GLOBAL_NEW_DELETE

#if EASYJIT_USE_CUSTOM_NEW_DELETE
extern "C" void *XXX_MemAlloc(unsigned int ulSidPid, unsigned char ucptNo,
                              unsigned long ulSize);
extern "C" unsigned int XXX_MemFree(unsigned int ulSidPid, void *pAddr);
#endif

static void *easyjit_allocate_or_null(std::size_t size) noexcept {
  if (size == 0) {
    size = 1;
  }
#if EASYJIT_USE_CUSTOM_NEW_DELETE
  return XXX_MemAlloc(0U, 0U, static_cast<unsigned long>(size));
#else
  return std::malloc(size);
#endif
}

void *operator new(std::size_t size) {
  while (true) {
    if (void *p = easyjit_allocate_or_null(size)) {
      return p;
    }

    std::new_handler handler = std::get_new_handler();
    if (!handler) {
      std::abort();
    }
    handler();
  }
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  return easyjit_allocate_or_null(size);
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  return easyjit_allocate_or_null(size);
}

void operator delete(void *p) noexcept {
#if EASYJIT_USE_CUSTOM_NEW_DELETE
  if (p) {
    (void)XXX_MemFree(0, p);
  }
#else
  std::free(p);
#endif
}

void operator delete[](void *p) noexcept {
  ::operator delete(p);
}

void operator delete(void *p, std::size_t) noexcept {
  ::operator delete(p);
}

void operator delete[](void *p, std::size_t) noexcept {
  ::operator delete[](p);
}

void operator delete(void *p, const std::nothrow_t &) noexcept {
  ::operator delete(p);
}

void operator delete[](void *p, const std::nothrow_t &) noexcept {
  ::operator delete[](p);
}

#if __cpp_aligned_new
void *operator new(std::size_t size, std::align_val_t alignment) {
  if (size == 0) {
    size = 1;
  }

  const std::size_t align = static_cast<std::size_t>(alignment);
  while (true) {
    void *p = nullptr;
#if EASYJIT_USE_CUSTOM_NEW_DELETE
    // Replace this with the platform aligned allocation hook if the board
    // requires over-aligned C++ objects. The current custom API has no
    // alignment argument, so it is only safe when it guarantees align bytes.
    p = XXX_MemAlloc(0U, 0U, static_cast<unsigned long>(size));
    if (p && reinterpret_cast<std::uintptr_t>(p) % align == 0) {
#else
    if (posix_memalign(&p, align, size) == 0) {
#endif
      return p;
    }

#if EASYJIT_USE_CUSTOM_NEW_DELETE
    if (p) {
      (void)XXX_MemFree(0, p);
    }
#endif

    std::new_handler handler = std::get_new_handler();
    if (!handler) {
      std::abort();
    }
    handler();
  }
}

void *operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}

void *operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t &) noexcept {
  void *p = nullptr;
#if EASYJIT_USE_CUSTOM_NEW_DELETE
  p = easyjit_allocate_or_null(size);
  const std::size_t align = static_cast<std::size_t>(alignment);
  if (p && reinterpret_cast<std::uintptr_t>(p) % align != 0) {
    (void)XXX_MemFree(0, p);
    p = nullptr;
  }
#else
  if (posix_memalign(&p, static_cast<std::size_t>(alignment),
                     size == 0 ? 1 : size) != 0)
    p = nullptr;
#endif
  return p;
}

void *operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t &) noexcept {
  return ::operator new(size, alignment, std::nothrow);
}

void operator delete(void *p, std::align_val_t) noexcept {
  ::operator delete(p);
}

void operator delete[](void *p, std::align_val_t) noexcept {
  ::operator delete[](p);
}

void operator delete(void *p, std::size_t, std::align_val_t) noexcept {
  ::operator delete(p);
}

void operator delete[](void *p, std::size_t, std::align_val_t) noexcept {
  ::operator delete[](p);
}

void operator delete(void *p, std::align_val_t,
                     const std::nothrow_t &) noexcept {
  ::operator delete(p);
}

void operator delete[](void *p, std::align_val_t,
                       const std::nothrow_t &) noexcept {
  ::operator delete[](p);
}
#endif

#endif // EASYJIT_DEFINE_GLOBAL_NEW_DELETE
