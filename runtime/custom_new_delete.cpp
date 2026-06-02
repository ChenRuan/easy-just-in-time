#include <cstdint>
#include <cstdlib>
#include <new>

#include "SreDebugLog.h"

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

#define EASYJIT_ALLOC_LOG(...) EASYJIT_SRE_LOG("[alloc] " __VA_ARGS__)

static void easyjit_debug_probe_allocation(void *p, std::size_t size,
                                           const char *tag) {
  EASYJIT_ALLOC_LOG("%s probe begin ptr=%p size=%zu\n", tag, p, size);
  if (!p || size == 0) {
    EASYJIT_ALLOC_LOG("%s probe skip ptr=%p size=%zu\n", tag, p, size);
    return;
  }
  volatile unsigned char *bytes = static_cast<volatile unsigned char *>(p);
  volatile unsigned char first = bytes[0];
  volatile unsigned char last = bytes[size - 1];
  bytes[0] = first;
  bytes[size - 1] = last;
  EASYJIT_ALLOC_LOG("%s probe ok ptr=%p size=%zu first=%02x last=%02x\n",
                    tag, p, size, (unsigned)first, (unsigned)last);
}

#if EASYJIT_USE_CUSTOM_NEW_DELETE
extern "C" void *SRE_MemAlloc(unsigned int ulSidPid, unsigned char ucptNo,
                              unsigned long ulSize) __attribute__((weak));
extern "C" unsigned int SRE_MemFree(unsigned int ulSidPid,
                                    void *pAddr) __attribute__((weak));
extern "C" void *XXX_MemAlloc(unsigned int ulSidPid, unsigned char ucptNo,
                              unsigned long ulSize) __attribute__((weak));
extern "C" unsigned int XXX_MemFree(unsigned int ulSidPid,
                                    void *pAddr) __attribute__((weak));
#endif

static void *easyjit_allocate_or_null(std::size_t size) noexcept {
  if (size == 0) {
    size = 1;
  }
#if EASYJIT_USE_CUSTOM_NEW_DELETE
  if (SRE_MemAlloc)
    return SRE_MemAlloc(0U, 0U, static_cast<unsigned long>(size));
  if (XXX_MemAlloc)
    return XXX_MemAlloc(0U, 0U, static_cast<unsigned long>(size));
  return nullptr;
#else
  return std::malloc(size);
#endif
}

#if EASYJIT_USE_CUSTOM_NEW_DELETE
static void easyjit_platform_free(void *p, const char *tag) noexcept {
  if (!p)
    return;
  if (SRE_MemFree) {
    EASYJIT_ALLOC_LOG("%s before SRE_MemFree ptr=%p\n", tag, p);
    (void)SRE_MemFree(0, p);
    EASYJIT_ALLOC_LOG("%s after SRE_MemFree ptr=%p\n", tag, p);
  } else if (XXX_MemFree) {
    EASYJIT_ALLOC_LOG("%s fallback before XXX_MemFree ptr=%p\n", tag, p);
    (void)XXX_MemFree(0, p);
    EASYJIT_ALLOC_LOG("%s fallback after XXX_MemFree ptr=%p\n", tag, p);
  } else {
    EASYJIT_ALLOC_LOG("%s no platform free hook ptr=%p\n", tag, p);
  }
}
#endif

void *operator new(std::size_t size) {
  EASYJIT_ALLOC_LOG("operator new begin size=%zu\n", size);
  while (true) {
    EASYJIT_ALLOC_LOG("operator new before alloc size=%zu\n", size);
    if (void *p = easyjit_allocate_or_null(size)) {
      EASYJIT_ALLOC_LOG("operator new success size=%zu ptr=%p\n", size, p);
      easyjit_debug_probe_allocation(p, size, "operator new");
      return p;
    }

    EASYJIT_ALLOC_LOG("operator new allocation failed size=%zu\n", size);
    std::new_handler handler = std::get_new_handler();
    if (!handler) {
      EASYJIT_ALLOC_LOG("operator new aborting allocation failed size=%zu\n", size);
      std::abort();
    }
    EASYJIT_ALLOC_LOG("operator new calling new_handler size=%zu handler=%p\n",
                      size, reinterpret_cast<void *>(handler));
    handler();
  }
}

void *operator new[](std::size_t size) {
  EASYJIT_ALLOC_LOG("operator new[] begin size=%zu\n", size);
  return ::operator new(size);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  EASYJIT_ALLOC_LOG("operator new nothrow begin size=%zu\n", size);
  if (void *p = easyjit_allocate_or_null(size)) {
    EASYJIT_ALLOC_LOG("operator new nothrow success size=%zu ptr=%p\n", size, p);
    return p;
  }
  EASYJIT_ALLOC_LOG("operator new nothrow failed size=%zu\n", size);
  return nullptr;
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  EASYJIT_ALLOC_LOG("operator new[] nothrow begin size=%zu\n", size);
  if (void *p = easyjit_allocate_or_null(size)) {
    EASYJIT_ALLOC_LOG("operator new[] nothrow success size=%zu ptr=%p\n", size, p);
    return p;
  }
  EASYJIT_ALLOC_LOG("operator new[] nothrow failed size=%zu\n", size);
  return nullptr;
}

void operator delete(void *p) noexcept {
  EASYJIT_ALLOC_LOG("operator delete ptr=%p\n", p);
#if EASYJIT_USE_CUSTOM_NEW_DELETE
  easyjit_platform_free(p, "operator delete");
#else
  std::free(p);
#endif
}

void operator delete[](void *p) noexcept {
  EASYJIT_ALLOC_LOG("operator delete[] ptr=%p\n", p);
  ::operator delete(p);
}

void operator delete(void *p, std::size_t size) noexcept {
  EASYJIT_ALLOC_LOG("operator delete sized ptr=%p size=%zu\n", p, size);
  ::operator delete(p);
}

void operator delete[](void *p, std::size_t size) noexcept {
  EASYJIT_ALLOC_LOG("operator delete[] sized ptr=%p size=%zu\n", p, size);
  ::operator delete[](p);
}

void operator delete(void *p, const std::nothrow_t &) noexcept {
  EASYJIT_ALLOC_LOG("operator delete nothrow ptr=%p\n", p);
  ::operator delete(p);
}

void operator delete[](void *p, const std::nothrow_t &) noexcept {
  EASYJIT_ALLOC_LOG("operator delete[] nothrow ptr=%p\n", p);
  ::operator delete[](p);
}

#if __cpp_aligned_new
void *operator new(std::size_t size, std::align_val_t alignment) {
  EASYJIT_ALLOC_LOG("operator new aligned begin size=%zu align=%zu\n",
                    size, static_cast<std::size_t>(alignment));
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
    EASYJIT_ALLOC_LOG("operator new aligned before platform alloc size=%zu align=%zu\n",
                      size, align);
    p = easyjit_allocate_or_null(size);
    if (p && reinterpret_cast<std::uintptr_t>(p) % align == 0) {
#else
    EASYJIT_ALLOC_LOG("operator new aligned before posix_memalign size=%zu align=%zu\n",
                      size, align);
    if (posix_memalign(&p, align, size) == 0) {
#endif
      EASYJIT_ALLOC_LOG("operator new aligned success size=%zu align=%zu ptr=%p mod=%zu\n",
                        size, align, p,
                        p ? reinterpret_cast<std::uintptr_t>(p) % align : 0);
      easyjit_debug_probe_allocation(p, size, "operator new aligned");
      return p;
    }

#if EASYJIT_USE_CUSTOM_NEW_DELETE
    if (p) {
      EASYJIT_ALLOC_LOG("operator new aligned free misaligned ptr=%p align=%zu mod=%zu\n",
                        p, align, reinterpret_cast<std::uintptr_t>(p) % align);
      easyjit_platform_free(p, "operator new aligned misaligned");
    }
#endif

    EASYJIT_ALLOC_LOG("operator new aligned allocation failed size=%zu align=%zu\n",
                      size, align);
    std::new_handler handler = std::get_new_handler();
    if (!handler) {
      EASYJIT_ALLOC_LOG("operator new aligned aborting allocation failed size=%zu align=%zu\n",
                        size, align);
      std::abort();
    }
    EASYJIT_ALLOC_LOG("operator new aligned calling new_handler size=%zu align=%zu handler=%p\n",
                      size, align, reinterpret_cast<void *>(handler));
    handler();
  }
}

void *operator new[](std::size_t size, std::align_val_t alignment) {
  EASYJIT_ALLOC_LOG("operator new[] aligned begin size=%zu align=%zu\n",
                    size, static_cast<std::size_t>(alignment));
  return ::operator new(size, alignment);
}

void *operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t &) noexcept {
  EASYJIT_ALLOC_LOG("operator new aligned nothrow begin size=%zu align=%zu\n",
                    size, static_cast<std::size_t>(alignment));
  void *p = nullptr;
#if EASYJIT_USE_CUSTOM_NEW_DELETE
  p = easyjit_allocate_or_null(size);
  if (p && reinterpret_cast<std::uintptr_t>(p) %
               static_cast<std::size_t>(alignment) != 0) {
    easyjit_platform_free(p, "operator new aligned nothrow misaligned");
    p = nullptr;
  }
#else
  if (posix_memalign(&p, static_cast<std::size_t>(alignment),
                     size == 0 ? 1 : size) != 0)
    p = nullptr;
#endif
  if (p) {
    EASYJIT_ALLOC_LOG("operator new aligned nothrow success size=%zu align=%zu ptr=%p\n",
                      size, static_cast<std::size_t>(alignment), p);
    return p;
  }
  EASYJIT_ALLOC_LOG("operator new aligned nothrow failed size=%zu align=%zu\n",
                    size, static_cast<std::size_t>(alignment));
  return nullptr;
}

void *operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t &) noexcept {
  EASYJIT_ALLOC_LOG("operator new[] aligned nothrow begin size=%zu align=%zu\n",
                    size, static_cast<std::size_t>(alignment));
  return ::operator new(size, alignment, std::nothrow);
}

void operator delete(void *p, std::align_val_t) noexcept {
  EASYJIT_ALLOC_LOG("operator delete aligned ptr=%p\n", p);
  ::operator delete(p);
}

void operator delete[](void *p, std::align_val_t) noexcept {
  EASYJIT_ALLOC_LOG("operator delete[] aligned ptr=%p\n", p);
  ::operator delete[](p);
}

void operator delete(void *p, std::size_t size, std::align_val_t alignment) noexcept {
  EASYJIT_ALLOC_LOG("operator delete sized aligned ptr=%p size=%zu align=%zu\n",
                    p, size, static_cast<std::size_t>(alignment));
  ::operator delete(p);
}

void operator delete[](void *p, std::size_t size, std::align_val_t alignment) noexcept {
  EASYJIT_ALLOC_LOG("operator delete[] sized aligned ptr=%p size=%zu align=%zu\n",
                    p, size, static_cast<std::size_t>(alignment));
  ::operator delete[](p);
}

void operator delete(void *p, std::align_val_t,
                     const std::nothrow_t &) noexcept {
  EASYJIT_ALLOC_LOG("operator delete aligned nothrow ptr=%p\n", p);
  ::operator delete(p);
}

void operator delete[](void *p, std::align_val_t,
                       const std::nothrow_t &) noexcept {
  EASYJIT_ALLOC_LOG("operator delete[] aligned nothrow ptr=%p\n", p);
  ::operator delete[](p);
}
#endif

#endif // EASYJIT_DEFINE_GLOBAL_NEW_DELETE
