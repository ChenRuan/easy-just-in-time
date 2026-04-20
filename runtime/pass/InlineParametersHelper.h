#ifndef INLINEPARAMETERSHELPER_H
#define INLINEPARAMETERSHELPER_H

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Constants.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/InstIterator.h>

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

struct PostLinkageSymbol {
  llvm::StringRef Name;
  ArgumentBase::ArgumentKind Kind;
  size_t ArgNo;
};

llvm::SmallVector<llvm::Value*, 4> GetForwardArgs(easy::HighLevelLayout::HighLevelArg &ArgInF, easy::HighLevelLayout &FHLL,
                                                  llvm::Function &Wrapper, easy::HighLevelLayout &WrapperHLL);
llvm::Constant* GetScalarArgument(easy::ArgumentBase const& Arg, llvm::Type* T);
std::pair<llvm::Constant*, size_t> GetAggregateConstantFromRaw(llvm::DataLayout const &DL,
                                                               llvm::Type* T,
                                                               const uint8_t* Raw);

llvm::Constant* LinkPointerIfPossible(llvm::Module &M, easy::PtrArgument const &Ptr, llvm::Type* PtrTy);
bool ApplyGlobalStructSnapshots(llvm::Module &M, llvm::StringRef TargetName, easy::Context const &C);

// Return true if any linkage happened
bool LinkAndUpdateSymbol(llvm::Module &M, llvm::StringRef FName, llvm::StringRef WrapperName, llvm::SmallVectorImpl<PostLinkageSymbol> &Symbols, easy::Context const &C, llvm::Value* CallToUpdate);

llvm::AllocaInst* GetStructAlloc(llvm::IRBuilder<> &B, llvm::DataLayout const &DL, easy::StructArgument const &Struct, llvm::Type* StructTy);
llvm::AllocaInst* GetPartialStructAlloc(llvm::IRBuilder<> &B,
                                        llvm::DataLayout const &DL,
                                        easy::PartialStructArgument const &Struct,
                                        llvm::Type* StructTy,
                                        llvm::Value* RuntimePtr);
llvm::Constant* GetArrayConstant(llvm::DataLayout const &DL, easy::ArrayArgument const &Array, llvm::Type* PointeeTy);
void ApplyStructArrayBindings(llvm::IRBuilder<> &B,
                              llvm::DataLayout const &DL,
                              std::vector<easy::StructArrayBinding> const &Bindings,
                              llvm::Type* StructTy,
                              llvm::AllocaInst* Alloc);
void ApplyStructFieldBindings(llvm::IRBuilder<> &B,
                              llvm::DataLayout const &DL,
                              std::vector<easy::StructFieldBinding> const &Bindings,
                              llvm::Type* StructTy,
                              llvm::AllocaInst* Alloc);

std::pair<llvm::Constant*, size_t> GetConstantFromRaw(llvm::DataLayout const& DL, llvm::Type* T, const uint8_t* Raw);

/// Discover the struct type a pointer argument points to by scanning
/// GEP / load / store instructions in the function body.
llvm::Type* FindPointeeStructType(llvm::Function &F, unsigned ArgIdx);

/// Discover the element type a pointer argument points to by scanning
/// GEP instructions in the function body.
llvm::Type* FindPointeeElementType(llvm::Function &F, unsigned ArgIdx);

}

#endif // INLINEPARAMETERSHELPER_H
