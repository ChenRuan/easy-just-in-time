// LightBackend.cpp — see LightBackend.h.
//
// This is the only place in the runtime that knows about
// light_codegen/light_aarch64.*. Everything else goes through the
// TryLightCompile() entry point defined here.

#include "LightBackend.h"

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
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#ifndef EASYJIT_RUNTIME_DEBUG
#define EASYJIT_RUNTIME_DEBUG 0
#endif

#if EASYJIT_RUNTIME_DEBUG
#define EASYJIT_RT_LOG(...)                                                      \
  do {                                                                           \
    std::fprintf(stderr, "[easyjit][runtime] " __VA_ARGS__);                     \
    std::fflush(stderr);                                                         \
  } while (0)
#else
#define EASYJIT_RT_LOG(...) do { } while (0)
#endif

namespace easy {
namespace light_backend {

// ----------------------------------------------------------------- policy

Policy GetPolicyFromEnv() {
  const char *v = std::getenv("EASYJIT_LIGHT");
  if (!v || !*v) return Policy::Off;
  if (std::strcmp(v, "off")   == 0 || std::strcmp(v, "0") == 0) return Policy::Off;
  if (std::strcmp(v, "try")   == 0 ||
      std::strcmp(v, "auto")  == 0 ||
      std::strcmp(v, "1")     == 0) return Policy::Try;
  if (std::strcmp(v, "force") == 0) return Policy::Force;
  // Unknown values: conservative default. Do NOT silently engage.
  std::fprintf(stderr,
               "[easyjit/light] unknown EASYJIT_LIGHT='%s' (expected off|try|force); "
               "treating as 'off'\n", v);
  return Policy::Off;
}

// Runtime-observable trace, gated by EASYJIT_LIGHT_VERBOSE=1. This does
// NOT require rebuilding with EASYJIT_RUNTIME_DEBUG=1, so users can
// verify which backend served their call without a dev-mode runtime.
static bool VerboseEnabled() {
  const char *v = std::getenv("EASYJIT_LIGHT_VERBOSE");
  return v && *v && std::strcmp(v, "0") != 0;
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
    if (sz == 0) continue;
    if (sz > (1ull << 20)) continue; // 1 MiB cap; sanity guard

    std::unique_ptr<uint8_t[]> buf(new uint8_t[sz]());

    if (auto *CDS = dyn_cast<ConstantDataSequential>(Init)) {
      StringRef raw = CDS->getRawDataValues();
      if (raw.size() > sz) continue;
      std::memcpy(buf.get(), raw.data(), raw.size());
      // Trailing alloc-size padding is already zero from value-init.
    } else if (isa<ConstantAggregateZero>(Init)) {
      // buf is already zero-initialised.
    } else if (auto *CI = dyn_cast<ConstantInt>(Init)) {
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
static bool HostIsAArch64() {
  std::string HostTriple = llvm::sys::getProcessTriple();
  llvm::Triple T(HostTriple);
  return T.getArch() == llvm::Triple::aarch64 ||
         T.getArch() == llvm::Triple::aarch64_be;
}

// Convert easy::GlobalMapping (Name, Address) <-> light::GlobalSymbol
// (name, address). The struct layouts happen to be identical, but we
// copy explicitly so the runtime does not depend on that invariant.
static std::vector<::light::GlobalSymbol>
BuildLightGlobals(GlobalMapping *Globals, size_t &countOut) {
  std::vector<::light::GlobalSymbol> out;
  countOut = 0;
  if (!Globals) return out;
  for (GlobalMapping *GM = Globals; GM && GM->Name; ++GM) {
    ::light::GlobalSymbol s;
    s.name = GM->Name;
    s.address = GM->Address;
    out.push_back(s);
  }
  countOut = out.size();
  return out;
}

Report TryLightCompile(const char *Name,
                       GlobalMapping *Globals,
                       std::unique_ptr<llvm::LLVMContext> &Ctx,
                       std::unique_ptr<llvm::Module> &M,
                       std::unique_ptr<easy::Function> &out,
                       Policy policy) {
  Report rep;

  if (policy == Policy::Off) {
    rep.outcome = Outcome::SkippedByPolicy;
    return rep;
  }

  if (!Name || !M || !Ctx) {
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

  if (r.status != ::light::Status::Ok || !code) {
    EASYJIT_RT_LOG("[light] REJECTED fn=%s status=%d reason=%s\n",
                   Name, (int)r.status, r.reason.c_str());
    LIGHT_TRACE("fn=%s REJECTED (status=%d reason=%s) policy=%s\n",
                Name, (int)r.status, r.reason.c_str(), PolicyName(policy));
    // EASYJIT_LIGHT_VERBOSE=2 additionally dumps the rejected function
    // IR to stderr. Useful when diagnosing why a given case is not
    // accepted (e.g. comparing C++ front-end vs C API lowering).
    if (const char *v = std::getenv("EASYJIT_LIGHT_VERBOSE");
        v && std::strcmp(v, "2") == 0) {
      std::fprintf(stderr, "---- [easyjit/light] rejected IR (%s) ----\n", Name);
      std::fflush(stderr);
      std::string buf;
      llvm::raw_string_ostream os(buf);
      F->print(os);
      os.flush();
      std::fputs(buf.c_str(), stderr);
      std::fputs("---- end IR ----\n", stderr);
    }
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

  // Transfer ownership of the code page + module into the holder.
  const size_t pageSize = (size_t)sysconf(_SC_PAGESIZE);
  auto *holderRaw = new LightCodeHolder(code, pageSize, std::move(Ctx), std::move(M));
  holderRaw->dataBuffers_ = std::move(dataBufs);
  holderRaw->nameBuffers_ = std::move(nameBufs);
  std::unique_ptr<LLVMHolder> Holder(holderRaw);

  out.reset(new Function(code, std::move(Holder)));
  rep.outcome = Outcome::Succeeded;
  return rep;
}

} // namespace light_backend
} // namespace easy
