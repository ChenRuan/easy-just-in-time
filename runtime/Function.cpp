#include <easy/runtime/BitcodeTracker.h>
#include <easy/runtime/Function.h>
#include <easy/runtime/RuntimePasses.h>
#include <easy/runtime/LLVMHolderImpl.h>
#include <easy/runtime/Utils.h>
#include <easy/exceptions.h>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/Host.h> 
#include <llvm/Support/Error.h>
#include <llvm/Target/TargetMachine.h> 
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Analysis/TargetTransformInfo.h> 
#include <llvm/Analysis/TargetLibraryInfo.h> 
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <cstdio>

#ifdef NDEBUG
#include <llvm/IR/Verifier.h>
#endif

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


using namespace easy;

extern void* __dso_handle;

namespace easy {
  DefineEasyException(JITCreateError, "Failed to create ORC JIT for:");
  DefineEasyException(CouldNotOpenFile, "Failed to file to dump intermediate representation.");
  DefineEasyException(TargetMachineCreateError, "Failed to create target machine for:");
  DefineEasyException(TargetLookupError, "Failed to lookup target for:");
  DefineEasyException(SymbolLookupError, "Failed to lookup JIT symbol for:");
}

Function::Function(void* Addr, std::unique_ptr<LLVMHolder> H)
  : Address(Addr), Holder(std::move(H)) {
}

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

  llvm::PassManagerBuilder Builder;
  Builder.OptLevel = OptLevel;
  Builder.SizeLevel = OptSize;
  Builder.LibraryInfo = new llvm::TargetLibraryInfoImpl(Triple);
  Builder.Inliner = llvm::createFunctionInliningPass(OptLevel, OptSize, false);

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
  TM->adjustPassManager(Builder);

  llvm::legacy::PassManager MPM;
  MPM.add(llvm::createTargetTransformInfoWrapperPass(TM->getTargetIRAnalysis()));
  MPM.add(easy::createContextAnalysisPass(C));
  MPM.add(easy::createInlineParametersPass(Name));
  Builder.populateModulePassManager(MPM);
  MPM.add(easy::createDevirtualizeConstantPass(Name));

#ifdef NDEBUG
  MPM.add(llvm::createVerifierPass());
#endif

  Builder.populateModulePassManager(MPM);

  EASYJIT_RT_LOG("Optimize: running pass manager for %s\n", Name ? Name : "<null>");
  MPM.run(M);
  EASYJIT_RT_LOG("Optimize: finished for %s\n", Name ? Name : "<null>");
}

static std::string TakeError(llvm::Error Err) {
  return llvm::toString(std::move(Err));
}

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

static std::unique_ptr<llvm::orc::LLJIT>
CreateJIT(llvm::Module const& M, const char *Name) {
  EASYJIT_RT_LOG("CreateJIT: begin name=%s triple=%s datalayout=%s\n",
                 Name ? Name : "<null>",
                 M.getTargetTriple().c_str(),
                 M.getDataLayoutStr().c_str());
  llvm::orc::LLJITBuilder Builder;
  Builder.setPlatformSetUp(llvm::orc::setUpInactivePlatform);
  Builder.setNumCompileThreads(0);
  Builder.setJITTargetMachineBuilder(GetJITTargetMachineBuilderForModule(M));
  Builder.setDataLayout(M.getDataLayout());

  auto JITOrErr = Builder.create();
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

static void MapGlobals(llvm::orc::LLJIT& JIT, GlobalMapping* Globals) {
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

static void WriteOptimizedToFile(llvm::Module const &M, std::string const& File) {
  if(File.empty())
    return;
  std::error_code Error;
  llvm::raw_fd_ostream Out(File, Error, llvm::sys::fs::OF_None);

  if(Error)
    throw CouldNotOpenFile(Error.message());

  Out << M;
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

  if (auto Err = JIT->addIRModule(
          llvm::orc::ThreadSafeModule(std::move(JITModule), std::move(TSCtx)))) {
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

  void *Address = AddressOrErr->toPtr<void*>();
  EASYJIT_RT_LOG("CompileAndWrap: lookup end name=%s address=%p\n",
                 Name ? Name : "<null>", Address);

  std::unique_ptr<LLVMHolder> Holder(
      new easy::LLVMHolderImpl{std::move(JIT), std::move(StoredCtx), std::move(StoredModule)});
  EASYJIT_RT_LOG("CompileAndWrap: success name=%s holder=%p\n",
                 Name ? Name : "<null>", (void*)Holder.get());
  return std::unique_ptr<Function>(new Function(Address, std::move(Holder)));
}

llvm::Module const& Function::getLLVMModule() const {
  return *static_cast<LLVMHolderImpl const&>(*this->Holder).M_;
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

  Optimize(*M, Name, C, OptLevel, OptSize);

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

  EASYJIT_RT_LOG("Function::Compile: CompileAndWrap begin\n");
  return CompileAndWrap(Name, Globals, std::move(Ctx), std::move(M));
}

void easy::Function::serialize(std::ostream& os) const {
  std::string buf;
  llvm::raw_string_ostream stream(buf);

  LLVMHolderImpl const *H = reinterpret_cast<LLVMHolderImpl const*>(Holder.get());
  llvm::WriteBitcodeToFile(*H->M_, stream);
  stream.flush();

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

  return CompileAndWrap(FunName.c_str(), Globals, std::move(Ctx), std::move(M));
}

bool Function::operator==(easy::Function const& other) const {
  LLVMHolderImpl& This = static_cast<LLVMHolderImpl&>(*this->Holder);
  LLVMHolderImpl& Other = static_cast<LLVMHolderImpl&>(*other.Holder);
  return This.M_.get() == Other.M_.get();
}

std::hash<easy::Function>::result_type
std::hash<easy::Function>::operator()(argument_type const& F) const noexcept {
  LLVMHolderImpl& This = static_cast<LLVMHolderImpl&>(*F.Holder);
  return std::hash<llvm::Module*>{}(This.M_.get());
}
