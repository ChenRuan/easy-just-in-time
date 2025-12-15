#ifndef INLINEPARAMETERSHELPER_H
#define INLINEPARAMETERSHELPER_H

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Constants.h>
#include <llvm/ADT/SmallVector.h>

#include <easy/runtime/Context.h>

namespace easy {

struct HighLevelLayout {
  struct HighLevelArg {
    size_t Position_;
    size_t FirstParamIdx_;
    llvm::SmallVector<llvm::Type*, 1> Types_;
    bool StructByPointer_ = false;
    bool StructByArray_ = false;

    HighLevelArg(size_t Pos, size_t FirstParamIdx) :
      Position_(Pos), FirstParamIdx_(FirstParamIdx) { }
    explicit HighLevelArg() = default;
  };

  llvm::Type* StructReturn_;
  llvm::SmallVector<HighLevelArg, 4> Args_;
  llvm::Type* Return_;

  HighLevelLayout(easy::Context const& C, llvm::Function &F);
};

llvm::SmallVector<llvm::Value*, 4> GetForwardArgs(easy::HighLevelLayout::HighLevelArg &ArgInF, easy::HighLevelLayout &FHLL,
                                                  llvm::Function &Wrapper, easy::HighLevelLayout &WrapperHLL);
llvm::Constant* GetScalarArgument(easy::ArgumentBase const& Arg, llvm::Type* T);

llvm::Constant* LinkPointerIfPossible(llvm::Module &M, easy::PtrArgument const &Ptr, llvm::Type* PtrTy);

llvm::AllocaInst* GetStructAlloc(llvm::IRBuilder<> &B, llvm::DataLayout const &DL, easy::StructArgument const &Struct, llvm::Type* StructPtrTy);

std::pair<llvm::Constant*, size_t> GetConstantFromRaw(llvm::DataLayout const& DL, llvm::Type* T, const uint8_t* Raw);

std::pair<llvm::Constant*, size_t> GetConstantFromRaw(llvm::DataLayout const& DL, llvm::Type* T, const uint8_t* Raw);

}

#endif // INLINEPARAMETERSHELPER_H
