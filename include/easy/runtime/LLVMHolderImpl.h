#ifndef LLVMHOLDER_IMPL
#define LLVMHOLDER_IMPL

#include <easy/runtime/LLVMHolder.h>

#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>

namespace easy {
class LLVMHolderImpl : public easy::LLVMHolder {
  public:

  std::unique_ptr<llvm::LLVMContext> Context_;
  std::unique_ptr<llvm::orc::LLJIT> JIT_;
  std::unique_ptr<llvm::Module> M_;

  LLVMHolderImpl(std::unique_ptr<llvm::orc::LLJIT> JIT,
                 std::unique_ptr<llvm::LLVMContext> C,
                 std::unique_ptr<llvm::Module> M)
    : Context_(std::move(C)), JIT_(std::move(JIT)), M_(std::move(M)) {
  }

  virtual ~LLVMHolderImpl() = default;
};
}

#endif
