// LightBackend.cpp — see LightBackend.h.
//
// This is the only place in the runtime that knows about
// light_codegen/light_aarch64.*. Everything else goes through the
// TryLightCompile() entry point defined here.

#include "LightBackend.h"
#include "SreDebugLog.h"

#include <easy/runtime/BitcodeTracker.h>  // GlobalMapping
#include <easy/runtime/Function.h>
#include <easy/runtime/LLVMHolder.h>

#include "../light_codegen/light_aarch64.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/Triple.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#define EASYJIT_RT_LOG(...) EASYJIT_SRE_LOG("[runtime] " __VA_ARGS__)

namespace easy {
namespace light_backend {

// ----------------------------------------------------------------- policy

Policy GetPolicyFromEnv() {
  EASYJIT_RT_LOG("[light] GetPolicyFromEnv: debug/SRE path forces light backend\n");
  // Debug branch only: avoid std::getenv on the target SRE platform.  The
  // board has crashed in libc environment handling; this branch is used to
  // validate the light backend specifically, so force it directly.
  return Policy::Force;
}

// Runtime-observable trace, gated by EASYJIT_LIGHT_VERBOSE=1. This does
// NOT require rebuilding with EASYJIT_RUNTIME_DEBUG=1, so users can
// verify which backend served their call without a dev-mode runtime.
static bool VerboseEnabled() {
  return false;
}
#define LIGHT_TRACE(...)                                                           \
  do {                                                                             \
    if (VerboseEnabled()) {                                                        \
      std::fprintf(stderr, "[easyjit/light] " __VA_ARGS__);                        \
      std::fflush(stderr);                                                         \
    }                                                                              \
  } while (0)

const char *PolicyName(Policy p) {
  switch (p) {
    case Policy::Off:   return "off";
    case Policy::Try:   return "try";
    case Policy::Force: return "force";
  }
  return "?";
}

// ----------------------------------------------------------------- holder
// The light emitter returns a raw mmap'd RX page. We need an RAII holder
// so the page is munmap'd when the easy::Function is destroyed.
//
// Round-12: the holder also owns any data buffers materialized for
// PrivateLinkage GVs (e.g. `@__easy_snapshot_struct_array` produced by
// the bind_global_array IR rewrite, which is a private GV with a
// ConstantDataArray initializer baked into the IR). The light emitter
// can't generate code that reads those bytes from the IR directly; we
// allocate a heap buffer per GV, copy the initializer, and pass the
// buffer's host address as an extra GlobalSymbol. The buffers live as
// long as the compiled function does.

namespace {
class LightCodeHolder : public ::easy::LLVMHolder {
public:
  void  *page;
  size_t pageSize;

  // Keep the source context + optimized module alive *in addition to* the
  // code page. The EasyJIT public API (easy::FunctionWrapper::getLLVMModule)
  // exposes the module of a compiled function; some users rely on it being
  // a complete module. Matches LLVMHolderImpl's ownership pattern.
  std::unique_ptr<llvm::LLVMContext> Context_;
  std::unique_ptr<llvm::Module>      M_;

  // Owned data slabs for materialized PrivateLinkage GVs. Each entry's
  // address was published in the GlobalSymbol vector passed to the
  // emitter; freeing happens automatically here on destruction.
  std::vector<std::unique_ptr<uint8_t[]>> dataBuffers_;
  // Owned strdup'd names — GlobalSymbol::name is a const char*; we keep
  // the storage backing those pointers alive too (the original GV
  // names live in the Module/Context so this is a defensive copy).
  std::vector<std::unique_ptr<char[]>>    nameBuffers_;

  LightCodeHolder(void *p, size_t sz,
                  std::unique_ptr<llvm::LLVMContext> Ctx,
                  std::unique_ptr<llvm::Module> M)
    : page(p), pageSize(sz),
      Context_(std::move(Ctx)), M_(std::move(M)) {}

  // LLVMHolder virtual — expose the optimized Module so that
  // Function::getLLVMModule(), operator==, and std::hash<Function>
  // work uniformly for the light path (same as LLVMHolderImpl).
  llvm::Module* getModule() const override { return M_.get(); }

  ~LightCodeHolder() override {
    if (page && page != MAP_FAILED) {
      ::munmap(page, pageSize);
    }
  }
};
} // anonymous namespace

// Walk the module for PrivateLinkage GVs whose initializer is a
// ConstantData (raw byte buffer — ConstantDataArray, ConstantDataVector,
// or simple ConstantInt scalar). For each such GV we allocate a heap
// buffer, copy the initializer bytes into it (using the target
// DataLayout to size correctly), and append a GlobalSymbol mapping the
// GV's name to the buffer's host address.
//
// Why "PrivateLinkage with ConstantData": this is the precise shape
// EasyJIT's `bind_global_array` rewrite produces (see
// runtime/pass/InlineParametersHelper.cpp's GetRawByteArrayPointer).
// We deliberately do NOT touch ExternalLinkage GVs (those are real host
// globals already in EasyJIT's GlobalMapping table) or GVs with
// non-constant initialisers.
//
// Returns the number of newly added GVs. The data buffers + name backing
// storage are appended to the supplied vectors; on a successful compile
// they are moved into the LightCodeHolder so they outlive the call.
static size_t MaterializePrivateGlobals(llvm::Module &M,
                                        std::vector<::light::GlobalSymbol> &syms,
                                        std::vector<std::unique_ptr<uint8_t[]>> &dataBufs,
                                        std::vector<std::unique_ptr<char[]>>    &nameBufs) {
  using namespace llvm;
  size_t added = 0;
  const DataLayout &DL = M.getDataLayout();
  for (GlobalVariable &GV : M.globals()) {
    EASYJIT_RT_LOG("[light] MaterializePrivateGlobals: inspect gv=%s linkage=%u has_init=%d\n",
                   GV.getName().str().c_str(), (unsigned)GV.getLinkage(),
                   (int)GV.hasInitializer());
    if (!GV.hasInitializer()) continue;
    if (!GV.hasPrivateLinkage() && !GV.hasInternalLinkage()) continue;
    Constant *Init = GV.getInitializer();
    // Skip if already in the supplied user-globals table (by name).
    StringRef N = GV.getName();
    bool already = false;
    for (auto &s : syms) {
      if (s.name && N == s.name) { already = true; break; }
    }
    if (already) continue;

    // Only handle "raw byte" initializers for now. ConstantDataArray /
    // ConstantDataVector store bytes directly (DL alloc-size = element
    // count * element size). ConstantAggregateZero is also fine
    // (zero-fill of known size). Anything else (ConstantStruct,
    // ConstantArray of ConstantExpr, etc.) is left to the existing
    // const-fold fast-paths in the emitter; we don't need to (and don't
    // know how to) reify those into a flat byte buffer here.
    Type *EltTy = GV.getValueType();
    uint64_t sz = DL.getTypeAllocSize(EltTy);
    EASYJIT_RT_LOG("[light] MaterializePrivateGlobals: candidate gv=%s size=%llu\n",
                   N.str().c_str(), (unsigned long long)sz);
    if (sz == 0) continue;
    if (sz > (1ull << 20)) continue; // 1 MiB cap; sanity guard

    std::unique_ptr<uint8_t[]> buf(new uint8_t[sz]());

    if (auto *CDS = dyn_cast<ConstantDataSequential>(Init)) {
      StringRef raw = CDS->getRawDataValues();
      EASYJIT_RT_LOG("[light] MaterializePrivateGlobals: gv=%s ConstantDataSequential raw=%zu alloc=%llu\n",
                     N.str().c_str(), raw.size(), (unsigned long long)sz);
      if (raw.size() > sz) continue;
      std::memcpy(buf.get(), raw.data(), raw.size());
      // Trailing alloc-size padding is already zero from value-init.
    } else if (isa<ConstantAggregateZero>(Init)) {
      EASYJIT_RT_LOG("[light] MaterializePrivateGlobals: gv=%s zero aggregate\n",
                     N.str().c_str());
      // buf is already zero-initialised.
    } else if (auto *CI = dyn_cast<ConstantInt>(Init)) {
      EASYJIT_RT_LOG("[light] MaterializePrivateGlobals: gv=%s constant int width=%u\n",
                     N.str().c_str(), CI->getBitWidth());
      // Write the integer in HOST byte order. The reader is JIT'd code
      // doing LDR with target data endian; in the light backend the JIT
      // process always runs the code it produces (same SCTLR_EL1.EE),
      // so target data endian == host endian. memcpy of a host uint64_t
      // therefore lands the right byte pattern on both aarch64-* (LE
      // host) and aarch64_be-* (BE host). The previous `(v >> i*8)`
      // extraction was implicitly little-endian and would have placed
      // bytes in the wrong order on a BE host.
      uint64_t v = CI->getZExtValue();
      uint64_t nbytes = std::min<uint64_t>(sz, 8);
      std::memcpy(buf.get(), &v, (size_t)nbytes);
      // Trailing alloc-size padding (if sz > 8) is already zero from
      // value-init. ConstantInt scalar init is rare in practice (real
      // array data uses ConstantDataSequential, which already stores
      // host-endian raw bytes — see the CDS branch above).
    } else {
      EASYJIT_RT_LOG("[light] MaterializePrivateGlobals: skip gv=%s unsupported initializer\n",
                     N.str().c_str());
      continue; // structurally interesting initialiser; skip.
    }

    // Stash a stable name copy — GlobalSymbol::name is a const char*.
    std::unique_ptr<char[]> nameCopy(new char[N.size() + 1]);
    std::memcpy(nameCopy.get(), N.data(), N.size());
    nameCopy[N.size()] = '\0';

    ::light::GlobalSymbol s;
    s.name    = nameCopy.get();
    s.address = (const void *)buf.get();
    syms.push_back(s);
    EASYJIT_RT_LOG("[light] MaterializePrivateGlobals: add symbol %s -> %p\n",
                   s.name, s.address);

    dataBufs.push_back(std::move(buf));
    nameBufs.push_back(std::move(nameCopy));
    ++added;
  }
  return added;
}

// ----------------------------------------------------------------- impl

// Host-vs-module triple compatibility check. The light emitter's own
// triple check still runs (and is authoritative) inside light::emit; this
// check just prevents us from wasting time on obviously-wrong hosts
// (e.g. running x86_64 runtime with an aarch64 module) and from
// producing AArch64 code on a host that can't execute it.
//
// Do not use llvm::sys::getProcessTriple() here. In cross-built static
// bundles it can reflect the LLVM build/configuration triple rather than
// the target architecture of this runtime binary. The compiler target
// macros are the authoritative signal for whether this object file was
// built as executable AArch64 code (LE or BE).
static bool HostIsAArch64() {
#if defined(__aarch64__) || defined(__arm64__)
  EASYJIT_RT_LOG("[light] HostIsAArch64: yes\n");
  return true;
#else
  EASYJIT_RT_LOG("[light] HostIsAArch64: no\n");
  return false;
#endif
}

// ----------------------------------------------------------- code dump
//
// EASYJIT_LIGHT_DUMP_CODE_DIR=<dir>   write each accepted function's
//                                     raw machine-code bytes to
//                                     <dir>/NNNN_<sanitized_name>.bin
// EASYJIT_LIGHT_DUMP_META=1           print one stderr line per dump:
//                                     [easyjit][light] code name=<n>
//                                     bytes=<n> file=<path>
//
// The dump is best-effort: failure to mkdir / open / write only emits
// a one-line warning to stderr and never aborts the JIT. POSIX
// open/write are used so this works in light-only builds that do not
// link against <filesystem>.

static std::string SanitizeForFilename(const char *Name) {
  std::string out;
  if (!Name || !*Name) {
    out = "unknown";
    return out;
  }
  out.reserve(64);
  for (const char *p = Name; *p; ++p) {
    unsigned char c = (unsigned char)*p;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == '-' || c == '+';
    out.push_back(ok ? (char)c : '_');
    if (out.size() >= 96) break; // keep file names sane
  }
  if (out.empty()) out = "unknown";
  return out;
}

// Try to create `dir` (mkdir -p style, last component only — we expect
// the parent path to already exist on the target). Returns true on
// success or if the directory already exists.
static bool EnsureDumpDir(const char *dir) {
  if (!dir || !*dir) return false;
  if (::mkdir(dir, 0755) == 0) return true;
  if (errno == EEXIST) {
    struct stat st;
    if (::stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) return true;
  }
  return false;
}

// Best-effort dump. Never throws, never aborts.
static void MaybeDumpLightCode(const char *Name, const void *Code,
                               size_t Bytes) {
  (void)Name;
  (void)Code;
  (void)Bytes;
  EASYJIT_RT_LOG("[light] MaybeDumpLightCode: skipped on debug/SRE path\n");
  return;
#if 0
  const char *dir = std::getenv("EASYJIT_LIGHT_DUMP_CODE_DIR");
  if (!dir || !*dir || !Code || Bytes == 0) return;

  if (!EnsureDumpDir(dir)) {
    std::fprintf(stderr,
                 "[easyjit][light] warning: cannot create dump dir '%s'"
                 " (errno=%d), skipping code dump for %s\n",
                 dir, errno, Name ? Name : "<null>");
    return;
  }

  // Atomic counter so multiple threads producing dumps don't clobber.
  static std::atomic<unsigned> Counter{0};
  unsigned idx = Counter.fetch_add(1, std::memory_order_relaxed) + 1;

  const char *basePrefix = std::getenv("EASYJIT_LIGHT_DUMP_CODE_BASENAME");
  char filename[512];
  std::snprintf(filename, sizeof(filename), "%s/%s%04u_%s.bin",
                dir,
                (basePrefix && *basePrefix) ? basePrefix : "",
                idx,
                SanitizeForFilename(Name).c_str());

  int fd = ::open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    std::fprintf(stderr,
                 "[easyjit][light] warning: open('%s') failed errno=%d,"
                 " skipping code dump for %s\n",
                 filename, errno, Name ? Name : "<null>");
    return;
  }

  const uint8_t *p = (const uint8_t *)Code;
  size_t left = Bytes;
  bool wrote_ok = true;
  while (left > 0) {
    ssize_t n = ::write(fd, p, left);
    if (n < 0) {
      if (errno == EINTR) continue;
      wrote_ok = false;
      break;
    }
    if (n == 0) { wrote_ok = false; break; }
    p += (size_t)n;
    left -= (size_t)n;
  }
  ::close(fd);

  if (!wrote_ok) {
    std::fprintf(stderr,
                 "[easyjit][light] warning: write('%s') failed (errno=%d,"
                 " %zu/%zu bytes), dump may be truncated\n",
                 filename, errno, Bytes - left, Bytes);
    return;
  }

  if (const char *meta = std::getenv("EASYJIT_LIGHT_DUMP_META");
      meta && *meta && std::strcmp(meta, "0") != 0) {
    std::fprintf(stderr,
                 "[easyjit][light] code name=%s bytes=%zu file=%s\n",
                 Name ? Name : "<null>", Bytes, filename);
    std::fflush(stderr);
  }
#endif
}

// Convert easy::GlobalMapping (Name, Address) <-> light::GlobalSymbol
// (name, address). The struct layouts happen to be identical, but we
// copy explicitly so the runtime does not depend on that invariant.
static std::vector<::light::GlobalSymbol>
BuildLightGlobals(GlobalMapping *Globals, size_t &countOut) {
  std::vector<::light::GlobalSymbol> out;
  countOut = 0;
  EASYJIT_RT_LOG("[light] BuildLightGlobals: globals=%p\n", (void*)Globals);
  if (!Globals) return out;
  for (GlobalMapping *GM = Globals; GM && GM->Name; ++GM) {
    ::light::GlobalSymbol s;
    s.name = GM->Name;
    s.address = GM->Address;
    out.push_back(s);
    EASYJIT_RT_LOG("[light] BuildLightGlobals: map %s -> %p\n",
                   s.name ? s.name : "<null>", s.address);
  }
  countOut = out.size();
  EASYJIT_RT_LOG("[light] BuildLightGlobals: count=%zu\n", countOut);
  return out;
}

Report TryLightCompile(const char *Name,
                       GlobalMapping *Globals,
                       std::unique_ptr<llvm::LLVMContext> &Ctx,
                       std::unique_ptr<llvm::Module> &M,
                       std::unique_ptr<easy::Function> &out,
                       Policy policy) {
  Report rep;
  EASYJIT_RT_LOG("[light] TryLightCompile: begin name=%s globals=%p ctx_ref=%p module_ref=%p out_ref=%p policy=%s\n",
                 Name ? Name : "<null>", (void*)Globals, (void*)Ctx.get(),
                 (void*)M.get(), (void*)&out, PolicyName(policy));

  if (policy == Policy::Off) {
    EASYJIT_RT_LOG("[light] TryLightCompile: policy off\n");
    rep.outcome = Outcome::SkippedByPolicy;
    return rep;
  }

  if (!Name || !M || !Ctx) {
    EASYJIT_RT_LOG("[light] TryLightCompile: null input name=%p module=%p ctx=%p\n",
                   (const void*)Name, (void*)M.get(), (void*)Ctx.get());
    rep.outcome = Outcome::SkippedByPolicy;
    rep.reason  = "null name/module/context";
    return rep;
  }

  // Host arch gate. On non-aarch64 hosts we never attempt the light path,
  // regardless of policy -- there is no meaningful "force" semantics when
  // we literally cannot execute the emitted instructions.
  if (!HostIsAArch64()) {
    EASYJIT_RT_LOG("[light] host is not aarch64*, skipping (policy=%s)\n",
                   PolicyName(policy));
    rep.outcome = (policy == Policy::Force) ? Outcome::FailedForce
                                            : Outcome::SkippedByPolicy;
    rep.reason  = "host is not aarch64";
    return rep;
  }

  llvm::Function *F = M->getFunction(Name);
  if (!F || F->isDeclaration() || F->empty()) {
    EASYJIT_RT_LOG("[light] function '%s' not defined in module, skipping\n", Name);
    rep.outcome = (policy == Policy::Force) ? Outcome::FailedForce
                                            : Outcome::Unsupported;
    rep.reason  = "function not in module";
    return rep;
  }

  // Build a light-globals view from EasyJIT's own mapping table.
  size_t nsyms = 0;
  std::vector<::light::GlobalSymbol> syms = BuildLightGlobals(Globals, nsyms);
  EASYJIT_RT_LOG("[light] TryLightCompile: after globals nsyms=%zu\n", nsyms);

  // Reify any PrivateLinkage GVs whose initializer is raw byte data
  // (e.g. the @__easy_snapshot_struct_array buffer that
  // bind_global_array's IR rewrite plants in the module). The light
  // backend only knows how to address GVs through its globals table —
  // so we materialize the IR-embedded constant bytes into heap buffers
  // here, register them under the GV's name, and let the emitter
  // resolve them just like any user-bound global. Buffer ownership is
  // moved into the LightCodeHolder on success.
  std::vector<std::unique_ptr<uint8_t[]>> dataBufs;
  std::vector<std::unique_ptr<char[]>>    nameBufs;
  size_t addedPriv = MaterializePrivateGlobals(*M, syms, dataBufs, nameBufs);
  nsyms = syms.size();
  (void)addedPriv;
  const ::light::GlobalSymbol *symsP = syms.empty() ? nullptr : syms.data();

  EASYJIT_RT_LOG("[light] trying fn=%s policy=%s ngv=%zu (priv+%zu) triple=%s dl=%s\n",
                 Name, PolicyName(policy), nsyms, addedPriv,
                 M->getTargetTriple().c_str(),
                 M->getDataLayoutStr().c_str());

  ::light::Result r;
  void *code = ::light::compile(*F, r, symsP, nsyms);
  EASYJIT_RT_LOG("[light] TryLightCompile: light::compile returned code=%p status=%d reason=%s bytes=%zu\n",
                 code, (int)r.status, r.reason.c_str(), r.codeBytes);

  if (r.status != ::light::Status::Ok || !code) {
    EASYJIT_RT_LOG("[light] REJECTED fn=%s status=%d reason=%s\n",
                   Name, (int)r.status, r.reason.c_str());
    LIGHT_TRACE("fn=%s REJECTED (status=%d reason=%s) policy=%s\n",
                Name, (int)r.status, r.reason.c_str(), PolicyName(policy));
    // EASYJIT_LIGHT_VERBOSE=2 additionally dumps the rejected function
    // IR to stderr. Useful when diagnosing why a given case is not
    // accepted (e.g. comparing C++ front-end vs C API lowering).
    EASYJIT_RT_LOG("[light] rejected IR dump skipped on debug/SRE path\n");
    rep.reason = r.reason.empty() ? std::string("light emitter rejected function")
                                  : r.reason;
    rep.outcome = (policy == Policy::Force) ? Outcome::FailedForce
                                            : Outcome::Unsupported;
    return rep;
  }

  EASYJIT_RT_LOG("[light] ACCEPTED fn=%s code=%p bytes=%zu\n",
                 Name, code, r.codeBytes);
  LIGHT_TRACE("fn=%s ACCEPTED bytes=%zu policy=%s\n",
              Name, r.codeBytes, PolicyName(policy));

  // Optional diagnostic: dump raw machine-code bytes for later
  // inspection. Controlled by EASYJIT_LIGHT_DUMP_CODE_DIR; see helper
  // comment above. Best-effort, never aborts the JIT.
  MaybeDumpLightCode(Name, code, r.codeBytes);

  // Transfer ownership of the code page + module into the holder.
  // Must match light::compile's debug/SRE fixed allocation size.  Avoid
  // sysconf(_SC_PAGESIZE) on the target platform; it has crashed through
  // libc relocation paths.
  const size_t codeSize = 4096u * 4u;
  auto *holderRaw = new LightCodeHolder(code, codeSize, std::move(Ctx), std::move(M));
  EASYJIT_RT_LOG("[light] TryLightCompile: holder=%p codeSize=%zu dataBufs=%zu nameBufs=%zu\n",
                 (void*)holderRaw, codeSize, dataBufs.size(), nameBufs.size());
  holderRaw->dataBuffers_ = std::move(dataBufs);
  holderRaw->nameBuffers_ = std::move(nameBufs);
  std::unique_ptr<LLVMHolder> Holder(holderRaw);

  out.reset(new Function(code, std::move(Holder)));
  EASYJIT_RT_LOG("[light] TryLightCompile: success function=%p raw=%p\n",
                 (void*)out.get(), code);
  rep.outcome = Outcome::Succeeded;
  return rep;
}

} // namespace light_backend
} // namespace easy
