#include "InlineParametersHelper.h"
#include <easy/runtime/BitcodeTracker.h>

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

#ifndef EASYJIT_RUNTIME_DEBUG
#define EASYJIT_RUNTIME_DEBUG 0
#endif

#if EASYJIT_RUNTIME_DEBUG
#define EASYJIT_RT_PASS_LOG(...)                                                 \
  do {                                                                           \
    std::fprintf(stderr, "[easyjit][pass] " __VA_ARGS__);                        \
    std::fflush(stderr);                                                         \
  } while (0)
#else
#define EASYJIT_RT_PASS_LOG(...) do { } while (0)
#endif

HighLevelLayout::HighLevelLayout(easy::Context const& C, llvm::Function &F) {
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

llvm::Constant* easy::LinkPointerIfPossible(llvm::Module &M, easy::PtrArgument const &Ptr, Type* PtrTy) {
  auto &BT = easy::BitcodeTracker::GetTracker();
  void* PtrValue = const_cast<void*>(Ptr.get());
  if(BT.hasGlobalMapping(PtrValue)) {
    const char* LName = std::get<0>(BT.getNameAndGlobalMapping(PtrValue));
    std::unique_ptr<Module> LM = BT.getModuleWithContext(PtrValue, M.getContext());

    if(!Linker::linkModules(M, std::move(LM), Linker::OverrideFromSrc,
                            [](Module &, const StringSet<> &){}))
    {
      GlobalValue *GV = M.getNamedValue(LName);
      if(GlobalVariable* G = dyn_cast<GlobalVariable>(GV)) {
        GV->setLinkage(llvm::Function::PrivateLinkage);
        if(GV->getType() != PtrTy) {
          return ConstantExpr::getPointerCast(GV, PtrTy);
        }
        return GV;
      }
      else if(llvm::Function* F = dyn_cast<llvm::Function>(GV)) {
        F->setLinkage(llvm::Function::PrivateLinkage);
        return F;
      }
      assert(false && "wtf");
    }
  }
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

static Constant* GetRawByteArrayPointer(Module &M,
                                        easy::StructArgument::ArrayBinding const &Binding) {
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

static bool FindFieldPathByOffset(llvm::DataLayout const &DL,
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
      if (Offset == FieldOffset && ElemTy->isPointerTy()) {
        FieldTy = ElemTy;
        return true;
      }

      if (FindFieldPathByOffset(DL, ElemTy, Offset - FieldOffset, Indices, FieldTy))
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
    if (FindFieldPathByOffset(DL, ATy->getElementType(), Offset - Index * ElemSize, Indices, FieldTy))
      return true;
    Indices.pop_back();
    return false;
  }

  return false;
}

void easy::ApplyStructArrayBindings(llvm::IRBuilder<> &B,
                                    llvm::DataLayout const &DL,
                                    easy::StructArgument const &Struct,
                                    llvm::Type* StructTy,
                                    llvm::AllocaInst* Alloc) {
  for (auto const &Binding : Struct.getArrayBindings()) {
    SmallVector<unsigned, 8> Indices;
    Type* FieldTy = nullptr;
    if (!FindFieldPathByOffset(DL, StructTy, Binding.Offset_, Indices, FieldTy)) {
      errs() << "WARNING: snapshot bind_array could not resolve field offset "
             << Binding.Offset_ << "\n";
      continue;
    }
    (void)FieldTy;

    Value* FieldPtr = Alloc;
    SmallVector<Value*, 8> GEP = {B.getInt32(0)};
    for (unsigned Index : Indices)
      GEP.push_back(B.getInt32(Index));
    FieldPtr = B.CreateGEP(Alloc->getAllocatedType(), Alloc, GEP, "bound.array.field.gep");

    Constant* BoundPtr = GetRawByteArrayPointer(*Alloc->getModule(), Binding);
    B.CreateStore(BoundPtr, FieldPtr);
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
