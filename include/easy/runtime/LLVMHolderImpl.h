#ifndef LLVMHOLDER_IMPL
#define LLVMHOLDER_IMPL

#include <easy/runtime/LLVMHolder.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <string>
#include <memory>

namespace easy {
class LLVMHolderImpl : public easy::LLVMHolder {
  public:

  std::unique_ptr<llvm::orc::LLJIT> JIT_;
  std::unique_ptr<llvm::LLVMContext> Context_;
  std::string SerializedBitcode_; // cached bitcode for serialization (Module is consumed by ORC)
  uintptr_t ModuleId_;            // unique id for equality / hashing

  LLVMHolderImpl(std::unique_ptr<llvm::orc::LLJIT> JIT,
                 std::unique_ptr<llvm::LLVMContext> C,
                 std::string Bitcode,
                 uintptr_t ModuleId)
    : JIT_(std::move(JIT)), Context_(std::move(C)),
      SerializedBitcode_(std::move(Bitcode)), ModuleId_(ModuleId) {
  }

  /// Reconstruct a Module from the cached bitcode.
  /// The caller supplies the LLVMContext; ownership of the Module is returned.
  std::unique_ptr<llvm::Module> getModuleFromBitcode(llvm::LLVMContext &Ctx) const {
    auto MemBuf = llvm::MemoryBuffer::getMemBuffer(
        llvm::StringRef(SerializedBitcode_), "", /*RequiresNullTerminator=*/false);
    auto MOrErr = llvm::parseBitcodeFile(*MemBuf, Ctx);
    if (!MOrErr) {
      llvm::consumeError(MOrErr.takeError());
      return nullptr;
    }
    return std::move(*MOrErr);
  }

  virtual ~LLVMHolderImpl() = default;
};
}

#endif
