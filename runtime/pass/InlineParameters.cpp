#include <easy/runtime/RuntimePasses.h>
#include <easy/runtime/Utils.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Linker/Linker.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Support/raw_ostream.h>
#include <numeric>

#include "InlineParametersHelper.h"
#include "../SreDebugLog.h"

using namespace llvm;
using easy::HighLevelLayout;

char easy::InlineParameters::ID = 0;

llvm::Pass* easy::createInlineParametersPass(llvm::StringRef Name) {
  EASYJIT_SRE_LOG("[pass] createInlineParametersPass: target=%s\n",
                  Name.str().c_str());
  return new InlineParameters(Name);
}

HighLevelLayout GetNewLayout(easy::Context const &C, HighLevelLayout &HLL) {
  EASYJIT_SRE_LOG("[pass] InlineParameters:GetNewLayout begin ctx_size=%zu old_args=%zu\n",
                  C.size(), HLL.Args_.size());

  assert(C.size() == HLL.Args_.size());

  size_t NNewArgs = 0;
  for(auto const &Arg : C) {
    if(auto const *Map = Arg->as<easy::ForwardArgument>())
      NNewArgs = std::max<size_t>(NNewArgs, Map->get()+1);
    else if (auto const *Map = Arg->as<easy::PartialStructArgument>())
      NNewArgs = std::max<size_t>(NNewArgs, Map->getIndex()+1);
  }

  HighLevelLayout NewHLL(HLL);
  NewHLL.Args_.clear();
  NewHLL.Args_.resize(NNewArgs, HighLevelLayout::HighLevelArg());

  SmallSet<unsigned, 8> VisitedArgs;

  // only forwarded params are kept
  for(size_t arg = 0; arg != HLL.Args_.size(); ++arg) {
    if(auto const *Map = C.getArgumentMapping(arg).as<easy::ForwardArgument>()) {
      if(!VisitedArgs.insert(Map->get()).second)
        continue;
      NewHLL.Args_[Map->get()] = HLL.Args_[arg];
    } else if (auto const *Map = C.getArgumentMapping(arg).as<easy::PartialStructArgument>()) {
      if(!VisitedArgs.insert(Map->getIndex()).second)
        continue;
      NewHLL.Args_[Map->getIndex()] = HLL.Args_[arg];
    }
  }

  // set the param_idx once all the parameter sizes are known
  for(size_t new_arg = 0, ParamIdx = 0; new_arg != NewHLL.Args_.size(); ++new_arg) {
    NewHLL.Args_[new_arg].FirstParamIdx_ = ParamIdx;
    ParamIdx += NewHLL.Args_[new_arg].Types_.size();
    EASYJIT_SRE_LOG("[pass] InlineParameters:GetNewLayout arg=%zu first_param=%zu fields=%zu\n",
                    new_arg, NewHLL.Args_[new_arg].FirstParamIdx_,
                    NewHLL.Args_[new_arg].Types_.size());
  }
  EASYJIT_SRE_LOG("[pass] InlineParameters:GetNewLayout end new_args=%zu\n",
                  NewHLL.Args_.size());
  return NewHLL;
}

FunctionType* GetWrapperTy(HighLevelLayout &HLL) {
  SmallVector<Type*, 8> Args;
  if(HLL.StructReturn_)
    Args.push_back(HLL.StructReturn_);
  for(auto &HLArg : HLL.Args_)
    Args.insert(Args.end(), HLArg.Types_.begin(), HLArg.Types_.end());
  EASYJIT_SRE_LOG("[pass] InlineParameters:GetWrapperTy args=%zu has_sret=%d\n",
                  Args.size(), HLL.StructReturn_ ? 1 : 0);
  return FunctionType::get(HLL.Return_, Args, false);
}

void GetInlineArgs(easy::Context const &C,
                   Function& F, HighLevelLayout &FHLL,
                   Function &Wrapper, HighLevelLayout &WrapperHLL,
                   SmallVectorImpl<Value*> &Args, IRBuilder<> &B) {

  LLVMContext &Ctx = F.getContext();
  DataLayout const &DL = F.getParent()->getDataLayout();

  if(FHLL.StructReturn_)
    Args.push_back(&*Wrapper.arg_begin());

  for(size_t i = 0, n = C.size(); i != n; ++i) {
    auto const &Arg = C.getArgumentMapping(i);
    auto &ArgInF = FHLL.Args_[i];
    EASYJIT_SRE_LOG("[pass] InlineParameters:GetInlineArgs arg=%zu kind=%d first_param=%zu types=%zu\n",
                    i, (int)Arg.kind(), ArgInF.FirstParamIdx_,
                    ArgInF.Types_.size());

    switch(Arg.kind()) {

      case easy::ArgumentBase::AK_Forward: {
        auto Forward = GetForwardArgs(ArgInF, FHLL, Wrapper, WrapperHLL);
        Args.insert(Args.end(), Forward.begin(), Forward.end());
        EASYJIT_SRE_LOG("[pass] InlineParameters:GetInlineArgs arg=%zu forward values=%zu\n",
                        i, Forward.size());
      } break;
      case easy::ArgumentBase::AK_Int:
      case easy::ArgumentBase::AK_Float: {
        Args.push_back(easy::GetScalarArgument(Arg, ArgInF.Types_[0]));
        EASYJIT_SRE_LOG("[pass] InlineParameters:GetInlineArgs arg=%zu scalar constant\n", i);
      } break;

      case easy::ArgumentBase::AK_Ptr: {
        auto const *Ptr = Arg.as<easy::PtrArgument>();
        Type* PtrTy = FHLL.Args_[i].Types_[0];

        Constant* PtrVal = easy::GetScalarArgument(Arg, PtrTy);
        bool Linked = false;
        if(Constant* LinkedPtr = easy::LinkPointerIfPossible(*Wrapper.getParent(), *Ptr, PtrTy)) {
          PtrVal = LinkedPtr;
          Linked = true;
        }

        Args.push_back(PtrVal);
        EASYJIT_SRE_LOG("[pass] InlineParameters:GetInlineArgs arg=%zu pointer ptr=%p linked=%d\n",
                        i, Ptr->get(), (int)Linked);
      } break;

      case easy::ArgumentBase::AK_Array: {
        auto const *Array = Arg.as<easy::ArrayArgument>();
        Type* PtrTy = FHLL.Args_[i].Types_[0];
        // With opaque pointers, discover element type from function body
        unsigned ParamIdx = ArgInF.FirstParamIdx_ + (FHLL.StructReturn_ ? 1 : 0);
        Type* PointeeTy = easy::FindPointeeElementType(F, ParamIdx);
        assert(PointeeTy && "Cannot discover array element type from function body");
        Constant* ArrayConst = easy::GetArrayConstant(DL, *Array, PointeeTy);
        auto *GV = new GlobalVariable(*Wrapper.getParent(), ArrayConst->getType(), true,
                                      GlobalValue::PrivateLinkage, ArrayConst,
                                      "__easy_snapshot_array");
        Constant* Zero = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
        Constant* Indices[] = {Zero, Zero};
        Constant* PtrVal = ConstantExpr::getInBoundsGetElementPtr(ArrayConst->getType(), GV, Indices);
        Args.push_back(PtrVal);
        EASYJIT_SRE_LOG("[pass] InlineParameters:GetInlineArgs arg=%zu array count=%zu elem=%zu\n",
                        i, Array->getCount(), Array->getElementSize());
      } break;

      case easy::ArgumentBase::AK_Struct: {
        auto const *Struct = Arg.as<easy::StructArgument>();
        auto &ArgInF = FHLL.Args_[i];

        if(ArgInF.StructByPointer_) {
          // For reference/pointer carriers, infer the pointee struct from the
          // function body instead of relying on pointer element types.
          unsigned ParamIdx = ArgInF.FirstParamIdx_ + (FHLL.StructReturn_ ? 1 : 0);
          Type* StructType = easy::FindPointeeStructType(F, ParamIdx);
          assert(StructType && "Cannot discover struct type for pointer/reference parameter");
          AllocaInst* ParamAlloc = easy::GetStructAlloc(B, DL, *Struct, StructType);
          easy::ApplyStructArrayBindings(B, DL, Struct->getArrayBindings(), StructType, ParamAlloc);
          Args.push_back(ParamAlloc);
          EASYJIT_SRE_LOG("[pass] InlineParameters:GetInlineArgs arg=%zu struct by pointer alloc=%p\n",
                          i, (void*)ParamAlloc);
        } else if (ArgInF.StructByArray_) {
          // struct is passed as an array
          Type* ArrayTy = ArgInF.Types_[0];
          size_t N = ArrayTy->getArrayNumElements();
          Type* FieldTy = ArrayTy->getArrayElementType();
          SmallVector<Constant*, 8> ArrayValues;

          for(size_t ParamIdx = 0, RawOffset = 0; ParamIdx != N; ++ParamIdx) {
            const char* RawField = &Struct->get()[RawOffset];

            Constant* FieldValue;
            size_t RawSize;
            std::tie(FieldValue, RawSize) = easy::GetConstantFromRaw(DL, FieldTy, (uint8_t const*)RawField);

            ArrayValues.push_back(FieldValue);
            RawOffset += RawSize;
          }

          Constant* ArrayConst = ConstantArray::get(cast<ArrayType>(ArrayTy), ArrayValues);
          Args.push_back(ArrayConst);
          EASYJIT_SRE_LOG("[pass] InlineParameters:GetInlineArgs arg=%zu struct by array fields=%zu\n",
                          i, N);
        } else {
          // struct is passed by value (may be many values)
          size_t N = ArgInF.Types_.size();
          for(size_t ParamIdx = 0, RawOffset = 0; ParamIdx != N; ++ParamIdx) {
            Type* FieldTy = ArgInF.Types_[ParamIdx];
            const char* RawField = &Struct->get()[RawOffset];

            Constant* FieldValue;
            size_t RawSize;
            std::tie(FieldValue, RawSize) = easy::GetConstantFromRaw(DL, FieldTy, (uint8_t const*)RawField);

            Args.push_back(FieldValue);
            RawOffset += RawSize;
            EASYJIT_SRE_LOG("[pass] InlineParameters:GetInlineArgs arg=%zu struct field=%zu raw_size=%zu\n",
                            i, ParamIdx, RawSize);
          }
        }
      } break;

      case easy::ArgumentBase::AK_PartialStruct: {
        auto const *Struct = Arg.as<easy::PartialStructArgument>();
        auto Forward = GetForwardArgs(ArgInF, FHLL, Wrapper, WrapperHLL);
        assert(Forward.size() == 1 && "partial struct arguments must map to one wrapper argument");
        if (!ArgInF.StructByPointer_) {
          llvm::report_fatal_error("partial struct binding requires a pointer/reference parameter carrier",
                                   true);
        }

        unsigned ParamIdx = ArgInF.FirstParamIdx_ + (FHLL.StructReturn_ ? 1 : 0);
        Type* StructType = easy::FindPointeeStructType(F, ParamIdx);
        assert(StructType && "Cannot discover struct type for partial-struct parameter");
        AllocaInst* ParamAlloc = easy::GetPartialStructAlloc(B, DL, *Struct, StructType, Forward[0]);
        Args.push_back(ParamAlloc);
        EASYJIT_SRE_LOG("[pass] InlineParameters:GetInlineArgs arg=%zu partial struct alloc=%p\n",
                        i, (void*)ParamAlloc);
      } break;

      case easy::ArgumentBase::AK_Module: {

        auto &ArgInF = FHLL.Args_[i];
        assert(ArgInF.Types_.size() == 1);

        easy::Function const &Function = Arg.as<easy::ModuleArgument>()->get();
        llvm::Module const& FunctionModule = Function.getLLVMModule();
        auto FunctionName = easy::GetEntryFunctionName(FunctionModule);

        std::unique_ptr<llvm::Module> LM =
            easy::CloneModuleWithContext(FunctionModule, Wrapper.getContext());

        assert(LM);

        easy::UnmarkEntry(*LM);

        llvm::Module* M = Wrapper.getParent();
        if(Linker::linkModules(*M, std::move(LM), Linker::OverrideFromSrc,
                                [](Module &, const StringSet<> &){})) {
          llvm::report_fatal_error("Failed to link with another module!", true);
        }

        llvm::Function* FunctionInM = M->getFunction(FunctionName);
        FunctionInM->setLinkage(Function::PrivateLinkage);

        Args.push_back(FunctionInM);
        EASYJIT_SRE_LOG("[pass] InlineParameters:GetInlineArgs arg=%zu module function=%s\n",
                        i, FunctionName.str().c_str());

      } break;
    }
  }
}

void RemapAttributes(Function const &F, HighLevelLayout const& HLL, Function &Wrapper, HighLevelLayout const& NewHLL) {
  auto FAttributes = F.getAttributes();

  auto FunAttrs = FAttributes.getFnAttrs();
  for(Attribute Attr : FunAttrs) {
    if(Attr.getKindAsEnum() == Attribute::OptimizeNone)
      continue;
    Wrapper.addFnAttr(Attr);
  }

  if(F.hasFnAttribute(Attribute::OptimizeNone))
    Wrapper.addFnAttr(Attribute::NoInline);

  for(size_t new_arg = 0; new_arg != NewHLL.Args_.size(); ++new_arg) {
    auto const &NewArg = NewHLL.Args_[new_arg];
    auto const &OrgArg = HLL.Args_[NewArg.Position_];

    for(size_t field = 0; field != NewArg.Types_.size(); ++field) {
      Wrapper.addParamAttrs(field + NewArg.FirstParamIdx_,
                             AttrBuilder(F.getContext(), FAttributes.getParamAttrs(field + OrgArg.FirstParamIdx_)));
    }
  }
}

Function* CreateWrapperFun(Module &M, Function &F, HighLevelLayout &HLL, easy::Context const &C) {
  EASYJIT_SRE_LOG("[pass] InlineParameters:CreateWrapperFun begin fn=%s module=%p\n",
                  F.getName().str().c_str(), (void*)&M);
  LLVMContext &CC = M.getContext();

  HighLevelLayout NewHLL(GetNewLayout(C, HLL));
  FunctionType *WrapperTy = GetWrapperTy(NewHLL);

  Function* Wrapper = Function::Create(WrapperTy, Function::ExternalLinkage, "", &M);
  EASYJIT_SRE_LOG("[pass] InlineParameters:CreateWrapperFun wrapper=%p args=%zu\n",
                  (void*)Wrapper, Wrapper->arg_size());

  BasicBlock* BB = BasicBlock::Create(CC, "", Wrapper);
  IRBuilder<> B(BB);

  SmallVector<Value*, 8> Args;
  GetInlineArgs(C, F, HLL, *Wrapper, NewHLL, Args, B);

  Value* Call = B.CreateCall(&F, Args);
  EASYJIT_SRE_LOG("[pass] InlineParameters:CreateWrapperFun call args=%zu ret_void=%d\n",
                  Args.size(), Call->getType()->isVoidTy() ? 1 : 0);

  if(HLL.StructReturn_) {
    Wrapper->arg_begin()->addAttr(Attribute::StructRet);
  }

  if(Call->getType()->isVoidTy()) {
    B.CreateRetVoid();
  } else {
    B.CreateRet(Call);
  }

  RemapAttributes(F, HLL, *Wrapper, NewHLL);

  return Wrapper;
}

bool easy::InlineParameters::runOnModule(llvm::Module &M) {

  easy::Context const &C = getAnalysis<ContextAnalysis>().getContext();
  EASYJIT_SRE_LOG("[pass] InlineParameters::runOnModule begin target=%s module=%p ctx_size=%zu\n",
                  TargetName_.str().c_str(), (void*)&M, C.size());
  llvm::Function* F = M.getFunction(TargetName_);
  assert(F);
  EASYJIT_SRE_LOG("[pass] InlineParameters::runOnModule found fn=%p args=%zu\n",
                  (void*)F, F->arg_size());

  bool GlobalChanged = easy::ApplyGlobalStructSnapshots(M, TargetName_, C);
  EASYJIT_SRE_LOG("[pass] InlineParameters::runOnModule global snapshots changed=%d\n",
                  (int)GlobalChanged);

  HighLevelLayout HLL(C, *F);
  llvm::Function* WrapperFun = CreateWrapperFun(M, *F, HLL, C);

  // privatize F, steal its name, copy its attributes, and its cc
  F->setLinkage(llvm::Function::PrivateLinkage);
  WrapperFun->takeName(F);
  WrapperFun->setCallingConv(CallingConv::C);

  // add metadata to identify the entry function
  easy::MarkAsEntry(*WrapperFun);

  EASYJIT_SRE_LOG("[pass] InlineParameters::runOnModule end wrapper=%p name=%s\n",
                  (void*)WrapperFun, WrapperFun->getName().str().c_str());

  return true;
}

static RegisterPass<easy::InlineParameters> X("","",false, false);
