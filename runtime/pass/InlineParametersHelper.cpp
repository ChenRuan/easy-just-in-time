#include "InlineParametersHelper.h"
#include <easy/runtime/BitcodeTracker.h>
#include "../SreDebugLog.h"

#include <llvm/Linker/Linker.h>
#include <llvm/ADT/APInt.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/GlobalVariable.h>

#include <llvm/Support/raw_ostream.h>
#include <cstdio>

using namespace llvm;
using namespace easy;

#define EASYJIT_RT_PASS_LOG(...) EASYJIT_SRE_LOG("[pass] " __VA_ARGS__)

HighLevelLayout::HighLevelLayout(easy::Context const& C, llvm::Function &F) {
  EASYJIT_RT_PASS_LOG("HighLevelLayout: begin fn=%s ctx_size=%zu args=%zu\n",
                      F.getName().str().c_str(), C.size(), F.arg_size());
  StructReturn_ = nullptr;

  FunctionType* FTy = F.getFunctionType();
  if(F.arg_begin()->hasStructRetAttr())
    StructReturn_ = FTy->getParamType(0);

  Return_ = FTy->getReturnType();

  auto &BT = easy::BitcodeTracker::GetTracker();

  size_t ParamIdx = 0;
  size_t ArgIdx = 0;
  if(StructReturn_)
    ParamIdx++;

  for(easy::layout_id lid : C.getLayout()) {
    EASYJIT_RT_PASS_LOG("HighLevelLayout: layout arg=%zu id=%p param_idx=%zu\n",
                        ArgIdx, lid, ParamIdx);
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
      EASYJIT_RT_PASS_LOG("HighLevelLayout: arg=%zu single type ptr=%d array=%d\n",
                          ArgIdx, (int)Arg.StructByPointer_,
                          (int)Arg.StructByArray_);
      ++ParamIdx;
      ++ArgIdx;
    } else {
      for(; ParamIdx != ArgEnd; ++ParamIdx)
        Arg.Types_.push_back(FTy->getParamType(ParamIdx));
      EASYJIT_RT_PASS_LOG("HighLevelLayout: arg=%zu multi fields=%zu\n",
                          ArgIdx, Arg.Types_.size());
      ++ArgIdx;
    }
  }
  EASYJIT_RT_PASS_LOG("HighLevelLayout: end args=%zu return_void=%d sret=%d\n",
                      Args_.size(), Return_->isVoidTy() ? 1 : 0,
                      StructReturn_ ? 1 : 0);
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
    EASYJIT_RT_PASS_LOG("GetForwardArgs: position=%zu field=%zu wrapper_param=%zu\n",
                        ArgPosition, j, ArgInWrapper.FirstParamIdx_ + j);
  }
  return Args;
}

Constant* easy::GetScalarArgument(ArgumentBase const& Arg, Type* T) {
  EASYJIT_RT_PASS_LOG("GetScalarArgument: kind=%d type_ptr=%p\n",
                      (int)Arg.kind(), (void*)T);
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

llvm::Constant* easy::LinkPointerIfPossible(llvm::Module &M, easy::PtrArgument const &Ptr, Type* PtrTy) {
  auto &BT = easy::BitcodeTracker::GetTracker();
  void* PtrValue = const_cast<void*>(Ptr.get());
  EASYJIT_RT_PASS_LOG("LinkPointerIfPossible: ptr=%p module=%p\n", PtrValue, (void*)&M);
  if(BT.hasGlobalMapping(PtrValue)) {
    const char* LName = std::get<0>(BT.getNameAndGlobalMapping(PtrValue));
    EASYJIT_RT_PASS_LOG("LinkPointerIfPossible: has mapping name=%s\n",
                        LName ? LName : "<null>");
    std::unique_ptr<Module> LM = BT.getModuleWithContext(PtrValue, M.getContext());

    if(!Linker::linkModules(M, std::move(LM), Linker::OverrideFromSrc,
                            [](Module &, const StringSet<> &){}))
    {
      GlobalValue *GV = M.getNamedValue(LName);
      if(GlobalVariable* G = dyn_cast<GlobalVariable>(GV)) {
        GV->setLinkage(llvm::Function::PrivateLinkage);
        if(GV->getType() != PtrTy) {
          EASYJIT_RT_PASS_LOG("LinkPointerIfPossible: linked global cast name=%s\n", LName);
          return ConstantExpr::getPointerCast(GV, PtrTy);
        }
        EASYJIT_RT_PASS_LOG("LinkPointerIfPossible: linked global name=%s\n", LName);
        return GV;
      }
      else if(llvm::Function* F = dyn_cast<llvm::Function>(GV)) {
        F->setLinkage(llvm::Function::PrivateLinkage);
        EASYJIT_RT_PASS_LOG("LinkPointerIfPossible: linked function name=%s\n", LName);
        return F;
      }
      assert(false && "wtf");
    }
  }
  EASYJIT_RT_PASS_LOG("LinkPointerIfPossible: no mapping ptr=%p\n", PtrValue);
  return nullptr;
}

std::pair<llvm::Constant*, size_t> easy::GetConstantFromRaw(llvm::DataLayout const& DL,
                                                            llvm::Type* T, const uint8_t* Raw) {
  size_t Size = DL.getTypeStoreSize(T);
  EASYJIT_RT_PASS_LOG("GetConstantFromRaw: type=%s size=%zu little_endian=%d\n",
                      std::string("").append([&]() {
                        std::string S;
                        llvm::raw_string_ostream OS(S);
                        T->print(OS);
                        OS.flush();
                        return S;
                      }()).c_str(),
                      Size,
                      DL.isLittleEndian() ? 1 : 0);

  auto GetBitsFromRaw = [&](unsigned BitWidth) {
    llvm::APInt Bits(BitWidth, 0);
    for (size_t I = 0; I != Size; ++I) {
      size_t ByteOffset = DL.isLittleEndian() ? I : (Size - 1 - I);
      Bits |= llvm::APInt(BitWidth, Raw[I]) << (ByteOffset * 8);
    }
    return Bits;
  };

  if (T->isIntegerTy()) {
    unsigned BitWidth = T->getIntegerBitWidth();
    llvm::APInt Bits = GetBitsFromRaw(std::max<unsigned>(BitWidth, Size * 8));
    if (Bits.getBitWidth() != BitWidth) {
      Bits = Bits.trunc(BitWidth);
    }
    return {ConstantInt::get(T, Bits), Size};
  }

  if (T->isFloatingPointTy()) {
    unsigned BitWidth = T->getPrimitiveSizeInBits();
    llvm::Type* IntTy = Type::getIntNTy(T->getContext(), BitWidth);
    llvm::APInt Bits = GetBitsFromRaw(BitWidth);
    Constant* IntValue = ConstantInt::get(IntTy, Bits);
    return {ConstantExpr::getBitCast(IntValue, T), Size};
  }

  if (T->isPointerTy()) {
    Type* TInt = DL.getIntPtrType(T->getContext());
    unsigned BitWidth = TInt->getIntegerBitWidth();
    llvm::APInt Bits = GetBitsFromRaw(BitWidth);
    Constant* IntValue = ConstantInt::get(TInt, Bits);
    return {ConstantExpr::getIntToPtr(IntValue, T), Size};
  }

  // Fallback for uncommon scalar leaf types.
  Type* I8 = Type::getInt8Ty(T->getContext());
  SmallVector<Constant*, sizeof(uint64_t)> Elements(Size, nullptr);
  for (size_t I = 0; I != Size; ++I) {
    Elements[I] = ConstantInt::get(I8, Raw[I]);
  }
  Constant* DataAsI8 = ConstantVector::get(Elements);
  return {ConstantExpr::getBitCast(DataAsI8, T), Size};
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

std::pair<llvm::Constant*, size_t> easy::GetAggregateConstantFromRaw(llvm::DataLayout const &DL,
                                                                     llvm::Type* T,
                                                                     const uint8_t* Raw) {
  if (auto *STy = dyn_cast<StructType>(T)) {
    SmallVector<Constant*, 8> Fields;
    Fields.reserve(STy->getNumElements());
    auto const *SL = DL.getStructLayout(STy);
    for (unsigned Field = 0; Field != STy->getNumElements(); ++Field) {
      size_t FieldOffset = SL->getElementOffset(Field);
      auto FieldValue = easy::GetAggregateConstantFromRaw(DL, STy->getElementType(Field), Raw + FieldOffset);
      Fields.push_back(FieldValue.first);
    }
    return {ConstantStruct::get(STy, Fields), DL.getTypeAllocSize(STy)};
  }

  if (auto *ATy = dyn_cast<ArrayType>(T)) {
    SmallVector<Constant*, 8> Elements;
    Elements.reserve(ATy->getNumElements());
    size_t ElemSize = DL.getTypeAllocSize(ATy->getElementType());
    for (uint64_t Index = 0; Index != ATy->getNumElements(); ++Index) {
      auto ElemValue =
          easy::GetAggregateConstantFromRaw(DL, ATy->getElementType(), Raw + Index * ElemSize);
      Elements.push_back(ElemValue.first);
    }
    return {ConstantArray::get(ATy, Elements), DL.getTypeAllocSize(ATy)};
  }

  return easy::GetConstantFromRaw(DL, T, Raw);
}

static const char* FindGlobalNameForAddress(easy::GlobalMapping* Globals, const void* Address) {
  for (easy::GlobalMapping* GM = Globals; GM && GM->Name; ++GM) {
    if (GM->Address == Address)
      return GM->Name;
  }
  return nullptr;
}

static bool FindLeafFieldPathByOffset(llvm::DataLayout const &DL,
                                      llvm::Type* Ty,
                                      size_t Offset,
                                      SmallVectorImpl<unsigned> &Indices,
                                      llvm::Type*& FieldTy);
static llvm::Constant* GetRawByteArrayPointer(Module &M,
                                              easy::StructArrayBinding const &Binding);

static bool ExtractGlobalFieldIndices(llvm::Value* Ptr,
                                      llvm::GlobalVariable* GV,
                                      SmallVectorImpl<unsigned> &Indices) {
  Ptr = Ptr->stripPointerCasts();
  if (Ptr == GV)
    return true;

  auto *GEP = dyn_cast<GEPOperator>(Ptr);
  if (!GEP)
    return false;

  if (!ExtractGlobalFieldIndices(GEP->getPointerOperand(), GV, Indices))
    return false;

  bool First = true;
  for (auto It = GEP->idx_begin(); It != GEP->idx_end(); ++It) {
    auto *CI = dyn_cast<ConstantInt>(It->get());
    if (!CI)
      return false;
    if (First && CI->isZero()) {
      First = false;
      continue;
    }
    First = false;
    Indices.push_back(static_cast<unsigned>(CI->getZExtValue()));
  }
  return true;
}

static bool ReplaceGlobalFieldLoads(llvm::Module &M,
                                    llvm::GlobalVariable* GV,
                                    ArrayRef<unsigned> FieldIndices,
                                    llvm::Constant* Replacement) {
  SmallVector<LoadInst*, 8> LoadsToReplace;
  for (llvm::Function &F : M) {
    for (llvm::Instruction &I : instructions(F)) {
      auto *LI = dyn_cast<LoadInst>(&I);
      if (!LI)
        continue;

      SmallVector<unsigned, 8> Indices;
      if (!ExtractGlobalFieldIndices(LI->getPointerOperand(), GV, Indices))
        continue;
      bool Matches = (Indices == FieldIndices);
      if (!Matches && Indices.empty() && FieldIndices.size() == 1 &&
          FieldIndices.front() == 0 &&
          LI->getPointerOperand()->stripPointerCasts() == GV) {
        Matches = true;
      }
      if (Matches)
        LoadsToReplace.push_back(LI);
    }
  }

  for (LoadInst* LI : LoadsToReplace) {
    llvm::Constant* Value = Replacement;
    if (Value->getType() != LI->getType()) {
      if (Value->getType()->isPointerTy() && LI->getType()->isPointerTy()) {
        Value = llvm::ConstantExpr::getPointerCast(Value, LI->getType());
      } else {
        continue;
      }
    }
    LI->replaceAllUsesWith(Value);
    LI->eraseFromParent();
  }
  return !LoadsToReplace.empty();
}

static llvm::Constant* BuildGlobalArrayPointer(Module &M,
                                               easy::StructArrayBinding const &Binding) {
  return GetRawByteArrayPointer(M, Binding);
}

bool easy::ApplyGlobalStructSnapshots(llvm::Module &M, llvm::StringRef TargetName, easy::Context const &C) {
  EASYJIT_RT_PASS_LOG("ApplyGlobalStructSnapshots: target=%s bindings=%zu\n",
                      TargetName.str().c_str(), C.getGlobalStructBindings().size());
  if (C.getGlobalStructBindings().empty())
    return false;

  auto &BT = easy::BitcodeTracker::GetTracker();
  void* HostFunction = BT.getAddress(TargetName.str());
  if (!HostFunction) {
    EASYJIT_RT_PASS_LOG("ApplyGlobalStructSnapshots: no host function target=%s\n",
                        TargetName.str().c_str());
    errs() << "WARNING: global snapshot could not resolve host function for "
           << TargetName << "\n";
    return false;
  }

  GlobalMapping* Globals = nullptr;
  std::tie(std::ignore, Globals) = BT.getNameAndGlobalMapping(HostFunction);
  if (!Globals) {
    EASYJIT_RT_PASS_LOG("ApplyGlobalStructSnapshots: no globals target=%s\n",
                        TargetName.str().c_str());
    errs() << "WARNING: global snapshot has no global mapping table for "
           << TargetName << "\n";
    return false;
  }

  bool Changed = false;
  llvm::DataLayout const &DL = M.getDataLayout();

  for (auto const &Binding : C.getGlobalStructBindings()) {
    EASYJIT_RT_PASS_LOG("ApplyGlobalStructSnapshots: binding addr=%p whole=%d bytes=%zu fields=%zu arrays=%zu\n",
                        Binding.Address_, (int)Binding.WholeSnapshot_,
                        Binding.Data_.size(), Binding.FieldBindings_.size(),
                        Binding.ArrayBindings_.size());
    const char* GlobalName = FindGlobalNameForAddress(Globals, Binding.Address_);
    if (!GlobalName) {
      EASYJIT_RT_PASS_LOG("ApplyGlobalStructSnapshots: no symbol for addr=%p\n",
                          Binding.Address_);
      errs() << "WARNING: global snapshot could not resolve symbol for host address "
             << Binding.Address_ << "\n";
      continue;
    }

    GlobalVariable* GV = M.getNamedGlobal(GlobalName);
    if (!GV) {
      EASYJIT_RT_PASS_LOG("ApplyGlobalStructSnapshots: no llvm global name=%s\n",
                          GlobalName);
      errs() << "WARNING: global snapshot could not find llvm global " << GlobalName << "\n";
      continue;
    }

    if (Binding.WholeSnapshot_) {
      EASYJIT_RT_PASS_LOG("ApplyGlobalStructSnapshots: whole snapshot global=%s bytes=%zu\n",
                          GlobalName, Binding.Data_.size());
      auto InitValue =
          easy::GetAggregateConstantFromRaw(DL,
                                            GV->getValueType(),
                                            reinterpret_cast<uint8_t const*>(Binding.Data_.data()));
      if (InitValue.second != Binding.Data_.size()) {
        errs() << "WARNING: global snapshot size mismatch for " << GlobalName
               << ": binding bytes=" << Binding.Data_.size()
               << " llvm alloc size=" << InitValue.second << "\n";
        continue;
      }

      GV->setInitializer(InitValue.first);
      GV->setConstant(true);
      GV->setLinkage(GlobalValue::PrivateLinkage);
      Changed = true;
      continue;
    }

    for (auto const &FieldBinding : Binding.FieldBindings_) {
      EASYJIT_RT_PASS_LOG("ApplyGlobalStructSnapshots: field global=%s offset=%zu bytes=%zu\n",
                          GlobalName, FieldBinding.Offset_, FieldBinding.Data_.size());
      SmallVector<unsigned, 8> Indices;
      Type* FieldTy = nullptr;
      if (!FindLeafFieldPathByOffset(DL, GV->getValueType(), FieldBinding.Offset_, Indices, FieldTy)) {
        errs() << "WARNING: global partial snapshot could not resolve field offset "
               << FieldBinding.Offset_ << " for " << GlobalName << "\n";
        continue;
      }

      Constant* FieldValue;
      size_t RawSize;
      std::tie(FieldValue, RawSize) =
          easy::GetConstantFromRaw(DL, FieldTy, reinterpret_cast<uint8_t const*>(FieldBinding.Data_.data()));
      if (RawSize != FieldBinding.Data_.size()) {
        errs() << "WARNING: global partial snapshot raw size mismatch at offset "
               << FieldBinding.Offset_ << " for " << GlobalName << "\n";
        continue;
      }

      Changed |= ReplaceGlobalFieldLoads(M, GV, Indices, FieldValue);
    }

    for (auto const &ArrayBinding : Binding.ArrayBindings_) {
      EASYJIT_RT_PASS_LOG("ApplyGlobalStructSnapshots: array global=%s offset=%zu count=%zu elem=%zu bytes=%zu\n",
                          GlobalName, ArrayBinding.Offset_, ArrayBinding.Count_,
                          ArrayBinding.ElementSize_, ArrayBinding.Data_.size());
      SmallVector<unsigned, 8> Indices;
      Type* FieldTy = nullptr;
      if (!FindLeafFieldPathByOffset(DL, GV->getValueType(), ArrayBinding.Offset_, Indices, FieldTy)) {
        errs() << "WARNING: global partial snapshot could not resolve array field offset "
               << ArrayBinding.Offset_ << " for " << GlobalName << "\n";
        continue;
      }
      if (!FieldTy->isPointerTy()) {
        errs() << "WARNING: global partial snapshot array binding offset "
               << ArrayBinding.Offset_ << " does not resolve to a pointer field for "
               << GlobalName << "\n";
        continue;
      }

      Changed |= ReplaceGlobalFieldLoads(M, GV, Indices, BuildGlobalArrayPointer(M, ArrayBinding));
    }

    auto *IntPtrTy = DL.getIntPtrType(M.getContext());
    auto HostAddr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(Binding.Address_));
    Constant* HostPtr = ConstantExpr::getIntToPtr(ConstantInt::get(IntPtrTy, HostAddr), GV->getType());
    GV->replaceAllUsesWith(HostPtr);
    if (GV->use_empty())
      GV->eraseFromParent();
    Changed = true;
  }

  EASYJIT_RT_PASS_LOG("ApplyGlobalStructSnapshots: end changed=%d\n", (int)Changed);
  return Changed;
}

static Constant* GetRawByteArrayPointer(Module &M,
                                        easy::StructArrayBinding const &Binding) {
  EASYJIT_RT_PASS_LOG("GetRawByteArrayPointer: module=%p bytes=%zu elem=%zu count=%zu\n",
                      (void*)&M, Binding.Data_.size(), Binding.ElementSize_,
                      Binding.Count_);
  LLVMContext &Ctx = M.getContext();
  auto *I8 = Type::getInt8Ty(Ctx);
  SmallVector<uint8_t, 16> Bytes;
  Bytes.reserve(Binding.Data_.size());
  for (char Byte : Binding.Data_)
    Bytes.push_back(static_cast<uint8_t>(Byte));

  Constant *Init = ConstantDataArray::get(Ctx, Bytes);
  auto *GV = new GlobalVariable(M,
                                Init->getType(),
                                true,
                                GlobalValue::PrivateLinkage,
                                Init,
                                "__easy_snapshot_struct_array");
  if (Binding.ElementSize_ != 0)
    GV->setAlignment(llvm::Align(Binding.ElementSize_));
  Constant* Zero = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
  Constant* Indices[] = {Zero, Zero};
  return ConstantExpr::getInBoundsGetElementPtr(Init->getType(), GV, Indices);
}

static bool FindLeafFieldPathByOffset(llvm::DataLayout const &DL,
                                      llvm::Type* Ty,
                                      size_t Offset,
                                      SmallVectorImpl<unsigned> &Indices,
                                      llvm::Type*& FieldTy) {
  if (auto *STy = dyn_cast<StructType>(Ty)) {
    auto const *SL = DL.getStructLayout(STy);
    for (unsigned Field = 0; Field != STy->getNumElements(); ++Field) {
      size_t FieldOffset = SL->getElementOffset(Field);
      Type* ElemTy = STy->getElementType(Field);
      size_t FieldSize = DL.getTypeAllocSize(ElemTy);
      if (Offset < FieldOffset || Offset >= FieldOffset + FieldSize)
        continue;

      Indices.push_back(Field);
      if (Offset == FieldOffset && !ElemTy->isAggregateType()) {
        FieldTy = ElemTy;
        return true;
      }

      if (FindLeafFieldPathByOffset(DL, ElemTy, Offset - FieldOffset, Indices, FieldTy))
        return true;

      Indices.pop_back();
    }
    return false;
  }

  if (auto *ATy = dyn_cast<ArrayType>(Ty)) {
    size_t ElemSize = DL.getTypeAllocSize(ATy->getElementType());
    if (ElemSize == 0)
      return false;
    size_t Index = Offset / ElemSize;
    if (Index >= ATy->getNumElements())
      return false;
    Indices.push_back(static_cast<unsigned>(Index));
    if (Offset == Index * ElemSize && !ATy->getElementType()->isAggregateType()) {
      FieldTy = ATy->getElementType();
      return true;
    }
    if (FindLeafFieldPathByOffset(DL, ATy->getElementType(), Offset - Index * ElemSize, Indices, FieldTy))
      return true;
    Indices.pop_back();
    return false;
  }

  return false;
}

void easy::ApplyStructArrayBindings(llvm::IRBuilder<> &B,
                                    llvm::DataLayout const &DL,
                                    std::vector<easy::StructArrayBinding> const &Bindings,
                                    llvm::Type* StructTy,
                                    llvm::AllocaInst* Alloc) {
  for (auto const &Binding : Bindings) {
    EASYJIT_RT_PASS_LOG("ApplyStructArrayBindings: offset=%zu count=%zu elem=%zu bytes=%zu\n",
                        Binding.Offset_, Binding.Count_, Binding.ElementSize_,
                        Binding.Data_.size());
    SmallVector<unsigned, 8> Indices;
    Type* FieldTy = nullptr;
    if (!FindLeafFieldPathByOffset(DL, StructTy, Binding.Offset_, Indices, FieldTy)) {
      errs() << "WARNING: snapshot bind_array could not resolve field offset "
             << Binding.Offset_ << "\n";
      continue;
    }
    if (!FieldTy->isPointerTy()) {
      errs() << "WARNING: snapshot bind_array field offset " << Binding.Offset_
             << " does not resolve to a pointer field\n";
      continue;
    }

    Value* FieldPtr = Alloc;
    SmallVector<Value*, 8> GEP = {B.getInt32(0)};
    for (unsigned Index : Indices)
      GEP.push_back(B.getInt32(Index));
    FieldPtr = B.CreateGEP(Alloc->getAllocatedType(), Alloc, GEP, "bound.array.field.gep");

    Constant* BoundPtr = GetRawByteArrayPointer(*Alloc->getModule(), Binding);
    B.CreateStore(BoundPtr, FieldPtr);
  }
}

void easy::ApplyStructFieldBindings(llvm::IRBuilder<> &B,
                                    llvm::DataLayout const &DL,
                                    std::vector<easy::StructFieldBinding> const &Bindings,
                                    llvm::Type* StructTy,
                                    llvm::AllocaInst* Alloc) {
  for (auto const &Binding : Bindings) {
    EASYJIT_RT_PASS_LOG("ApplyStructFieldBindings: offset=%zu bytes=%zu\n",
                        Binding.Offset_, Binding.Data_.size());
    SmallVector<unsigned, 8> Indices;
    Type* FieldTy = nullptr;
    if (!FindLeafFieldPathByOffset(DL, StructTy, Binding.Offset_, Indices, FieldTy)) {
      errs() << "WARNING: partial struct bind_field could not resolve field offset "
             << Binding.Offset_ << "\n";
      continue;
    }

    Constant* FieldValue;
    size_t RawSize;
    std::tie(FieldValue, RawSize) =
        easy::GetConstantFromRaw(DL, FieldTy,
                                 reinterpret_cast<uint8_t const*>(Binding.Data_.data()));
    if (RawSize != Binding.Data_.size()) {
      errs() << "WARNING: partial struct bind_field raw size mismatch at offset "
             << Binding.Offset_ << ": binding bytes=" << Binding.Data_.size()
             << " field store size=" << RawSize << "\n";
      continue;
    }

    SmallVector<Value*, 8> GEP = {B.getInt32(0)};
    for (unsigned Index : Indices)
      GEP.push_back(B.getInt32(Index));
    Value* FieldPtr = B.CreateGEP(Alloc->getAllocatedType(), Alloc, GEP, "bound.field.gep");
    B.CreateStore(FieldValue, FieldPtr);
  }
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
  std::string TypeStr;
  llvm::raw_string_ostream TypeOS(TypeStr);
  StructTy->print(TypeOS);
  TypeOS.flush();
  EASYJIT_RT_PASS_LOG("GetStructAlloc: struct_ty=%s raw_size=%zu alloc_size=%zu little_endian=%d\n",
                      TypeStr.c_str(),
                      Struct.get().size(),
                      static_cast<size_t>(DL.getTypeAllocSize(StructTy).getFixedValue()),
                      DL.isLittleEndian() ? 1 : 0);
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

  EASYJIT_RT_PASS_LOG("GetStructAlloc: done alloc=%p\n", (void*)Alloc);
  return Alloc;
}

llvm::AllocaInst* easy::GetPartialStructAlloc(llvm::IRBuilder<> &B,
                                              llvm::DataLayout const &DL,
                                              easy::PartialStructArgument const &Struct,
                                              llvm::Type* StructTy,
                                              llvm::Value* RuntimePtr) {
  AllocaInst* Alloc = B.CreateAlloca(StructTy);
  EASYJIT_RT_PASS_LOG("GetPartialStructAlloc: struct_ty=%p runtime_ptr=%p alloc=%p fields=%zu arrays=%zu\n",
                      (void*)StructTy, (void*)RuntimePtr, (void*)Alloc,
                      Struct.getFieldBindings().size(),
                      Struct.getArrayBindings().size());
  llvm::Align StructAlign = DL.getPrefTypeAlign(StructTy);
  uint64_t StructSize = DL.getTypeAllocSize(StructTy).getFixedValue();

  Value* Dest = B.CreateBitCast(Alloc, B.getInt8PtrTy(), "partial.struct.dst");
  Value* Src = B.CreateBitCast(RuntimePtr, B.getInt8PtrTy(), "partial.struct.src");
  B.CreateMemCpy(Dest, StructAlign, Src, StructAlign, StructSize);

  easy::ApplyStructFieldBindings(B, DL, Struct.getFieldBindings(), StructTy, Alloc);
  easy::ApplyStructArrayBindings(B, DL, Struct.getArrayBindings(), StructTy, Alloc);
  return Alloc;
}

/// Discover the struct type a pointer argument points to by scanning
/// GEP / load / store instructions in the function body.
/// Returns nullptr if no struct type can be found.
llvm::Type* easy::FindPointeeStructType(llvm::Function &F, unsigned ArgIdx) {
  EASYJIT_RT_PASS_LOG("FindPointeeStructType: fn=%s arg=%u\n",
                      F.getName().str().c_str(), ArgIdx);
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
        if (isa<StructType>(SrcTy)) {
          EASYJIT_RT_PASS_LOG("FindPointeeStructType: found via gep arg=%u\n", ArgIdx);
          return SrcTy;
        }
      }
      if (auto *LI = dyn_cast<LoadInst>(U)) {
        Type *LoadedTy = LI->getType();
        if (isa<StructType>(LoadedTy)) {
          EASYJIT_RT_PASS_LOG("FindPointeeStructType: found via load arg=%u\n", ArgIdx);
          return LoadedTy;
        }
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
  EASYJIT_RT_PASS_LOG("FindPointeeElementType: fn=%s arg=%u\n",
                      F.getName().str().c_str(), ArgIdx);
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
        EASYJIT_RT_PASS_LOG("FindPointeeElementType: found via gep arg=%u\n", ArgIdx);
        return GEP->getSourceElementType();
      }
      if (auto *LI = dyn_cast<LoadInst>(U)) {
        // If the loaded type is not a pointer, it's the element type
        if (!LI->getType()->isPointerTy()) {
          EASYJIT_RT_PASS_LOG("FindPointeeElementType: found via load arg=%u\n", ArgIdx);
          return LI->getType();
        }
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
