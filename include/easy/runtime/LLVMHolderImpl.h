#ifndef LLVMHOLDER_IMPL
#define LLVMHOLDER_IMPL

#include <easy/runtime/LLVMHolder.h>

#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>

#ifndef EASYJIT_LIGHT_BACKEND_ONLY
#define EASYJIT_LIGHT_BACKEND_ONLY 0
#endif

#if !EASYJIT_LIGHT_BACKEND_ONLY
#include <easy/runtime/MinimalOrcJIT.h>
#endif

namespace easy {

#if !EASYJIT_LIGHT_BACKEND_ONLY
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

  llvm::Module* getModule() const override { return M_.get(); }

  virtual ~LLVMHolderImpl() = default;
};
#endif // !EASYJIT_LIGHT_BACKEND_ONLY

}

#endif
