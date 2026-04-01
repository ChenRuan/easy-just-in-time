#include <easy/runtime/BitcodeTracker.h>
#include <easy/runtime/Function.h>
#include "internal/RuntimePassesInternal.h"
#include <easy/runtime/LLVMHolderImpl.h>
#include "internal/UtilsInternal.h"
#include "internal/BitcodeTrackerInternal.h"
#include <easy/exceptions.h>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>

#ifdef NDEBUG
#include <llvm/IR/Verifier.h>
#endif


using namespace easy;

namespace easy {
  DefineEasyException(ExecutionEngineCreateError, "Failed to create execution engine for:");
  DefineEasyException(CouldNotOpenFile, "Failed to file to dump intermediate representation.");
}

Function::Function(void* Addr, std::unique_ptr<LLVMHolder> H)
  : Address(Addr), Holder(std::move(H)) {
}

static std::unique_ptr<llvm::TargetMachine> GetHostTargetMachine() {
  auto JTMB = llvm::orc::JITTargetMachineBuilder::detectHost();
  if (!JTMB) {
    llvm::consumeError(JTMB.takeError());
    return nullptr;
  }
  JTMB->setCodeGenOptLevel(llvm::CodeGenOptLevel::Aggressive);
  auto TM = JTMB->createTargetMachine();
  if (!TM) {
    llvm::consumeError(TM.takeError());
    return nullptr;
  }
  return std::move(*TM);
}

static void Optimize(llvm::Module& M, const char* Name, const easy::Context& C, llvm::OptimizationLevel OptLevel) {

  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;

  MAM.registerPass([&C]{return ContextAnalysisPass(C);});

  std::unique_ptr<llvm::TargetMachine> TM = GetHostTargetMachine();
  assert(TM);

  llvm::PassBuilder PB(TM.get());

  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  llvm::ModulePassManager MPM;

  MPM.addPass(easy::InlineParametersPass(Name));
  MPM.addPass(PB.buildPerModuleDefaultPipeline(OptLevel));
  MPM.addPass(llvm::createModuleToFunctionPassAdaptor(easy::DevirtualizeConstantPass(Name)));

#ifdef NDEBUG
  MPM.addPass(llvm::VerifierPass());
#endif

  MPM.addPass(PB.buildPerModuleDefaultPipeline(OptLevel));

  MPM.run(M, MAM);
}

static std::unique_ptr<llvm::orc::LLJIT> CreateLLJIT() {
  auto Builder = llvm::orc::LLJITBuilder();
  auto JIT = Builder.create();
  if (!JIT) {
    llvm::consumeError(JIT.takeError());
    return nullptr;
  }
  return std::move(*JIT);
}

static void MapGlobals(llvm::orc::LLJIT& JIT, GlobalMapping* Globals) {
  llvm::orc::SymbolMap SymMap;
  auto &ES = JIT.getExecutionSession();
  for(GlobalMapping *GM = Globals; GM->Name; ++GM) {
    SymMap[ES.intern(GM->Name)] = {
      llvm::orc::ExecutorAddr::fromPtr((void*)GM->Address),
      llvm::JITSymbolFlags::Exported
    };
  }
  // Also map __dso_handle
  SymMap[ES.intern("__dso_handle")] = {
    llvm::orc::ExecutorAddr::fromPtr((void*)&JIT),
    llvm::JITSymbolFlags::Exported
  };

  if (!SymMap.empty()) {
    if (auto Err = JIT.getMainJITDylib().define(
            llvm::orc::absoluteSymbols(std::move(SymMap)))) {
      llvm::consumeError(std::move(Err));
    }
  }
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

  // Snapshot the optimized bitcode before ORC takes ownership of the Module
  std::string BitcodeBuf;
  {
    llvm::raw_string_ostream BOS(BitcodeBuf);
    llvm::WriteBitcodeToFile(*M, BOS);
  }
  uintptr_t ModuleId = reinterpret_cast<uintptr_t>(M.get());

  auto JIT = CreateLLJIT();
  if (!JIT) {
    throw easy::ExecutionEngineCreateError(Name);
  }

  // Register process symbols so JIT'd code can call back into the host
  auto &ES = JIT->getExecutionSession();
  auto &DL = JIT->getDataLayout();
  auto ProcessSymbolsGenerator =
      llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
          DL.getGlobalPrefix());
  if (ProcessSymbolsGenerator) {
    JIT->getMainJITDylib().addGenerator(std::move(*ProcessSymbolsGenerator));
  } else {
    llvm::consumeError(ProcessSymbolsGenerator.takeError());
  }

  if(Globals) {
    MapGlobals(*JIT, Globals);
  }

  // Wrap module + context in ThreadSafeModule and add to JIT
  auto TSM = llvm::orc::ThreadSafeModule(std::move(M), std::move(Ctx));
  if (auto Err = JIT->addIRModule(std::move(TSM))) {
    llvm::consumeError(std::move(Err));
    throw easy::ExecutionEngineCreateError(Name);
  }

  // Look up the compiled function
  auto Sym = JIT->lookup(Name);
  if (!Sym) {
    llvm::consumeError(Sym.takeError());
    throw easy::ExecutionEngineCreateError(Name);
  }

  void *Address = Sym->toPtr<void*>();

  // Create a new context for the holder (the original was consumed by ThreadSafeModule)
  std::unique_ptr<llvm::LLVMContext> HolderCtx(new llvm::LLVMContext());

  std::unique_ptr<LLVMHolder> Holder(new easy::LLVMHolderImpl{
      std::move(JIT), std::move(HolderCtx),
      std::move(BitcodeBuf), ModuleId});
  return std::unique_ptr<Function>(new Function(Address, std::move(Holder)));
}

static llvm::OptimizationLevel getOptimizationLevel(const std::pair<unsigned, unsigned> & OptLevelPair) {
  unsigned OptLevel = OptLevelPair.first;
  unsigned OptSize = OptLevelPair.second;
  assert(OptLevel <= 3 && "Optimization level for speed should be 0, 1, 2, or 3");
  assert(OptSize <= 2 && "Optimization level for size should be 0, 1, or 2");
  assert((OptSize == 0 || OptLevel == 2) && "Optimize for size should be encoded with speedup level == 2");
  if(OptLevel == 0)
    return llvm::OptimizationLevel::O0;
  if(OptLevel == 1)
    return llvm::OptimizationLevel::O1;
  if(OptLevel == 2) {
    if(OptSize == 0)
      return llvm::OptimizationLevel::O2;
    else if (OptSize == 1)
      return llvm::OptimizationLevel::Os;
    else // OptSize == 2
      return llvm::OptimizationLevel::Oz;
  }
  if(OptLevel == 3)
    return llvm::OptimizationLevel::O3;
  return llvm::OptimizationLevel::O3;
}

std::unique_ptr<Function> Function::Compile(void *Addr, easy::Context const& C) {
  // llvm::DebugFlag = true;
  // llvm::setCurrentDebugType("jit");

  auto &BT = BitcodeTracker::GetTracker();

  const char* Name;
  GlobalMapping* Globals;
  std::tie(Name, Globals) = BT.getNameAndGlobalMapping(Addr);

  std::unique_ptr<llvm::Module> M;
  std::unique_ptr<llvm::LLVMContext> Ctx;
  auto pair = BT_getModule(BT, Addr);
  M = std::move(pair.first);
  Ctx = std::move(pair.second);

  WriteOptimizedToFile(*M, GetDumpFileWithSuffix(C.getDebugFile(), ".before"));

  llvm::OptimizationLevel OptimizationLevel = getOptimizationLevel(C.getOptLevel());

  Optimize(*M, Name, C, OptimizationLevel);

  WriteOptimizedToFile(*M, C.getDebugFile());
  WriteOptimizedToFile(*M, GetDumpFileWithSuffix(C.getDebugFile(), ".after"));

  return CompileAndWrap(Name, Globals, std::move(Ctx), std::move(M));
}

void easy::Function::serialize(std::ostream& os) const {
  LLVMHolderImpl const *H = reinterpret_cast<LLVMHolderImpl const*>(Holder.get());
  os << H->SerializedBitcode_;
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
  return This.ModuleId_ == Other.ModuleId_;
}

std::hash<easy::Function>::result_type
std::hash<easy::Function>::operator()(argument_type const& F) const noexcept {
  LLVMHolderImpl& This = static_cast<LLVMHolderImpl&>(*F.Holder);
  return std::hash<uintptr_t>{}(This.ModuleId_);
}
