#include <easy/runtime/MinimalOrcJIT.h>

namespace easy {
namespace detail {

MinimalOrcJIT::MinimalOrcJIT(
    std::unique_ptr<llvm::orc::ExecutionSession> ES,
    std::unique_ptr<llvm::TargetMachine> TM,
    llvm::orc::JITTargetMachineBuilder JTMB, llvm::DataLayout DL)
    : ES_(std::move(ES)), DL_(std::move(DL)), Mangle_(*ES_, DL_),
      ObjectLayer_(*ES_, []() {
        return std::make_unique<llvm::SectionMemoryManager>();
      }),
      TM_(std::move(TM)),
      CompileLayer_(*ES_, ObjectLayer_,
                    std::make_unique<llvm::orc::SimpleCompiler>(*TM_)),
      MainJD_(ES_->createBareJITDylib("<main>")) {
  if (JTMB.getTargetTriple().isOSBinFormatCOFF()) {
    ObjectLayer_.setOverrideObjectFlagsWithResponsibilityFlags(true);
    ObjectLayer_.setAutoClaimResponsibilityForObjectSymbols(true);
  }
}

MinimalOrcJIT::~MinimalOrcJIT() {
  if (auto Err = ES_->endSession())
    ES_->reportError(std::move(Err));
}

llvm::Expected<std::unique_ptr<MinimalOrcJIT>>
MinimalOrcJIT::Create(llvm::orc::JITTargetMachineBuilder JTMB,
                      llvm::DataLayout DL) {
  auto EPC = llvm::orc::SelfExecutorProcessControl::Create();
  if (!EPC)
    return EPC.takeError();

  auto TM = JTMB.createTargetMachine();
  if (!TM)
    return TM.takeError();

  auto ES = std::make_unique<llvm::orc::ExecutionSession>(std::move(*EPC));
  return std::unique_ptr<MinimalOrcJIT>(
      new MinimalOrcJIT(std::move(ES), std::move(*TM), std::move(JTMB),
                        std::move(DL)));
}

llvm::Error MinimalOrcJIT::addIRModule(std::unique_ptr<llvm::Module> M,
                                       std::unique_ptr<llvm::LLVMContext> Ctx) {
  return CompileLayer_.add(MainJD_.getDefaultResourceTracker(),
                           llvm::orc::ThreadSafeModule(std::move(M),
                                                       std::move(Ctx)));
}

llvm::Expected<llvm::JITEvaluatedSymbol>
MinimalOrcJIT::lookup(llvm::StringRef Name) {
  return ES_->lookup({&MainJD_}, Mangle_(Name.str()));
}

} // namespace detail
} // namespace easy
