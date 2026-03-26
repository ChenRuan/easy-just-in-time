#ifndef CACHE
#define CACHE

#include <easy/jit.h>
#include <tuple>
#include <unordered_map>

namespace easy {

namespace {
using AutoKey = std::pair<void*, easy::Context>;

template<class KeyTy>
class CacheBase {

  public:

  using Key = KeyTy;

  protected:

  std::unordered_map<Key, easy::FunctionWrapperBase> Cache_;
  using iterator = typename std::unordered_map<Key, easy::FunctionWrapperBase>::iterator;

  template<class T, class ... Args>
  auto const & compile_if_not_in_cache(std::pair<iterator, bool> &CacheEntry, T &&Fun, Args&& ... args) {
    using wrapper_ty = decltype(easy::jit(std::forward<T>(Fun), std::forward<Args>(args)...));

    FunctionWrapperBase &FWB = CacheEntry.first->second;
    if(CacheEntry.second) {
      auto FW = easy::jit(std::forward<T>(Fun), std::forward<Args>(args)...);
      FWB = std::move(FW);
    }
    return reinterpret_cast<wrapper_ty&>(FWB);
  }
};
}

template<class Key = AutoKey>
class Cache : public CacheBase<Key> {
  public:

  template<class T, class ... Args>
  auto const& EASY_JIT_COMPILER_INTERFACE jit(Key const &K, T &&Fun, Args&& ... args) {
    auto CacheIter = CacheBase<Key>::Cache_.find(K);
    std::pair<typename CacheBase<Key>::iterator, bool> CacheEntry;
    if(CacheIter != CacheBase<Key>::Cache_.end()) {
      CacheEntry = std::make_pair(CacheIter, false);
    } else {
      CacheEntry = CacheBase<Key>::Cache_.emplace(
          std::piecewise_construct,
          std::forward_as_tuple(K),
          std::forward_as_tuple());
    }
    return CacheBase<Key>::compile_if_not_in_cache(CacheEntry, std::forward<T>(Fun), std::forward<Args>(args)...);
  }

  template<class T, class ... Args>
  auto const& EASY_JIT_COMPILER_INTERFACE jit(Key &&K, T &&Fun, Args&& ... args) {
    auto CacheIter = CacheBase<Key>::Cache_.find(K);
    std::pair<typename CacheBase<Key>::iterator, bool> CacheEntry;
    if(CacheIter != CacheBase<Key>::Cache_.end()) {
      CacheEntry = std::make_pair(CacheIter, false);
    } else {
      CacheEntry = CacheBase<Key>::Cache_.emplace(
          std::piecewise_construct,
          std::forward_as_tuple(std::move(K)),
          std::forward_as_tuple());
    }
    return CacheBase<Key>::compile_if_not_in_cache(CacheEntry, std::forward<T>(Fun), std::forward<Args>(args)...);
  }

  bool has(Key const &K) const {
    auto const CacheEntry = CacheBase<Key>::Cache_.find(K);
    return CacheEntry != CacheBase<Key>::Cache_.end();
  }
};


template<>
class Cache<AutoKey> : public CacheBase<AutoKey> {
  public:

  template<class T, class ... Args>
  auto const& EASY_JIT_COMPILER_INTERFACE jit(T &&Fun, Args&& ... args) {
    void* FunPtr = reinterpret_cast<void*>(meta::get_as_pointer(Fun));
    Key CacheKey(FunPtr, get_context_for<T, Args...>(std::forward<Args>(args)...));
    auto CacheIter = CacheBase<Key>::Cache_.find(CacheKey);
    std::pair<typename CacheBase<Key>::iterator, bool> CacheEntry;
    if(CacheIter != CacheBase<Key>::Cache_.end()) {
      CacheEntry = std::make_pair(CacheIter, false);
    } else {
      CacheEntry = CacheBase<Key>::Cache_.emplace(
          std::piecewise_construct,
          std::forward_as_tuple(std::move(CacheKey)),
          std::forward_as_tuple());
    }
    return CacheBase<Key>::compile_if_not_in_cache(CacheEntry, std::forward<T>(Fun), std::forward<Args>(args)...);
  }

  template<class T, class ... Args>
  bool has(T &&Fun, Args&& ... args) const {
    void* FunPtr = reinterpret_cast<void*>(meta::get_as_pointer(Fun));
    auto const CacheEntry =
        CacheBase<Key>::Cache_.find(Key(FunPtr,
                    get_context_for<T, Args...>(std::forward<Args>(args)...)));
    return CacheEntry != Cache_.end();
  }
};

}

#endif // CACHE
