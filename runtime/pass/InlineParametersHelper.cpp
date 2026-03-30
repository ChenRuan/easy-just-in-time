#include "InlineParametersHelper.h"
#include "internal/UtilsInternal.h"
#include <easy/runtime/BitcodeTracker.h>
#include "internal/BitcodeTrackerInternal.h"
#include <easy/runtime/LLVMHolderImpl.h>

#include <llvm/Linker/Linker.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/SmallVector.h>

#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace easy;

HighLevelLayout::HighLevelLayout(easy::Context const& C, llvm::Function &F) {
  StructReturn_ = nullptr;

  FunctionType* FTy = F.getFunctionType();
  if(F.arg_begin()->hasStructRetAttr())
    StructReturn_ = F.getParamStructRetType(0);

  Return_ = FTy->getReturnType();

  auto &BT = easy::BitcodeTracker::GetTracker();

  size_t ParamIdx = 0;
  size_t ArgIdx = 0;
  if(StructReturn_)
    ParamIdx++;

  for(easy::layout_id lid : C.getLayout()) {
    size_t N = BT.getLayoutInfo(lid).NumFields;

    Args_.emplace_back(ArgIdx, ParamIdx);
    HighLevelArg& Arg = Args_.back();

    size_t ArgEnd = (ParamIdx+N);

    bool SingleArg = (ParamIdx + 1) == ArgEnd;
    if(SingleArg) {
      Type* ParamTy = FTy->getParamType(ParamIdx);
      Arg.Types_.push_back(ParamTy);
      Arg.StructByPointer_ = ParamTy->isPointerTy();
      Arg.StructByArray_ = ParamTy->isArrayTy();
      ++ParamIdx;
      ++ArgIdx;
    } else {
      for(; ParamIdx != ArgEnd; ++ParamIdx)
        Arg.Types_.push_back(FTy->getParamType(ParamIdx));
      ++ArgIdx;
    }
  }
}

llvm::SmallVector<llvm::Value*, 4>
easy::GetForwardArgs(easy::HighLevelLayout::HighLevelArg &ArgInF, easy::HighLevelLayout &FHLL,
               llvm::Function &Wrapper, easy::HighLevelLayout &WrapperHLL) {

  llvm::SmallVector<llvm::Value*, 4> Args;

  auto GetArg = [&Wrapper, &FHLL](size_t i) -> Argument*
    { return &*(Wrapper.arg_begin()+i+(FHLL.StructReturn_ ? 1 : 0)); };

  // find layout in the new wrapper
  size_t ArgPosition = ArgInF.Position_;
  auto &ArgInWrapper = *std::find_if(WrapperHLL.Args_.begin(), WrapperHLL.Args_.end(),
                                     [ArgPosition](HighLevelLayout::HighLevelArg &ArgInWrapper)
                                      { return ArgInWrapper.Position_ == ArgPosition; });

  for(size_t j = 0; j != ArgInF.Types_.size(); ++j) {
    Args.push_back(GetArg(ArgInWrapper.FirstParamIdx_ + j));
  }
  return Args;
}

Constant* easy::GetScalarArgument(ArgumentBase const& Arg, Type* T) {
  switch(Arg.kind()) {
    case easy::ArgumentBase::AK_Int: {
      auto const *Int = Arg.as<easy::IntArgument>();
      return ConstantInt::get(T, Int->get(), true);
    }
    case easy::ArgumentBase::AK_Float: {
      auto const *Float = Arg.as<easy::FloatArgument>();
      return ConstantFP::get(T, Float->get());
    }
    case easy::ArgumentBase::AK_Ptr: {
      auto const *Ptr = Arg.as<easy::PtrArgument>();
      uintptr_t Addr = (uintptr_t)Ptr->get();
      return ConstantExpr::getIntToPtr(
                ConstantInt::get(Type::getInt64Ty(T->getContext()), Addr, false),
                T);
    }
    default:
      return nullptr;
  }
}

llvm::StringRef easy::GetGlobalName(llvm::Module &M, easy::PtrArgument const &Ptr) {
  auto &BT = easy::BitcodeTracker::GetTracker();
  void* PtrValue = const_cast<void*>(Ptr.get());
  if(BT.hasGlobalMapping(PtrValue)) {
    return std::get<0>(BT.getNameAndGlobalMapping(PtrValue));
  }
  return "";
}

static
void UpdateCallArg(llvm::Value* Call, easy::Context const &C, Value* newArg, size_t argNo) {
  assert(Call != nullptr && "CallToUpdate is null.");
  if(auto *CI = dyn_cast<CallInst>(Call)) {
    CI->setArgOperand(argNo, newArg);
  }
  else {
    report_fatal_error("CallToUpdate is not a call instruction.", true);
  }
}

bool easy::LinkAndUpdateSymbol(llvm::Module &M, llvm::StringRef FName, llvm::StringRef WrapperName, llvm::SmallVectorImpl<PostLinkageSymbol> &Symbols, easy::Context const &C, llvm::Value* CallToUpdate) {
  assert(CallToUpdate != nullptr);
  assert(llvm::isa<llvm::CallInst>(CallToUpdate) && "CallToUpdate is not a call instruction.");

  auto &BT = easy::BitcodeTracker::GetTracker();
  SmallVector<std::unique_ptr<llvm::Module>,8> ModulesToLink;

  // Collect modules to link
  for (auto& Symbol : Symbols) {
    auto const &Arg = C.getArgumentMapping(Symbol.ArgNo);
    switch (Arg.kind()) {
      case easy::ArgumentBase::AK_Ptr: {
        auto const *Ptr = Arg.as<easy::PtrArgument>();
        Constant* PtrVal = GetScalarArgument(Arg, PointerType::getUnqual(M.getContext()));
        void * PtrValue = const_cast<void*>(Ptr->get());
        if(BT.hasGlobalMapping(PtrValue)) {
          std::unique_ptr<Module> LM = easy::BT_getModuleWithContext(BT, PtrValue, M.getContext());
          ModulesToLink.push_back(std::move(LM));
        }
      } break;
      case easy::ArgumentBase::AK_Module: {
        easy::Function const &Function = Arg.as<easy::ModuleArgument>()->get();
        auto const *HI = static_cast<easy::LLVMHolderImpl const*>(Function.getHolder());
        llvm::Module const &FunctionModule = *HI->M_;
        std::unique_ptr<llvm::Module> LM = 
          easy::CloneModuleWithContext(FunctionModule, M.getContext());
        assert(LM);
        easy::UnmarkEntry(*LM);
        ModulesToLink.push_back(std::move(LM));
      } break;
      default:
        break;
    }
  }

  // Link modules
  for(auto &LM : ModulesToLink) {
    if(Linker::linkModules(M, std::move(LM), Linker::OverrideFromSrc,
                            [](Module &, const StringSet<> &){})) {
      llvm::report_fatal_error("Failed to link with module!", true);
    }
  }

  if (ModulesToLink.empty()) 
    return false;

  // Look up the symbol
  for (auto& Symbol : Symbols) {
    StringRef Name = Symbol.Name;
    assert(!Name.empty() && "Unnamed symbol shouldn't reach here.");
    auto Kind = Symbol.Kind;
    auto *GV = M.getNamedValue(Name);
    switch (Kind) {
      case easy::ArgumentBase::AK_Ptr: {
        if (GlobalVariable *G = dyn_cast<GlobalVariable>(GV)) {
          GV->setLinkage(llvm::Function::PrivateLinkage);
          assert(GV->getType()->isPointerTy() && "Global variable passed as ptr arg is not a pointer");
          UpdateCallArg(CallToUpdate, C, GV, Symbol.ArgNo);
        }
        else if (llvm::Function *F = dyn_cast<llvm::Function>(GV)) {
          F->setLinkage(llvm::Function::PrivateLinkage);
          UpdateCallArg(CallToUpdate, C, F, Symbol.ArgNo);
        }
        else { 
          report_fatal_error("Global varialbe passed as ptr but is not a pointer nor a function", true);
        }
      } break;
      case easy::ArgumentBase::AK_Module: {
        llvm::Function* FunctionInM = M.getFunction(Name);
        FunctionInM->setLinkage(llvm::Function::PrivateLinkage);
        UpdateCallArg(CallToUpdate, C, FunctionInM, Symbol.ArgNo);
      } break;
      default:
        break;
    }
    
  }

  return true;
}

std::pair<llvm::Constant*, size_t> easy::GetConstantFromRaw(llvm::DataLayout const& DL,
                                                            llvm::Type* T, const uint8_t* Raw) {
  // pack in a I8 constant vector and cast
  Type* I8 = Type::getInt8Ty(T->getContext());
  size_t Size = DL.getTypeStoreSize(T); // TODO: not sure about this

  SmallVector<Constant*, sizeof(uint64_t)> Elements(Size, nullptr);
  for(size_t i = 0; i != Size; ++i) {
    Elements[i] = ConstantInt::get(I8, Raw[i]);
  }

  Constant* DataAsI8 = ConstantVector::get(Elements);
  Constant* DataAsT;
  if(T->isPointerTy()) {
    Type* TInt = DL.getIntPtrType(T->getContext());
    Constant* DataAsTSizedInt = ConstantExpr::getBitCast(DataAsI8, TInt);
    DataAsT = ConstantExpr::getIntToPtr(DataAsTSizedInt, T);
  } else {
    DataAsT = ConstantExpr::getBitCast(DataAsI8, T);
  }
  return {DataAsT, Size};
}

llvm::Constant* easy::GetArrayConstant(llvm::DataLayout const &DL,
                                       easy::ArrayArgument const &Array,
                                       llvm::Type* PointeeTy) {
  size_t Count = Array.getCount();
  size_t ElemSize = Array.getElementSize();
  assert(Array.get().size() == Count * ElemSize &&
         "snapshot array buffer size does not match element count");
  SmallVector<Constant*, 8> Elements;
  Elements.reserve(Count);

  for (size_t I = 0; I != Count; ++I) {
    auto const *ElemRaw = reinterpret_cast<uint8_t const*>(Array.get().data() + I * ElemSize);
    Constant* ElemConst;
    size_t RawSize;
    std::tie(ElemConst, RawSize) = easy::GetConstantFromRaw(DL, PointeeTy, ElemRaw);
    assert(RawSize == ElemSize &&
           "snapshot array element size does not match inferred pointee type");
    Elements.push_back(ElemConst);
  }

  return ConstantArray::get(ArrayType::get(PointeeTy, Count), Elements);
}

static
size_t StoreStructField(llvm::IRBuilder<> &B,
                        llvm::DataLayout const &DL,
                        Type* Ty,
                        uint8_t const* Raw,
                        AllocaInst* Alloc, SmallVectorImpl<Value*> &GEP) {

  StructType* STy = dyn_cast<StructType>(Ty);
  if(STy) {
    const StructLayout *SL = DL.getStructLayout(STy);
    size_t Fields = STy->getNumContainedTypes();
    for(size_t Field = 0; Field != Fields; ++Field) {
      GEP.push_back(B.getInt32(Field));
      size_t FieldOffset = SL->getElementOffset(Field);
      StoreStructField(B, DL, STy->getElementType(Field), Raw + FieldOffset, Alloc, GEP);
      GEP.pop_back();
    }
    return DL.getTypeAllocSize(STy);
  } else if (ArrayType* ATy = dyn_cast<ArrayType>(Ty)) {
    size_t ElemCount = ATy->getNumElements();
    size_t ElemAllocSize = DL.getTypeAllocSize(ATy->getElementType());
    for (size_t Index = 0; Index != ElemCount; ++Index) {
      GEP.push_back(B.getInt32(Index));
      StoreStructField(B, DL, ATy->getElementType(), Raw + Index * ElemAllocSize, Alloc, GEP);
      GEP.pop_back();
    }
    return DL.getTypeAllocSize(ATy);
  } else {
    Constant* FieldValue;
    size_t StoreSize;
    std::tie(FieldValue, StoreSize) = easy::GetConstantFromRaw(DL, Ty, (uint8_t const*)Raw);

    Value* FieldPtr = B.CreateGEP(Alloc->getAllocatedType(), Alloc, GEP, "field.gep");
    B.CreateStore(FieldValue, FieldPtr);
    return StoreSize;
  }
}

llvm::AllocaInst* easy::GetStructAlloc(llvm::IRBuilder<> &B,
                                       llvm::DataLayout const &DL,
                                       easy::StructArgument const &Struct,
                                       llvm::Type* StructTy) {
  AllocaInst* Alloc = B.CreateAlloca(StructTy);
  B.CreateStore(Constant::getNullValue(StructTy), Alloc);

  SmallVector<Value*, 4> GEP = {B.getInt32(0)};

  auto const &Data = Struct.get();
  size_t ExpectedSize = DL.getTypeAllocSize(StructTy);
  if (Data.size() != ExpectedSize) {
    errs() << "WARNING: GetStructAlloc: raw data size (" << Data.size()
           << ") != LLVM struct alloc size (" << ExpectedSize << ")\n";
  }

  StoreStructField(B, DL, StructTy, (uint8_t const*)Data.data(), Alloc, GEP);

  return Alloc;
}

/// Discover the struct type a pointer argument points to by scanning
/// GEP / load / store instructions in the function body.
/// Returns nullptr if no struct type can be found.
llvm::Type* easy::FindPointeeStructType(llvm::Function &F, unsigned ArgIdx) {
  Argument *Arg = F.getArg(ArgIdx);

  // First, check for byval attribute
  if (Arg->hasByValAttr()) {
    Type *T = Arg->getParamByValType();
    if (T && isa<StructType>(T))
      return T;
  }

  // Collect all values that hold the pointer (including loads from allocas
  // where the arg was stored – the typical -O0 pattern).
  SmallVector<Value*, 8> Worklist;
  SmallSet<Value*, 8> Visited;
  Worklist.push_back(Arg);

  while (!Worklist.empty()) {
    Value *V = Worklist.pop_back_val();
    if (!Visited.insert(V).second)
      continue;

    for (User *U : V->users()) {
      if (auto *GEP = dyn_cast<GetElementPtrInst>(U)) {
        Type *SrcTy = GEP->getSourceElementType();
        if (isa<StructType>(SrcTy))
          return SrcTy;
      }
      if (auto *LI = dyn_cast<LoadInst>(U)) {
        Type *LoadedTy = LI->getType();
        if (isa<StructType>(LoadedTy))
          return LoadedTy;
      }
      // arg → store to alloca → load from alloca → GEP
      if (auto *SI = dyn_cast<StoreInst>(U)) {
        if (SI->getValueOperand() == V) {
          Value *Ptr = SI->getPointerOperand();
          if (auto *AI = dyn_cast<AllocaInst>(Ptr)) {
            for (User *AU : AI->users()) {
              if (auto *LI = dyn_cast<LoadInst>(AU))
                Worklist.push_back(LI);
            }
          }
        }
      }
    }
  }

  return nullptr;
}

/// Discover the element type a pointer argument points to by scanning
/// GEP / load instructions in the function body.
/// Returns nullptr if no element type can be found.
llvm::Type* easy::FindPointeeElementType(llvm::Function &F, unsigned ArgIdx) {
  Argument *Arg = F.getArg(ArgIdx);

  // Collect all values that hold the pointer (including loads from allocas
  // where the arg was stored – the typical -O0 pattern).
  SmallVector<Value*, 8> Worklist;
  SmallSet<Value*, 8> Visited;
  Worklist.push_back(Arg);

  while (!Worklist.empty()) {
    Value *V = Worklist.pop_back_val();
    if (!Visited.insert(V).second)
      continue;

    for (User *U : V->users()) {
      if (auto *GEP = dyn_cast<GetElementPtrInst>(U)) {
        return GEP->getSourceElementType();
      }
      if (auto *LI = dyn_cast<LoadInst>(U)) {
        // If the loaded type is not a pointer, it's the element type
        if (!LI->getType()->isPointerTy())
          return LI->getType();
      }
      // arg → store to alloca → load from alloca → GEP / load
      if (auto *SI = dyn_cast<StoreInst>(U)) {
        if (SI->getValueOperand() == V) {
          Value *Ptr = SI->getPointerOperand();
          if (auto *AI = dyn_cast<AllocaInst>(Ptr)) {
            for (User *AU : AI->users()) {
              if (auto *LI = dyn_cast<LoadInst>(AU))
                Worklist.push_back(LI);
            }
          }
        }
      }
    }
  }

  return nullptr;
}
