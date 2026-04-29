#ifndef LLVMHOLDER_IMPL
#define LLVMHOLDER_IMPL

#include <easy/runtime/LLVMHolder.h>
#include <easy/runtime/MinimalOrcJIT.h>

#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>

namespace easy {
class LLVMHolderImpl : public easy::LLVMHolder {
  public:

  std::unique_ptr<llvm::LLVMContext> Context_;
  std::unique_ptr<easy::detail::MinimalOrcJIT> JIT_;
  std::unique_ptr<llvm::Module> M_;

  LLVMHolderImpl(std::unique_ptr<easy::detail::MinimalOrcJIT> JIT,
                 std::unique_ptr<llvm::LLVMContext> C,
                 std::unique_ptr<llvm::Module> M)
    : Context_(std::move(C)), JIT_(std::move(JIT)), M_(std::move(M)) {
  }

  virtual ~LLVMHolderImpl() = default;
};
}

#endif
