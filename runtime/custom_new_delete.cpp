#include <cstdint>
#include <cstdlib>
#include <cstring>
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
extern "C" void *XXX_MemAlloc(unsigned int ulSidPid, unsigned char ucptNo,
                              unsigned long ulSize);
extern "C" unsigned int XXX_MemFree(unsigned int ulSidPid, void *pAddr);
#endif

#if EASYJIT_USE_CUSTOM_NEW_DELETE
namespace {
struct EasyJitMallocHeader {
  std::size_t Size;
  std::uintptr_t Magic;
};

constexpr std::uintptr_t kEasyJitMallocMagic =
    static_cast<std::uintptr_t>(0x454a49544d414c4cULL); // "EJITMALL"

void *easyjit_malloc_impl(std::size_t size, const char *tag) {
  if (size == 0) {
    size = 1;
  }
  const std::size_t total = sizeof(EasyJitMallocHeader) + size;
  EASYJIT_ALLOC_LOG("%s begin size=%zu total=%zu\n", tag, size, total);
  void *raw = XXX_MemAlloc(0U, 0U, static_cast<unsigned long>(total));
  EASYJIT_ALLOC_LOG("%s after XXX_MemAlloc raw=%p size=%zu total=%zu\n",
                    tag, raw, size, total);
  if (!raw) {
    return nullptr;
  }
  auto *header = static_cast<EasyJitMallocHeader *>(raw);
  header->Size = size;
  header->Magic = kEasyJitMallocMagic;
  void *user = header + 1;
  EASYJIT_ALLOC_LOG("%s success user=%p raw=%p size=%zu\n",
                    tag, user, raw, size);
  easyjit_debug_probe_allocation(user, size, tag);
  return user;
}

EasyJitMallocHeader *easyjit_header_from_user(void *ptr) {
  if (!ptr) {
    return nullptr;
  }
  auto *header = static_cast<EasyJitMallocHeader *>(ptr) - 1;
  if (header->Magic != kEasyJitMallocMagic) {
    EASYJIT_ALLOC_LOG("malloc header magic mismatch ptr=%p header=%p magic=%zx\n",
                      ptr, (void *)header, static_cast<std::size_t>(header->Magic));
    return nullptr;
  }
  return header;
}
} // namespace

extern "C" void *malloc(std::size_t size) {
  return easyjit_malloc_impl(size, "malloc");
}

extern "C" void *calloc(std::size_t count, std::size_t size) {
  EASYJIT_ALLOC_LOG("calloc begin count=%zu size=%zu\n", count, size);
  if (count && size > static_cast<std::size_t>(-1) / count) {
    EASYJIT_ALLOC_LOG("calloc overflow count=%zu size=%zu\n", count, size);
    return nullptr;
  }
  std::size_t total = count * size;
  void *p = easyjit_malloc_impl(total, "calloc");
  if (p) {
    EASYJIT_ALLOC_LOG("calloc before memset ptr=%p total=%zu\n", p, total);
    std::memset(p, 0, total);
    EASYJIT_ALLOC_LOG("calloc after memset ptr=%p total=%zu\n", p, total);
  }
  return p;
}

extern "C" void free(void *ptr) {
  EASYJIT_ALLOC_LOG("free begin ptr=%p\n", ptr);
  if (!ptr) {
    return;
  }
  EasyJitMallocHeader *header = easyjit_header_from_user(ptr);
  if (!header) {
    EASYJIT_ALLOC_LOG("free ignore unknown ptr=%p\n", ptr);
    return;
  }
  header->Magic = 0;
  EASYJIT_ALLOC_LOG("free before XXX_MemFree ptr=%p raw=%p size=%zu\n",
                    ptr, (void *)header, header->Size);
  (void)XXX_MemFree(0, header);
  EASYJIT_ALLOC_LOG("free after XXX_MemFree ptr=%p\n", ptr);
}

extern "C" void *realloc(void *ptr, std::size_t size) {
  EASYJIT_ALLOC_LOG("realloc begin ptr=%p size=%zu\n", ptr, size);
  if (!ptr) {
    return malloc(size);
  }
  if (size == 0) {
    free(ptr);
    return nullptr;
  }

  EasyJitMallocHeader *header = easyjit_header_from_user(ptr);
  if (!header) {
    EASYJIT_ALLOC_LOG("realloc unknown old ptr=%p\n", ptr);
    return nullptr;
  }
  std::size_t oldSize = header->Size;
  void *newPtr = malloc(size);
  if (!newPtr) {
    return nullptr;
  }
  std::memcpy(newPtr, ptr, oldSize < size ? oldSize : size);
  free(ptr);
  EASYJIT_ALLOC_LOG("realloc success old=%p new=%p old_size=%zu new_size=%zu\n",
                    ptr, newPtr, oldSize, size);
  return newPtr;
}
#endif

void *operator new(std::size_t size) {
  EASYJIT_ALLOC_LOG("operator new begin size=%zu\n", size);
  if (size == 0) {
    size = 1;
  }

  while (true) {
#if EASYJIT_USE_CUSTOM_NEW_DELETE
    EASYJIT_ALLOC_LOG("operator new before XXX_MemAlloc size=%zu\n", size);
    if (void *p = XXX_MemAlloc(0U, 0U, static_cast<unsigned long>(size))) {
#else
    EASYJIT_ALLOC_LOG("operator new before malloc size=%zu\n", size);
    if (void *p = std::malloc(size)) {
#endif
      EASYJIT_ALLOC_LOG("operator new success size=%zu ptr=%p\n", size, p);
      easyjit_debug_probe_allocation(p, size, "operator new");
      return p;
    }

    EASYJIT_ALLOC_LOG("operator new allocation failed size=%zu\n", size);
    std::new_handler handler = std::get_new_handler();
    if (!handler) {
      EASYJIT_ALLOC_LOG("operator new throwing bad_alloc size=%zu\n", size);
      throw std::bad_alloc();
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
  try {
    void *p = ::operator new(size);
    EASYJIT_ALLOC_LOG("operator new nothrow success size=%zu ptr=%p\n", size, p);
    return p;
  } catch (...) {
    EASYJIT_ALLOC_LOG("operator new nothrow failed size=%zu\n", size);
    return nullptr;
  }
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  EASYJIT_ALLOC_LOG("operator new[] nothrow begin size=%zu\n", size);
  try {
    void *p = ::operator new[](size);
    EASYJIT_ALLOC_LOG("operator new[] nothrow success size=%zu ptr=%p\n", size, p);
    return p;
  } catch (...) {
    EASYJIT_ALLOC_LOG("operator new[] nothrow failed size=%zu\n", size);
    return nullptr;
  }
}

void operator delete(void *p) noexcept {
  EASYJIT_ALLOC_LOG("operator delete ptr=%p\n", p);
#if EASYJIT_USE_CUSTOM_NEW_DELETE
  if (p) {
    EASYJIT_ALLOC_LOG("operator delete before XXX_MemFree ptr=%p\n", p);
    (void)XXX_MemFree(0, p);
    EASYJIT_ALLOC_LOG("operator delete after XXX_MemFree ptr=%p\n", p);
  }
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
    EASYJIT_ALLOC_LOG("operator new aligned before XXX_MemAlloc size=%zu align=%zu\n",
                      size, align);
    p = XXX_MemAlloc(0U, 0U, static_cast<unsigned long>(size));
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
      (void)XXX_MemFree(0, p);
    }
#endif

    EASYJIT_ALLOC_LOG("operator new aligned allocation failed size=%zu align=%zu\n",
                      size, align);
    std::new_handler handler = std::get_new_handler();
    if (!handler) {
      EASYJIT_ALLOC_LOG("operator new aligned throwing bad_alloc size=%zu align=%zu\n",
                        size, align);
      throw std::bad_alloc();
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
  try {
    void *p = ::operator new(size, alignment);
    EASYJIT_ALLOC_LOG("operator new aligned nothrow success size=%zu align=%zu ptr=%p\n",
                      size, static_cast<std::size_t>(alignment), p);
    return p;
  } catch (...) {
    EASYJIT_ALLOC_LOG("operator new aligned nothrow failed size=%zu align=%zu\n",
                      size, static_cast<std::size_t>(alignment));
    return nullptr;
  }
}

void *operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t &) noexcept {
  EASYJIT_ALLOC_LOG("operator new[] aligned nothrow begin size=%zu align=%zu\n",
                    size, static_cast<std::size_t>(alignment));
  try {
    void *p = ::operator new[](size, alignment);
    EASYJIT_ALLOC_LOG("operator new[] aligned nothrow success size=%zu align=%zu ptr=%p\n",
                      size, static_cast<std::size_t>(alignment), p);
    return p;
  } catch (...) {
    EASYJIT_ALLOC_LOG("operator new[] aligned nothrow failed size=%zu align=%zu\n",
                      size, static_cast<std::size_t>(alignment));
    return nullptr;
  }
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
