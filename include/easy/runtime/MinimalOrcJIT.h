#ifndef EASY_RUNTIME_MINIMAL_ORC_JIT_H
#define EASY_RUNTIME_MINIMAL_ORC_JIT_H

#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ExecutorProcessControl.h>
#include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Target/TargetMachine.h>

#include <memory>

namespace easy {
namespace detail {

class MinimalOrcJIT {
public:
  static llvm::Expected<std::unique_ptr<MinimalOrcJIT>>
  Create(llvm::orc::JITTargetMachineBuilder JTMB, llvm::DataLayout DL);

  ~MinimalOrcJIT();

  const llvm::DataLayout &getDataLayout() const { return DL_; }
  llvm::orc::ExecutionSession &getExecutionSession() { return *ES_; }
  llvm::orc::JITDylib &getMainJITDylib() { return MainJD_; }

  llvm::Error addIRModule(std::unique_ptr<llvm::Module> M,
                          std::unique_ptr<llvm::LLVMContext> Ctx);

  llvm::Expected<llvm::JITEvaluatedSymbol> lookup(llvm::StringRef Name);

private:
  MinimalOrcJIT(std::unique_ptr<llvm::orc::ExecutionSession> ES,
                std::unique_ptr<llvm::TargetMachine> TM,
                llvm::orc::JITTargetMachineBuilder JTMB,
                llvm::DataLayout DL);

  std::unique_ptr<llvm::orc::ExecutionSession> ES_;
  llvm::DataLayout DL_;
  llvm::orc::MangleAndInterner Mangle_;
  llvm::orc::RTDyldObjectLinkingLayer ObjectLayer_;
  std::unique_ptr<llvm::TargetMachine> TM_;
  llvm::orc::IRCompileLayer CompileLayer_;
  llvm::orc::JITDylib &MainJD_;
};

} // namespace detail
} // namespace easy

#endif
