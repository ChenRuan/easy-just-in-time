#ifndef ASYNC_CACHE
#define ASYNC_CACHE

#include <easy/jit.h>
#include <easy/code_cache.h>
#include <tuple>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <chrono>

namespace easy {

// ---------------------------------------------------------------------------
// CompilationState: tracks the per-key JIT lifecycle
// ---------------------------------------------------------------------------
enum class CompilationState : int {
  not_started = 0,  // JIT has never been requested for this key
  compiling   = 1,  // a background thread is compiling
  ready       = 2   // the JIT'd function is available in cache
};

inline const char* to_string(CompilationState s) {
  switch (s) {
    case CompilationState::not_started: return "not_started";
    case CompilationState::compiling:   return "compiling";
    case CompilationState::ready:       return "ready";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// AsyncCallable<Fallback, WrapperTy>
//
// A lightweight value-type returned by AsyncCache::jit.
// It holds EITHER a pointer to the JIT'd FunctionWrapper (when ready)
// OR a raw fallback function pointer.  operator() dispatches accordingly.
// ---------------------------------------------------------------------------
template<class Fallback, class WrapperTy>
class AsyncCallable;

template<class Fallback, class Ret, class ... Params>
class AsyncCallable<Fallback, FunctionWrapper<Ret(Params...)>> {
public:
  using jit_wrapper_ty = FunctionWrapper<Ret(Params...)>;

  // Construct with JIT wrapper (ready path)
  explicit AsyncCallable(jit_wrapper_ty const *jit)
      : jit_(jit), fallback_(nullptr) {}

  // Construct with fallback pointer (compiling path)
  explicit AsyncCallable(Fallback fb)
      : jit_(nullptr), fallback_(fb) {}

  template<class ... Args>
  Ret operator()(Args&&... args) const {
    if (jit_) {
      return (*jit_)(std::forward<Args>(args)...);
    }
    return fallback_(std::forward<Args>(args)...);
  }

  /// Returns true if this callable dispatches to the JIT'd version.
  bool is_jit() const { return jit_ != nullptr; }

private:
  jit_wrapper_ty const *jit_;
  Fallback              fallback_;
};

namespace {

// ---------------------------------------------------------------------------
// AsyncCacheEntry – one per key, holds the compiled wrapper + state
// ---------------------------------------------------------------------------
struct AsyncCacheEntry {
  std::atomic<CompilationState> state{CompilationState::not_started};
  FunctionWrapperBase           wrapper;     // populated when state == ready
  std::atomic<void*>            raw_ptr{nullptr}; // fast-path raw pointer
  std::mutex                    mtx;         // guards the install step
};

// ---------------------------------------------------------------------------
// AsyncCacheBase – common machinery, templated on key type
// ---------------------------------------------------------------------------
template<class KeyTy>
class AsyncCacheBase {
public:
  using Key = KeyTy;

protected:
  // The map is only mutated during entry creation (cold path).
  // Once an entry exists, it is never removed, so read-only lookups
  // after all keys have been inserted are safe without a lock provided
  // no new keys are being added concurrently.
  //
  // We track whether the map is "sealed" (all keys created) with an
  // atomic flag.  After sealing, find_entry_fast() bypasses the mutex.
  std::unordered_map<Key, std::shared_ptr<AsyncCacheEntry>> cache_;
  std::mutex                                                 map_mtx_;
  std::atomic<bool>                                          sealed_{false};

  // Return (or create) the entry for `key`.
  std::shared_ptr<AsyncCacheEntry> get_or_create_entry(Key const &key) {
    std::lock_guard<std::mutex> lk(map_mtx_);
    auto it = cache_.find(key);
    if (it != cache_.end())
      return it->second;
    auto entry = std::make_shared<AsyncCacheEntry>();
    cache_.emplace(key, entry);
    return entry;
  }

  // Fast lookup – no mutex, returns raw pointer to entry.
  // Only safe when the caller knows the key has already been inserted
  // (i.e. after all keys have been triggered at least once).
  AsyncCacheEntry* find_entry_unsafe(Key const &key) {
    auto it = cache_.find(key);
    if (it != cache_.end())
      return it->second.get();
    return nullptr;
  }

  // Safe lookup – with mutex.
  AsyncCacheEntry* find_entry_safe(Key const &key) {
    std::lock_guard<std::mutex> lk(map_mtx_);
    auto it = cache_.find(key);
    if (it != cache_.end())
      return it->second.get();
    return nullptr;
  }

  // Try to transition entry from not_started → compiling.
  // Returns true exactly once (the winner that should spawn the JIT thread).
  static bool try_claim(AsyncCacheEntry *entry) {
    CompilationState expected = CompilationState::not_started;
    return entry->state.compare_exchange_strong(
        expected, CompilationState::compiling,
        std::memory_order_acq_rel, std::memory_order_acquire);
  }

  // Called by the background thread once JIT is done.
  static void install(std::shared_ptr<AsyncCacheEntry> const &entry,
                      FunctionWrapperBase &&wrapper) {
    std::lock_guard<std::mutex> lk(entry->mtx);
    entry->wrapper = std::move(wrapper);
    entry->raw_ptr.store(entry->wrapper.getRawPointer(),
                         std::memory_order_release);
    entry->state.store(CompilationState::ready, std::memory_order_release);
  }

  // Launch the JIT on a detached background thread.
  // All arguments are captured BY VALUE so the thread owns its data.
  template<class T, class ... Args>
  static void launch_jit(std::shared_ptr<AsyncCacheEntry> entry,
                         T fun, Args... args) {
    std::thread([entry, fun, args...]() mutable {
      auto compiled = easy::jit(fun, args...);
      install(entry, std::move(compiled));
    }).detach();
  }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// AsyncCache<Key> – explicit-key variant  (mirrors Cache<Key>)
//
// Design rationale
// ~~~~~~~~~~~~~~~~
// Unlike easy::Cache which returns a const-ref to a FunctionWrapper and
// blocks on JIT compilation, AsyncCache **never blocks the caller**.
//
// API:
//   easy::AsyncCache<int> AC;
//
//   // In hot loop:
//   int result = AC.jit(
//       key,
//       fallback_fn,            // raw C function ptr, same signature as JIT'd
//       jit_target_fn,          // function to JIT-compile
//       jit_arg1, jit_arg2, ... // forwarded to easy::jit
//   )(runtime_args...);
//
// On first call (not_started):
//   1. Atomically transitions state to `compiling`.
//   2. Spawns a detached thread that runs easy::jit and installs the result.
//   3. Returns the *fallback* callable immediately (zero JIT latency).
//
// On subsequent calls while compiling:
//   - Returns fallback – no duplicate JIT spawns.
//
// Once ready:
//   - Returns the JIT'd FunctionWrapper – identical to Cache fast-path.
//
// Concurrency guarantees:
//   - CAS on state ensures exactly one JIT thread per key.
//   - map_mtx_ serializes entry creation (rare, cold path).
//   - No mutex acquired on the hot "ready" path (atomic load only).
// ---------------------------------------------------------------------------
template<class Key>
class AsyncCache : public AsyncCacheBase<Key> {
  using Base = AsyncCacheBase<Key>;
public:

  /// jit – core API
  ///
  /// `Fallback` must be a raw function pointer whose type matches the
  /// *specialized* signature produced by easy::jit(JitFun, JitArgs...).
  ///
  /// Returns an AsyncCallable that wraps either the JIT'd function or
  /// the fallback, with the same operator() signature.
  ///
  /// HOT PATH (entry exists + ready):
  ///   - NO mutex, NO shared_ptr copy, just map.find + atomic load.
  ///   - Same cost as original Cache::jit.
  ///
  /// COLD PATH (first call for a key):
  ///   - Takes mutex to create entry, launches JIT thread.
  ///   - Only happens once per key.
  template<class Fallback, class JitFun, class ... JitArgs>
  auto EASY_JIT_COMPILER_INTERFACE
  jit(Key const &K,
      Fallback fallback,
      JitFun &&jit_fun, JitArgs&&... jit_args)
    -> AsyncCallable<Fallback,
         decltype(easy::jit(std::forward<JitFun>(jit_fun),
                            std::forward<JitArgs>(jit_args)...))>
  {
    using wrapper_ty = decltype(easy::jit(std::forward<JitFun>(jit_fun),
                                          std::forward<JitArgs>(jit_args)...));

    // ---- Fast path: lock-free lookup ----
    // Once all keys have been triggered at least once, the map is
    // never modified again.  We can skip the mutex entirely.
    // Even before sealing, for an *existing* key whose entry is
    // already in the map, a concurrent insert of a *different* key
    // could rehash the map.  So we only go lock-free after seal().
    if (Base::sealed_.load(std::memory_order_acquire)) {
      AsyncCacheEntry *e = Base::find_entry_unsafe(K);
      if (e && e->state.load(std::memory_order_acquire) == CompilationState::ready) {
        return AsyncCallable<Fallback, wrapper_ty>(
            &reinterpret_cast<wrapper_ty const &>(e->wrapper));
      }
      // Entry exists but still compiling (rare after seal), or
      // key not found (shouldn't happen after seal if used correctly).
      return AsyncCallable<Fallback, wrapper_ty>(fallback);
    }

    // ---- Slow path: mutex-protected lookup / create ----
    AsyncCacheEntry *e = Base::find_entry_safe(K);
    if (e) {
      CompilationState st = e->state.load(std::memory_order_acquire);
      if (st == CompilationState::ready) {
        return AsyncCallable<Fallback, wrapper_ty>(
            &reinterpret_cast<wrapper_ty const &>(e->wrapper));
      }
      // Already compiling – just return fallback, no duplicate spawn.
      return AsyncCallable<Fallback, wrapper_ty>(fallback);
    }

    // Entry doesn't exist yet – create it and launch JIT.
    auto entry_sp = Base::get_or_create_entry(K);
    e = entry_sp.get();
    CompilationState st = e->state.load(std::memory_order_acquire);

    if (st == CompilationState::not_started) {
      if (Base::try_claim(e)) {
        Base::launch_jit(entry_sp,
                         std::forward<JitFun>(jit_fun),
                         std::forward<JitArgs>(jit_args)...);
      }
    }
    return AsyncCallable<Fallback, wrapper_ty>(fallback);
  }

  /// seal – call after all keys have been triggered at least once.
  ///
  /// After this, jit() uses a lock-free fast path for
  /// map lookups.  The map will not be modified after seal().
  void seal() {
    Base::sealed_.store(true, std::memory_order_release);
  }

  /// Query the compilation state for a given key.
  CompilationState state(Key const &K) {
    std::lock_guard<std::mutex> lk(Base::map_mtx_);
    auto it = Base::cache_.find(K);
    if (it == Base::cache_.end())
      return CompilationState::not_started;
    return it->second->state.load(std::memory_order_acquire);
  }

  /// Check whether JIT is ready for a given key.
  bool ready(Key const &K) {
    return state(K) == CompilationState::ready;
  }

  /// Wait (blocking) until all currently-known keys reach `ready`.
  void wait_all() {
    while (true) {
      bool all_done = true;
      {
        std::lock_guard<std::mutex> lk(Base::map_mtx_);
        for (auto &kv : Base::cache_) {
          if (kv.second->state.load(std::memory_order_acquire)
              != CompilationState::ready) {
            all_done = false;
            break;
          }
        }
      }
      if (all_done) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  /// Wait for a specific key to reach `ready`.
  void wait(Key const &K) {
    auto entry = Base::get_or_create_entry(K);
    while (entry->state.load(std::memory_order_acquire)
           != CompilationState::ready) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  /// get_raw_pointer – zero-overhead hot-path accessor
  ///
  /// Once a key is known to be `ready`, this returns the raw JIT'd
  /// function pointer (void*).  The caller must cast it to the correct
  /// function-pointer type.
  ///
  /// Returns nullptr if the key doesn't exist or is not yet ready.
  void* get_raw_pointer(Key const &K) {
    AsyncCacheEntry *e = Base::sealed_.load(std::memory_order_acquire)
                           ? Base::find_entry_unsafe(K)
                           : Base::find_entry_safe(K);
    if (!e) return nullptr;
    if (e->state.load(std::memory_order_acquire) != CompilationState::ready)
      return nullptr;
    return e->raw_ptr.load(std::memory_order_acquire);
  }
};

} // namespace easy

#endif // ASYNC_CACHE
