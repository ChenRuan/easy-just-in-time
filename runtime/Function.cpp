#ifndef EASYJIT_LIGHT_BACKEND_ONLY
#define EASYJIT_LIGHT_BACKEND_ONLY 0
#endif

#include <easy/runtime/BitcodeTracker.h>
#include <easy/runtime/Function.h>
#include <easy/runtime/RuntimePasses.h>
#include <easy/runtime/LLVMHolderImpl.h>
#include "SreDebugLog.h"
#if !EASYJIT_LIGHT_BACKEND_ONLY
#include <easy/runtime/MinimalOrcJIT.h>
#endif
#include <easy/runtime/Utils.h>
#include <easy/exceptions.h>

#if EASYJIT_LIGHT_BACKEND_ENABLED
#include "LightBackend.h"
#endif

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils.h>
#if !EASYJIT_LIGHT_BACKEND_ONLY
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#endif
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/Error.h>
#if !EASYJIT_LIGHT_BACKEND_ONLY
#include <llvm/Target/TargetMachine.h>
#include <llvm/MC/TargetRegistry.h>
#endif
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#ifdef NDEBUG
#include <llvm/IR/Verifier.h>
#endif

#define EASYJIT_RT_LOG(...) EASYJIT_SRE_LOG("[runtime] " __VA_ARGS__)


using namespace easy;

extern void* __dso_handle;

namespace easy {
  DefineEasyException(JITCreateError, "Failed to create ORC JIT for:");
  DefineEasyException(CouldNotOpenFile, "Failed to file to dump intermediate representation.");
  DefineEasyException(TargetMachineCreateError, "Failed to create target machine for:");
  DefineEasyException(TargetLookupError, "Failed to lookup target for:");
  DefineEasyException(SymbolLookupError, "Failed to lookup JIT symbol for:");
  // Light-only / LightBackend specific failure. Distinct from JITCreateError
  // (which is reserved for the ORC fallback path) so users / log scrapers
  // can tell the two apart and we never claim to have tried ORC when the
  // runtime was compiled without it.
  DefineEasyException(LightBackendCompileError,
                      "Light backend cannot compile function: ");
}

Function::Function(void* Addr, std::unique_ptr<LLVMHolder> H)
  : Address(Addr), Holder(std::move(H)) {
}

namespace {
class OriginalFunctionHolder : public easy::LLVMHolder {
public:
  explicit OriginalFunctionHolder(std::string Reason)
      : Reason_(std::move(Reason)) {}

  llvm::Module* getModule() const override { return nullptr; }

  std::string const& reason() const { return Reason_; }

private:
  std::string Reason_;
};

static bool CanFallbackToOriginalFunction(easy::Context const& C,
                                          llvm::Module const& M,
                                          const char* Name) {
  EASYJIT_RT_LOG("CanFallbackToOriginalFunction: begin name=%s ctx_size=%zu\n",
                 Name ? Name : "<null>", C.size());
  if (!Name)
    return false;

  llvm::Function *F = M.getFunction(Name);
  if (!F) {
    EASYJIT_RT_LOG("CanFallbackToOriginalFunction: no function\n");
    return false;
  }

  if (C.size() != F->arg_size()) {
    EASYJIT_RT_LOG("CanFallbackToOriginalFunction: arg mismatch ctx=%zu fn=%u\n",
                   C.size(), (unsigned)F->arg_size());
    return false;
  }

  for (size_t I = 0; I < C.size(); ++I) {
    auto const *Forward = C.getArgumentMapping(I).as<easy::ForwardArgument>();
    if (!Forward || Forward->get() != I) {
      EASYJIT_RT_LOG("CanFallbackToOriginalFunction: arg %zu not identity forward\n", I);
      return false;
    }
  }

  EASYJIT_RT_LOG("CanFallbackToOriginalFunction: yes\n");
  return true;
}

static std::unique_ptr<easy::Function>
MakeOriginalFunctionFallback(void *Addr, std::string Reason) {
  EASYJIT_RT_LOG("Function::Compile: using original function fallback: %s\n",
                 Reason.c_str());
  if (const char *Verbose = std::getenv("EASYJIT_LIGHT_VERBOSE");
      Verbose && Verbose[0] != '\0' && Verbose[0] != '0') {
    std::fprintf(stderr, "[easyjit/light] fallback: %s\n", Reason.c_str());
    std::fflush(stderr);
  }
  std::unique_ptr<easy::LLVMHolder> Holder(
      new OriginalFunctionHolder(std::move(Reason)));
  return std::unique_ptr<easy::Function>(
      new easy::Function(Addr, std::move(Holder)));
}
} // namespace

#if !EASYJIT_LIGHT_BACKEND_ONLY
static std::unique_ptr<llvm::TargetMachine> GetTargetMachineForModule(llvm::Module const& M) {
  std::string TripleStr = M.getTargetTriple();
  if (TripleStr.empty()) {
    TripleStr = llvm::sys::getProcessTriple();
  }

  llvm::Triple Triple(TripleStr);
  std::string Error;
  const llvm::Target* Target = llvm::TargetRegistry::lookupTarget(TripleStr, Error);

  EASYJIT_RT_LOG("GetTargetMachineForModule: module_triple=%s effective_triple=%s cpu=%s\n",
                 M.getTargetTriple().c_str(),
                 TripleStr.c_str(),
                 llvm::sys::getHostCPUName().str().c_str());

  if (!Target) {
    EASYJIT_RT_LOG("GetTargetMachineForModule: lookup failed error=%s\n", Error.c_str());
    throw easy::TargetLookupError(TripleStr.c_str());
  }

  llvm::TargetOptions Options;
  std::unique_ptr<llvm::TargetMachine> TM(
      Target->createTargetMachine(TripleStr,
                                  llvm::sys::getHostCPUName().str(),
                                  "",
                                  Options,
                                  llvm::Reloc::PIC_,
                                  llvm::CodeModel::Small,
                                  llvm::CodeGenOpt::Aggressive));
  EASYJIT_RT_LOG("GetTargetMachineForModule: result=%p\n", (void*)TM.get());
  return TM;
}
#endif // !EASYJIT_LIGHT_BACKEND_ONLY

static void Optimize(llvm::Module& M, const char* Name, const easy::Context& C, unsigned OptLevel, unsigned OptSize) {

  std::string TripleStr = M.getTargetTriple();
  if (TripleStr.empty()) {
    TripleStr = llvm::sys::getProcessTriple();
  }
  llvm::Triple Triple{TripleStr};
  EASYJIT_RT_LOG("Optimize: begin name=%s triple=%s opt=%u size=%u module_triple=%s datalayout=%s\n",
                 Name ? Name : "<null>",
                 TripleStr.c_str(),
                 OptLevel,
                 OptSize,
                 M.getTargetTriple().c_str(),
                 M.getDataLayoutStr().c_str());

#if !EASYJIT_LIGHT_BACKEND_ONLY
  std::unique_ptr<llvm::TargetMachine> TM = GetTargetMachineForModule(M);
  if (!TM) {
    EASYJIT_RT_LOG("Optimize: target machine creation failed for %s\n", Name ? Name : "<null>");
    throw easy::TargetMachineCreateError(Name);
  }
  M.setTargetTriple(TripleStr);
  M.setDataLayout(TM->createDataLayout());
  EASYJIT_RT_LOG("Optimize: adjusted module triple=%s datalayout=%s\n",
                 M.getTargetTriple().c_str(),
                 M.getDataLayoutStr().c_str());
#else
  // Light-only: no LLVMCodeGen / LLVMMC available. Keep the module's
  // existing triple/DataLayout (set by clang at -emit-llvm time) and
  // skip TargetMachine-derived analyses (TTI). The lightweight pass
  // pipeline below works without TTI.
  if (M.getTargetTriple().empty())
    M.setTargetTriple(TripleStr);
  EASYJIT_RT_LOG("Optimize: light-only mode, using module triple=%s datalayout=%s\n",
                 M.getTargetTriple().c_str(),
                 M.getDataLayoutStr().c_str());
#endif

  // Lightweight optimization pipeline (synced from llvm15_trim).
  //
  // We replace the legacy O2/O3 PassManagerBuilder pipeline with an
  // explicit, minimal sequence that exactly covers what EasyJIT needs
  // to fold the bound snapshot constants:
  //
  //   ContextAnalysis        - bind easy::Context to the module
  //   InlineParameters       - rewrite the wrapper to feed in the
  //                            captured argument constants
  //   DevirtualizeConstant   - rewrite indirect calls when the
  //                            callee is now a known constant
  //   FunctionInlining       - inline wrapper -> original (critical)
  //   ConstStructPropagate   - custom lightweight pass that walks
  //                            alloca -> store(GEP, const) -> load(GEP)
  //                            chains, propagates the constant to the
  //                            load, folds resulting branches and ICmps,
  //                            and removes dead code. Replaces
  //                            SROA + SCCP + InstCombine + Reassociate
  //                            + ADCE for the snapshot pattern.
  //   mem2reg                - lift remaining non-struct allocas to SSA
  //   ConstStructPropagate   - second round, picks up constants that
  //                            mem2reg exposed
  //   InstCombine            - cheap canonical cleanup after specialization
  //   LoopSimplify/LCSSA     - canonical form for loop transforms (Opt>=2)
  //   LoopRotate             - canonical do-while shape (Opt>=2)
  //   LoopUnroll             - fully unroll loops whose trip count became
  //                            a compile-time constant after specialization
  //                            (Opt>=2). Light backend friendly: produces
  //                            straight-line scalar IR, no vector IR.
  //   InstCombine (2nd)      - cleanup after unroll exposes new constants
  //   CFGSimplification      - prune now-dead branches/blocks
  //   Internalize            - hide everything but the JIT entry
  //   GlobalDCE              - drop now-unreachable globals/functions
  //   StripDeadPrototypes    - drop dangling external decls
  //
  // Snapshot/InlineParameters/DevirtualizeConstant/global mapping
  // mechanics are unchanged; only the *cleanup* pipeline shrinks.
  // LLJIT/ORC backend (CreateJIT, CompileAndWrap, MapGlobals) is
  // unchanged.

  llvm::legacy::PassManager MPM;
  EASYJIT_RT_LOG("Optimize: add TargetTransformInfo pass\n");
#if !EASYJIT_LIGHT_BACKEND_ONLY
  MPM.add(llvm::createTargetTransformInfoWrapperPass(TM->getTargetIRAnalysis()));
#else
  // Light-only: use a default (no-target) TargetIRAnalysis. The custom
  // ConstStructPropagate pass and InstCombine-free pipeline do not rely
  // on accurate cost modeling.
  MPM.add(llvm::createTargetTransformInfoWrapperPass(llvm::TargetIRAnalysis()));
#endif
  EASYJIT_RT_LOG("Optimize: add ContextAnalysis pass ctx_size=%zu\n", C.size());
  MPM.add(easy::createContextAnalysisPass(C));
  EASYJIT_RT_LOG("Optimize: add InlineParameters pass target=%s\n", Name ? Name : "<null>");
  MPM.add(easy::createInlineParametersPass(Name));
  EASYJIT_RT_LOG("Optimize: add DevirtualizeConstant pass target=%s\n", Name ? Name : "<null>");
  MPM.add(easy::createDevirtualizeConstantPass(Name));
  EASYJIT_RT_LOG("Optimize: add FunctionInlining pass opt=%u size=%u\n", OptLevel, OptSize);
  // Inline the wrapper -> original-function call (critical).
  MPM.add(llvm::createFunctionInliningPass(OptLevel, OptSize, false));
  // Custom lightweight propagator: alloca/store/GEP/load -> const.
  EASYJIT_RT_LOG("Optimize: add ConstStructPropagate pass #1 target=%s\n", Name ? Name : "<null>");
  MPM.add(easy::createConstStructPropagatePass(Name));
  // Promote remaining allocas to SSA.
  EASYJIT_RT_LOG("Optimize: add mem2reg pass\n");
  MPM.add(llvm::createPromoteMemoryToRegisterPass());
  // Second round picks up constants exposed by mem2reg.
  EASYJIT_RT_LOG("Optimize: add ConstStructPropagate pass #2 target=%s\n", Name ? Name : "<null>");
  MPM.add(easy::createConstStructPropagatePass(Name));
  // Canonicalize simple arithmetic and casts after constants are exposed.
  EASYJIT_RT_LOG("Optimize: add InstCombine pass\n");
  MPM.add(llvm::createInstructionCombiningPass());
  // Scalar loop unroll (light backend friendly).
  //
  // EasyJIT specialization typically turns runtime-fixed loop bounds
  // (e.g. cfg->n bound to a constant via snapshot/InlineParameters /
  // ConstStructPropagate) into compile-time constants. Once the trip
  // count is statically known, LLVM's legacy LoopUnroll pass can fully
  // unroll the loop into straight-line scalar IR -- which is exactly
  // the shape the light AArch64 backend can lower (it does not support
  // vector IR or NEON). We only enable this at OptLevel >= 2 so a
  // user explicitly opting into -O0/-O1 sees the original loop shape.
  //
  // We do NOT add the LoopVectorize / SLPVectorize passes -- the light
  // backend cannot consume vector IR. Users with vectorizer-friendly
  // hot loops should compile their JIT-relevant TUs with
  //   -fno-vectorize -fno-slp-vectorize
  // see runtime/LightBackend_LIMITATIONS.md.
  if (OptLevel >= 2) {
    EASYJIT_RT_LOG("Optimize: add loop canonicalization/unroll passes\n");
    MPM.add(llvm::createLoopSimplifyPass());
    MPM.add(llvm::createLCSSAPass());
    MPM.add(llvm::createLoopRotatePass());
    MPM.add(llvm::createLoopUnrollPass(
        /*OptLevel=*/(int)OptLevel,
        /*OnlyWhenForced=*/false,
        /*ForgetAllSCEV=*/false));
    // NOTE: a 2nd InstCombine here would further canonicalise the
    // unrolled body, but it also re-shapes IR in non-loop functions
    // (gauntlet kernel hit "cast src not in reg" after the rerun).
    // The downstream CFGSimplify + the light backend's per-instruction
    // emitter handle the loop-unroll output without a second pass.
  }
  EASYJIT_RT_LOG("Optimize: add CFGSimplification/Internalize/GlobalDCE/StripDeadPrototypes\n");
  // Minimal cleanup.
  MPM.add(llvm::createCFGSimplificationPass());
  MPM.add(llvm::createInternalizePass([Name](const llvm::GlobalValue &GV) {
    return GV.getName() == Name || GV.getName() == "__dso_handle";
  }));
  MPM.add(llvm::createGlobalDCEPass());
  MPM.add(llvm::createStripDeadPrototypesPass());

#ifdef NDEBUG
  MPM.add(llvm::createVerifierPass());
#endif

  EASYJIT_RT_LOG("Optimize: running pass manager for %s\n", Name ? Name : "<null>");
  MPM.run(M);
  EASYJIT_RT_LOG("Optimize: finished for %s\n", Name ? Name : "<null>");

  // Optional IR dump for benchmarking / debugging the pass pipeline.
  if (const char *DumpPath = std::getenv("EASYJIT_DUMP_IR")) {
    std::error_code EC;
    llvm::raw_fd_ostream OS(DumpPath, EC);
    if (!EC) M.print(OS, nullptr);
  }
}

static void DisableRecursiveJit(llvm::Module &M, const char *EntryName) {
  EASYJIT_RT_LOG("DisableRecursiveJit: begin entry=%s module=%p\n",
                 EntryName ? EntryName : "<null>", (void*)&M);
  if (!EntryName)
    return;

  for (const char *CtorDtorName : {"llvm.global_ctors", "llvm.global_dtors"}) {
    if (llvm::GlobalVariable *GV = M.getGlobalVariable(CtorDtorName)) {
      EASYJIT_RT_LOG("DisableRecursiveJit: erase %s\n", CtorDtorName);
      GV->replaceAllUsesWith(llvm::UndefValue::get(GV->getType()));
      GV->eraseFromParent();
    }
  }

  for (llvm::Function &F : M) {
    if (F.isDeclaration() || F.getName() == EntryName)
      continue;
    if (F.getName().startswith("llvm."))
      continue;
    if (F.getName() == "register_layout")
      continue;

    EASYJIT_RT_LOG("DisableRecursiveJit: externalize helper %s\n",
                   F.getName().str().c_str());
    F.deleteBody();
    F.setComdat(nullptr);
    F.setSection("");
    F.setVisibility(llvm::GlobalValue::DefaultVisibility);
    F.setLinkage(llvm::GlobalValue::ExternalLinkage);
  }
}

static std::string TakeError(llvm::Error Err) {
  return llvm::toString(std::move(Err));
}

#if !EASYJIT_LIGHT_BACKEND_ONLY
static llvm::orc::JITTargetMachineBuilder
GetJITTargetMachineBuilderForModule(llvm::Module const& M) {
  std::string TripleStr = M.getTargetTriple();
  if (TripleStr.empty()) {
    TripleStr = llvm::sys::getProcessTriple();
  }

  llvm::orc::JITTargetMachineBuilder JTMB{llvm::Triple(TripleStr)};
  JTMB.setCPU(llvm::sys::getHostCPUName().str());
  JTMB.setRelocationModel(llvm::Reloc::PIC_);
  JTMB.setCodeModel(llvm::CodeModel::Small);
  JTMB.setCodeGenOptLevel(llvm::CodeGenOpt::Aggressive);

  EASYJIT_RT_LOG("GetJITTargetMachineBuilderForModule: module_triple=%s effective_triple=%s cpu=%s\n",
                 M.getTargetTriple().c_str(),
                 TripleStr.c_str(),
                 llvm::sys::getHostCPUName().str().c_str());
  return JTMB;
}

static std::unique_ptr<easy::detail::MinimalOrcJIT>
CreateJIT(llvm::Module const& M, const char *Name) {
  EASYJIT_RT_LOG("CreateJIT: begin name=%s triple=%s datalayout=%s\n",
                 Name ? Name : "<null>",
                 M.getTargetTriple().c_str(),
                 M.getDataLayoutStr().c_str());

  auto JITOrErr = easy::detail::MinimalOrcJIT::Create(
      GetJITTargetMachineBuilderForModule(M), M.getDataLayout());
  if (!JITOrErr) {
    auto Err = TakeError(JITOrErr.takeError());
    EASYJIT_RT_LOG("CreateJIT: failed name=%s error=%s\n",
                   Name ? Name : "<null>", Err.c_str());
    throw easy::JITCreateError(Name);
  }

  EASYJIT_RT_LOG("CreateJIT: success name=%s jit=%p\n",
                 Name ? Name : "<null>", (void*)JITOrErr->get());
  return std::move(*JITOrErr);
}

static void MapGlobals(easy::detail::MinimalOrcJIT& JIT, GlobalMapping* Globals) {
  EASYJIT_RT_LOG("MapGlobals: begin jit=%p globals=%p\n", (void*)&JIT, (void*)Globals);

  llvm::orc::MangleAndInterner Mangle(JIT.getExecutionSession(), JIT.getDataLayout());
  llvm::orc::SymbolMap Symbols;

  for(GlobalMapping *GM = Globals; GM && GM->Name; ++GM) {
    EASYJIT_RT_LOG("MapGlobals: map %s -> %p\n", GM->Name, GM->Address);
    Symbols[Mangle(GM->Name)] =
      llvm::JITEvaluatedSymbol(llvm::pointerToJITTargetAddress(GM->Address),
                               llvm::JITSymbolFlags::Exported);
  }

  EASYJIT_RT_LOG("MapGlobals: map __dso_handle -> %p\n", &__dso_handle);
  Symbols[Mangle("__dso_handle")] =
    llvm::JITEvaluatedSymbol(llvm::pointerToJITTargetAddress(&__dso_handle),
                             llvm::JITSymbolFlags::Exported);

  if (auto Err = JIT.getMainJITDylib().define(absoluteSymbols(std::move(Symbols)))) {
    auto ErrStr = TakeError(std::move(Err));
    EASYJIT_RT_LOG("MapGlobals: define failed error=%s\n", ErrStr.c_str());
    throw easy::JITCreateError("global mapping");
  }

  auto GeneratorOrErr =
      llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
          JIT.getDataLayout().getGlobalPrefix());
  if (!GeneratorOrErr) {
    auto ErrStr = TakeError(GeneratorOrErr.takeError());
    EASYJIT_RT_LOG("MapGlobals: current-process generator failed error=%s\n", ErrStr.c_str());
    throw easy::JITCreateError("current process symbols");
  }

  JIT.getMainJITDylib().addGenerator(std::move(*GeneratorOrErr));
  EASYJIT_RT_LOG("MapGlobals: end\n");
}
#endif // !EASYJIT_LIGHT_BACKEND_ONLY

static void WriteOptimizedToFile(llvm::Module const &M, std::string const& File) {
  EASYJIT_RT_LOG("WriteOptimizedToFile: file=%s module=%p\n",
                 File.empty() ? "<empty>" : File.c_str(), (const void*)&M);
  if(File.empty())
    return;
  std::error_code Error;
  llvm::raw_fd_ostream Out(File, Error, llvm::sys::fs::OF_None);

  if(Error) {
    EASYJIT_RT_LOG("WriteOptimizedToFile: open failed file=%s error=%s\n",
                   File.c_str(), Error.message().c_str());
    throw CouldNotOpenFile(Error.message());
  }

  Out << M;
  EASYJIT_RT_LOG("WriteOptimizedToFile: done file=%s\n", File.c_str());
}

static std::string GetDumpFileWithSuffix(std::string File, llvm::StringRef Suffix) {
  if(File.empty())
    return File;
  llvm::SmallString<256> Path(File);
  llvm::StringRef Extension = llvm::sys::path::extension(Path);
  if(Extension.empty()) {
    Path += Suffix;
  } else {
    llvm::SmallString<256> Stem(Path);
    Stem.resize(Stem.size() - Extension.size());
    Stem += Suffix;
    Stem += Extension;
    Path = Stem;
  }
  return std::string(Path.str());
}

#if !EASYJIT_LIGHT_BACKEND_ONLY
std::unique_ptr<Function>
CompileAndWrap(const char*Name, GlobalMapping* Globals,
               std::unique_ptr<llvm::LLVMContext> Ctx,
               std::unique_ptr<llvm::Module> M) {

  EASYJIT_RT_LOG("CompileAndWrap: begin name=%s globals=%p module=%p ctx=%p\n",
                 Name ? Name : "<null>",
                 (void*)Globals,
                 (void*)M.get(),
                 (void*)Ctx.get());
  auto StoredCtx = std::make_unique<llvm::LLVMContext>();
  auto StoredModule = easy::CloneModuleWithContext(*M, *StoredCtx);
  if (!StoredModule) {
    EASYJIT_RT_LOG("CompileAndWrap: failed to clone optimized module name=%s\n",
                   Name ? Name : "<null>");
    throw easy::JITCreateError(Name);
  }

  auto JIT = CreateJIT(*M, Name);

  if(Globals) {
    MapGlobals(*JIT, Globals);
  }

  EASYJIT_RT_LOG("CompileAndWrap: addIRModule begin name=%s\n", Name ? Name : "<null>");
  auto TSCtx = std::make_unique<llvm::LLVMContext>();
  auto JITModule = easy::CloneModuleWithContext(*M, *TSCtx);
  if (!JITModule) {
    EASYJIT_RT_LOG("CompileAndWrap: failed to clone jit module name=%s\n",
                   Name ? Name : "<null>");
    throw easy::JITCreateError(Name);
  }

  if (auto Err = JIT->addIRModule(std::move(JITModule), std::move(TSCtx))) {
    auto ErrStr = TakeError(std::move(Err));
    EASYJIT_RT_LOG("CompileAndWrap: addIRModule failed name=%s error=%s\n",
                   Name ? Name : "<null>", ErrStr.c_str());
    throw easy::JITCreateError(Name);
  }
  EASYJIT_RT_LOG("CompileAndWrap: addIRModule end name=%s\n", Name ? Name : "<null>");

  EASYJIT_RT_LOG("CompileAndWrap: lookup begin name=%s\n", Name ? Name : "<null>");
  auto AddressOrErr = JIT->lookup(Name);
  if (!AddressOrErr) {
    auto ErrStr = TakeError(AddressOrErr.takeError());
    EASYJIT_RT_LOG("CompileAndWrap: lookup failed name=%s error=%s\n",
                   Name ? Name : "<null>", ErrStr.c_str());
    throw easy::SymbolLookupError(Name);
  }

  void *Address = llvm::jitTargetAddressToPointer<void *>(AddressOrErr->getAddress());
  EASYJIT_RT_LOG("CompileAndWrap: lookup end name=%s address=%p\n",
                 Name ? Name : "<null>", Address);

  std::unique_ptr<LLVMHolder> Holder(
      new easy::LLVMHolderImpl{std::move(JIT), std::move(StoredCtx), std::move(StoredModule)});
  EASYJIT_RT_LOG("CompileAndWrap: success name=%s holder=%p\n",
                 Name ? Name : "<null>", (void*)Holder.get());
  return std::unique_ptr<Function>(new Function(Address, std::move(Holder)));
}
#endif // !EASYJIT_LIGHT_BACKEND_ONLY

llvm::Module const& Function::getLLVMModule() const {
  llvm::Module *M = this->Holder->getModule();
  if (!M)
    throw std::runtime_error(
        "easy::Function::getLLVMModule: function has no LLVM module "
        "(compiled function fell back to the original function pointer)");
  return *M;
}

std::unique_ptr<Function> Function::Compile(void *Addr, easy::Context const& C) {
  // llvm::DebugFlag = true;
  // llvm::setCurrentDebugType("jit");

  auto &BT = BitcodeTracker::GetTracker();
  EASYJIT_RT_LOG("Function::Compile: begin addr=%p ctx_size=%zu\n", Addr, C.size());

  const char* Name;
  GlobalMapping* Globals;
  std::tie(Name, Globals) = BT.getNameAndGlobalMapping(Addr);
  EASYJIT_RT_LOG("Function::Compile: tracker name=%s globals=%p\n",
                 Name ? Name : "<null>", (void*)Globals);

  std::unique_ptr<llvm::Module> M;
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::tie(M, Ctx) = BT.getModule(Addr);
  if (!M || !Ctx) {
    EASYJIT_RT_LOG("Function::Compile: null module/context after tracker lookup addr=%p module=%p ctx=%p\n",
                   Addr, (void*)M.get(), (void*)Ctx.get());
  }
  EASYJIT_RT_LOG("Function::Compile: module loaded module=%p ctx=%p module_triple=%s datalayout=%s\n",
                 (void*)M.get(),
                 (void*)Ctx.get(),
                 M ? M->getTargetTriple().c_str() : "<null>",
                 M ? M->getDataLayoutStr().c_str() : "<null>");

  unsigned OptLevel;
  unsigned OptSize;
  std::tie(OptLevel, OptSize) = C.getOptLevel();
  EASYJIT_RT_LOG("Function::Compile: optlevel=%u optsz=%u debugfile=%s\n",
                 OptLevel, OptSize, C.getDebugFile().c_str());

  EASYJIT_RT_LOG("Function::Compile: write before-ir begin\n");
  WriteOptimizedToFile(*M, GetDumpFileWithSuffix(C.getDebugFile(), ".before"));
  EASYJIT_RT_LOG("Function::Compile: write before-ir end\n");

  if (!C.getRecursiveJit()) {
    EASYJIT_RT_LOG("Function::Compile: disabling recursive JIT for %s\n",
                   Name ? Name : "<null>");
    DisableRecursiveJit(*M, Name);
  }

  Optimize(*M, Name, C, OptLevel, OptSize);
  EASYJIT_RT_LOG("Function::Compile: Optimize complete module=%p ctx=%p\n",
                 (void*)M.get(), (void*)Ctx.get());

  EASYJIT_RT_LOG("Function::Compile: write final-ir begin\n");
  WriteOptimizedToFile(*M, C.getDebugFile());
  EASYJIT_RT_LOG("Function::Compile: write final-ir end\n");
  EASYJIT_RT_LOG("Function::Compile: write after-ir begin\n");
  WriteOptimizedToFile(*M, GetDumpFileWithSuffix(C.getDebugFile(), ".after"));
  EASYJIT_RT_LOG("Function::Compile: write after-ir end\n");

  // ORC emits object code through an object streamer, which cannot handle
  // module-level raw inline asm. C++ standard-library headers may inject
  // harmless directives such as ".globl _ZSt21ios_base_library_initv"; strip
  // them before handing the module to the JIT compiler.
  if (!M->getModuleInlineAsm().empty()) {
    EASYJIT_RT_LOG("Function::Compile: stripping module inline asm before JIT\n");
    M->setModuleInlineAsm("");
  }

#if EASYJIT_LIGHT_BACKEND_ENABLED
  {
    auto policy = easy::light_backend::GetPolicyFromEnv();
#if EASYJIT_LIGHT_BACKEND_ONLY
    // Light-only build: no ORC fallback exists. Always go through the
    // light backend; treat any non-success as a hard JIT failure.
    if (policy == easy::light_backend::Policy::Off)
      policy = easy::light_backend::Policy::Try;
#endif
    if (policy != easy::light_backend::Policy::Off) {
      EASYJIT_RT_LOG("Function::Compile: TryLightCompile begin policy=%s\n",
                     easy::light_backend::PolicyName(policy));
      std::unique_ptr<Function> lightFn;
      auto rep = easy::light_backend::TryLightCompile(
          Name, Globals, Ctx, M, lightFn, policy);

      // Helper: when we are about to throw out of Function::Compile, the
      // module must be torn down BEFORE the LLVMContext (M's destructor
      // calls LLVMContext::removeModule). Reset M here so the implicit
      // reverse-declaration order of locals (Ctx destroyed before M) does
      // not crash on the unwind path.
      auto failCleanup = [&]() {
        M.reset();
      };

      switch (rep.outcome) {
        case easy::light_backend::Outcome::Succeeded:
          EASYJIT_RT_LOG("Function::Compile: light path used name=%s\n",
                         Name ? Name : "<null>");
          return lightFn;
        case easy::light_backend::Outcome::FailedForce:
          EASYJIT_RT_LOG("Function::Compile: EASYJIT_LIGHT=force rejected: %s\n",
                         rep.reason.c_str());
          failCleanup();
          throw easy::LightBackendCompileError(
              std::string(Name ? Name : "<null>") +
              " (EASYJIT_LIGHT=force, reason: " + rep.reason + ")");
        case easy::light_backend::Outcome::Unsupported:
#if EASYJIT_LIGHT_BACKEND_ONLY
          EASYJIT_RT_LOG("Function::Compile: light unsupported in light-only build: %s\n",
                         rep.reason.c_str());
          if (CanFallbackToOriginalFunction(C, *M, Name)) {
            std::string reason =
                std::string(Name ? Name : "<null>") +
                " (unsupported IR, reason: " + rep.reason +
                "; light-only runtime fell back to original function pointer)";
            failCleanup();
            return MakeOriginalFunctionFallback(Addr, std::move(reason));
          }
          failCleanup();
          throw easy::LightBackendCompileError(
              std::string(Name ? Name : "<null>") +
              " (unsupported IR, reason: " + rep.reason +
              "; light-only runtime has no ORC fallback and original function "
              "fallback is unsafe because the context changes the call signature)");
#else
          EASYJIT_RT_LOG("Function::Compile: light unsupported (%s), ORC fallback\n",
                         rep.reason.c_str());
          break;
#endif
        case easy::light_backend::Outcome::SkippedByPolicy:
#if EASYJIT_LIGHT_BACKEND_ONLY
          EASYJIT_RT_LOG("Function::Compile: light skipped in light-only build: %s\n",
                         rep.reason.c_str());
          failCleanup();
          throw easy::LightBackendCompileError(
              std::string(Name ? Name : "<null>") +
              " (skipped by policy: " + rep.reason +
              "; light-only runtime has no ORC fallback)");
#else
          EASYJIT_RT_LOG("Function::Compile: light skipped (%s)\n",
                         rep.reason.c_str());
          break;
#endif
      }
    }
  }
#endif

#if EASYJIT_LIGHT_BACKEND_ONLY
  // Light-only build: no ORC fallback compiled in. Tear down M before
  // Ctx (locals destroy in reverse declaration order, but ~Module needs
  // a live LLVMContext).
  M.reset();
  EASYJIT_RT_LOG("Function::Compile: light-only build cannot fall back\n");
  throw easy::LightBackendCompileError(
      std::string(Name ? Name : "<null>") +
      " (light-only runtime, LightBackend not engaged)");
#else
  EASYJIT_RT_LOG("Function::Compile: CompileAndWrap begin\n");
  auto Result = CompileAndWrap(Name, Globals, std::move(Ctx), std::move(M));
  EASYJIT_RT_LOG("Function::Compile: CompileAndWrap returned function=%p raw=%p\n",
                 (void*)Result.get(), Result ? Result->getRawPointer() : nullptr);
  return Result;
#endif
}

void easy::Function::serialize(std::ostream& os) const {
  std::string buf;
  llvm::raw_string_ostream stream(buf);

  llvm::Module *M = Holder->getModule();
  if (M) {
    llvm::WriteBitcodeToFile(*M, stream);
    stream.flush();
  }

  os << buf;
}

std::unique_ptr<easy::Function> easy::Function::deserialize(std::istream& is) {

  auto &BT = BitcodeTracker::GetTracker();

  std::string buf(std::istreambuf_iterator<char>(is), {}); // read the entire istream
  auto MemBuf = llvm::MemoryBuffer::getMemBuffer(llvm::StringRef(buf));

  std::unique_ptr<llvm::LLVMContext> Ctx(new llvm::LLVMContext());
  auto ModuleOrError = llvm::parseBitcodeFile(*MemBuf, *Ctx);
  if(ModuleOrError.takeError()) {
    return nullptr;
  }

  auto M = std::move(ModuleOrError.get());

  std::string FunName = easy::GetEntryFunctionName(*M).str();

  GlobalMapping* Globals = nullptr;
  if(void* OrigFunPtr = BT.getAddress(FunName)) {
    std::tie(std::ignore, Globals) = BT.getNameAndGlobalMapping(OrigFunPtr);
  }

#if EASYJIT_LIGHT_BACKEND_ONLY
  // Light-only build: no ORC fallback. Drive deserialization through
  // the lightweight backend. Force-mode so that any non-success is a
  // hard, observable error rather than a silent nullptr.
#if !EASYJIT_LIGHT_BACKEND_ENABLED
#error "EASYJIT_LIGHT_BACKEND_ONLY=1 requires EASYJIT_LIGHT_BACKEND_ENABLED=1"
#endif
  std::unique_ptr<Function> lightFn;
  auto rep = easy::light_backend::TryLightCompile(
      FunName.c_str(), Globals, Ctx, M, lightFn,
      easy::light_backend::Policy::Force);
  if (rep.outcome == easy::light_backend::Outcome::Succeeded) {
    EASYJIT_RT_LOG("Function::deserialize: light path used name=%s\n",
                   FunName.c_str());
    return lightFn;
  }
  EASYJIT_RT_LOG("Function::deserialize: light path rejected name=%s reason=%s\n",
                 FunName.c_str(), rep.reason.c_str());
  // Tear down M before its parent LLVMContext goes out of scope.
  M.reset();
  throw easy::LightBackendCompileError(
      FunName + " (deserialize, light-only runtime, reason: " + rep.reason + ")");
#else
  return CompileAndWrap(FunName.c_str(), Globals, std::move(Ctx), std::move(M));
#endif // EASYJIT_LIGHT_BACKEND_ONLY
}

bool Function::operator==(easy::Function const& other) const {
  return this->Holder->getModule() == other.Holder->getModule();
}

std::hash<easy::Function>::result_type
std::hash<easy::Function>::operator()(argument_type const& F) const noexcept {
  return std::hash<llvm::Module*>{}(F.Holder->getModule());
}
