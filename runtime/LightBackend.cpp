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
#include <llvm/Support/Host.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/Triple.h>

#include <sys/mman.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>
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
  const ::light::GlobalSymbol *symsP = syms.empty() ? nullptr : syms.data();

  EASYJIT_RT_LOG("[light] trying fn=%s policy=%s ngv=%zu triple=%s dl=%s\n",
                 Name, PolicyName(policy), nsyms,
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
  std::unique_ptr<LLVMHolder> Holder(
      new LightCodeHolder(code, pageSize, std::move(Ctx), std::move(M)));

  out.reset(new Function(code, std::move(Holder)));
  rep.outcome = Outcome::Succeeded;
  return rep;
}

} // namespace light_backend
} // namespace easy
