#include <easy/runtime/RuntimePasses.h>
#include <easy/runtime/BitcodeTracker.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/AbstractCallSite.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Linker/Linker.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Support/raw_ostream.h>
#include <numeric>

using namespace llvm;

char easy::DevirtualizeConstant::ID = 0;

llvm::Pass* easy::createDevirtualizeConstantPass(llvm::StringRef Name) {
  return new DevirtualizeConstant(Name);
}

static ConstantInt* getVTableHostAddress(Value& V) {
    auto* VTable = dyn_cast<LoadInst>(&V);
    if(!VTable)
      return nullptr;
    MDNode *Tag = VTable->getMetadata(LLVMContext::MD_tbaa);
    if(!Tag || !Tag->isTBAAVtableAccess())
      return nullptr;

    // that's a vtable
    auto* Location = dyn_cast<Constant>(VTable->getPointerOperand()->stripPointerCasts());
    if(!Location)
      return nullptr;

    if(auto* CE = dyn_cast<ConstantExpr>(Location)) {
      if(CE->getOpcode() == Instruction::IntToPtr) {
        Location = CE->getOperand(0);
      }
    }
    auto* CLocation = dyn_cast<ConstantInt>(Location);
    if(!CLocation)
      return nullptr;
    return CLocation;
}

static Function* findFunctionAndLinkModules(Module& M, void* HostValue) {
    auto &BT = easy::BitcodeTracker::GetTracker();
    const char* FName = std::get<0>(BT.getNameAndGlobalMapping(HostValue));

    if(!FName)
      return nullptr;

    std::unique_ptr<Module> LM = BT.getModuleWithContext(HostValue, M.getContext());

    if(!Linker::linkModules(M, std::move(LM), Linker::OverrideFromSrc,
                            [](Module &, const StringSet<> &){}))
    {
      GlobalValue *GV = M.getNamedValue(FName);
      if(Function* F = dyn_cast<Function>(GV)) {
        F->setLinkage(Function::PrivateLinkage);
        return F;
      }
      else {
        assert(false && "wtf");
      }
    }
    return nullptr;
}

template<class IIter>
bool Devirtualize(IIter it, IIter end) {
  bool Changed = false;

  // We are trying to match %1 from CallInsts like %3
  // Matching %1 means we are doing a virtualized call in %3.

  // %1 = load ptr, ptr inttoptr (i64 187650338298944 to ptr), align 64, !tbaa !9
  // %2 = load ptr, ptr %1, align 8
  // %3 = tail call noundef i32 %2(ptr noundef nonnull align 8 dereferenceable(8) inttoptr (i64 187650338298944 to ptr))
  for (; it != end; ++it) {
    Instruction &I = *it;
    CallInst* CI = dyn_cast<CallInst>(&I); // %3
    if(!CI)
      continue;

    // Try to take us to where we load the VTable
    // This only happens when we are calling a temp, not a function
    if (CI->getCalledFunction())
      continue;
    LoadInst* LI = dyn_cast<LoadInst>(CI->getCalledOperand()); // %2

    // must come from a pointer load
    if (!LI)
      continue;
    
    LoadInst* LLI = dyn_cast<LoadInst>(LI->getOperand(0)); // %1
    if (!LLI)
      continue;
    
    auto* VTable = getVTableHostAddress(*LLI);
    if(!VTable)
      continue;

    void** RuntimeLoadedValue = *(void***)(uintptr_t)(VTable->getZExtValue());

    void* CalledPtrHostValue = *RuntimeLoadedValue;
    llvm::Function* F = findFunctionAndLinkModules(*LLI->getParent()->getParent()->getParent(), CalledPtrHostValue);
    if(!F)
      continue;

    LI->replaceAllUsesWith(F);

    Changed = true;
  }
  return Changed;
}

bool CastCallWithPointerCasts(FunctionType* CalledTy, FunctionType* UncastedTy) {
  if(CalledTy->getReturnType() != UncastedTy->getReturnType())
    return false;
  if(CalledTy->getNumParams() != UncastedTy->getNumParams())
    return false;
  if(CalledTy->isVarArg() != UncastedTy->isVarArg())
    return false;

  size_t N = CalledTy->getNumParams();
  for(size_t i = 0; i != N; ++i) {
    Type* CArgTy = CalledTy->getParamType(i);
    Type* UArgTy = UncastedTy->getParamType(i);
    if(CArgTy != UArgTy) {
      if(!CArgTy->isPointerTy() || !UArgTy->isPointerTy())
        return false;
    }
  }
  return true;
}

template<class IIter>
void RecastCalls(IIter it, IIter end) {
  for(;it != end;) {
    auto *CB = dyn_cast<CallBase>(&*it++);
    if(!CB)
        continue;

    Value* Called = CB->getCalledOperand();
    Value* Uncasted = Called->stripPointerCasts();
    if(Called == Uncasted)
      continue;

    FunctionType* CalledTy = cast<FunctionType>(Called->getType()->getContainedType(0));
    FunctionType* UncastedTy = cast<FunctionType>(Uncasted->getType()->getContainedType(0));

    if(!CastCallWithPointerCasts(CalledTy, UncastedTy))
      continue;

    Function* CalledFunction = CB->getCalledFunction();
    CB->setCalledFunction(CalledFunction);
    CB->mutateFunctionType(UncastedTy);

    // cast every pointer argument to the expected type
    IRBuilder<> B(CB);

    size_t N = CB->arg_size();
    for(unsigned i = 0; i != N; ++i) {
      Value* Arg = CB->getArgOperand(i);
      Type* ArgTy = Arg->getType();
      Type* UncTy = UncastedTy->getParamType(i);
      if(ArgTy->isPointerTy() && ArgTy != UncTy)
        CB->setArgOperand(i, B.CreatePointerCast(Arg, UncTy, Arg->getName() + ".recast_calls"));
    }
  }
}

bool easy::DevirtualizeConstant::runOnFunction(llvm::Function &F) {
  if(F.getName() != TargetName_)
    return false;

  if(Devirtualize(inst_begin(F), inst_end(F))) {
    RecastCalls(inst_begin(F), inst_end(F));
    return true;
  }
  return false;
}

static RegisterPass<easy::DevirtualizeConstant> X("","",false, false);
