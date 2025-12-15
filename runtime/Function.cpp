#include <easy/runtime/BitcodeTracker.h>
#include <easy/runtime/Function.h>
#include <easy/runtime/RuntimePasses.h>
#include <easy/runtime/LLVMHolderImpl.h>
#include <easy/runtime/Utils.h>
#include <easy/exceptions.h>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>

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
  std::unique_ptr<llvm::TargetMachine> TM(llvm::EngineBuilder().selectTarget());
  return TM;
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

static std::unique_ptr<llvm::ExecutionEngine> GetEngine(std::unique_ptr<llvm::Module> M, const char *Name) {
  llvm::EngineBuilder ebuilder(std::move(M));
  std::string eeError;

  std::unique_ptr<llvm::ExecutionEngine> EE(ebuilder.setErrorStr(&eeError)
          .setMCPU(llvm::sys::getHostCPUName())
          .setEngineKind(llvm::EngineKind::JIT)
          .setOptLevel(llvm::CodeGenOptLevel::Aggressive)
          .create());

  if(!EE) {
    throw easy::ExecutionEngineCreateError(Name);
  }

  return EE;
}

static void MapGlobals(llvm::ExecutionEngine& EE, GlobalMapping* Globals) {
  for(GlobalMapping *GM = Globals; GM->Name; ++GM) {
    EE.addGlobalMapping(GM->Name, (uint64_t)GM->Address);
  }
  EE.addGlobalMapping("__dso_handle", (uint64_t)&EE);
  EE.finalizeObject();
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

std::unique_ptr<Function>
CompileAndWrap(const char*Name, GlobalMapping* Globals,
               std::unique_ptr<llvm::LLVMContext> Ctx,
               std::unique_ptr<llvm::Module> M) {

  llvm::Module* MPtr = M.get();
  std::unique_ptr<llvm::ExecutionEngine> EE = GetEngine(std::move(M), Name);

  if(Globals) {
    MapGlobals(*EE, Globals);
  }

  void *Address = (void*)EE->getFunctionAddress(Name);

  std::unique_ptr<LLVMHolder> Holder(new easy::LLVMHolderImpl{std::move(EE), std::move(Ctx), MPtr});
  return std::unique_ptr<Function>(new Function(Address, std::move(Holder)));
}

llvm::Module const& Function::getLLVMModule() const {
  return *static_cast<LLVMHolderImpl const&>(*this->Holder).M_;
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
  std::tie(M, Ctx) = BT.getModule(Addr);

  llvm::OptimizationLevel OptimizationLevel = getOptimizationLevel(C.getOptLevel());

  Optimize(*M, Name, C, OptimizationLevel);

  WriteOptimizedToFile(*M, C.getDebugFile());

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
  return This.M_ == Other.M_;
}

std::hash<easy::Function>::result_type
std::hash<easy::Function>::operator()(argument_type const& F) const noexcept {
  LLVMHolderImpl& This = static_cast<LLVMHolderImpl&>(*F.Holder);
  return std::hash<llvm::Module*>{}(This.M_);
}
