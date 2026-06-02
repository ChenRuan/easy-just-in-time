#ifndef EASYJIT_LIGHT_BACKEND_ONLY
#define EASYJIT_LIGHT_BACKEND_ONLY 0
#endif

#include <easy/runtime/BitcodeTracker.h>
#include <easy/runtime/Function.h>
#include <easy/runtime/RuntimePasses.h>
#include <easy/runtime/LLVMHolderImpl.h>
#include "SreDebugLog.h"
#if !EASYJIT_LIGHT_BACKEND_ONLY
#include <easy/runtime/MinimalOrcJIT.h>
#endif
#include <easy/runtime/Utils.h>
#include <easy/exceptions.h>

#if EASYJIT_LIGHT_BACKEND_ENABLED
#include "LightBackend.h"
#endif

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils.h>
#if !EASYJIT_LIGHT_BACKEND_ONLY
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#endif
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>
#if !EASYJIT_LIGHT_BACKEND_ONLY
#include <llvm/Target/TargetMachine.h>
#include <llvm/MC/TargetRegistry.h>
#endif
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/ADT/SmallString.h>
#include <cstdio>
#include <cstdlib>

#define EASYJIT_RT_LOG(...) EASYJIT_SRE_LOG("[runtime] " __VA_ARGS__)
#define EASYJIT_RT_RAW(...)                                                    \
  do {                                                                         \
    if (SRE_printf) {                                                          \
      SRE_printf(__VA_ARGS__);                                                 \
    } else {                                                                   \
      std::fprintf(stderr, __VA_ARGS__);                                       \
      std::fflush(stderr);                                                     \
    }                                                                          \
  } while (0)


using namespace easy;

extern void* __dso_handle;

namespace easy {
  DefineEasyException(JITCreateError, "Failed to create ORC JIT for:");
  DefineEasyException(CouldNotOpenFile, "Failed to file to dump intermediate representation.");
  DefineEasyException(TargetMachineCreateError, "Failed to create target machine for:");
  DefineEasyException(TargetLookupError, "Failed to lookup target for:");
  DefineEasyException(SymbolLookupError, "Failed to lookup JIT symbol for:");
  // Light-only / LightBackend specific failure. Distinct from JITCreateError
  // (which is reserved for the ORC fallback path) so users / log scrapers
  // can tell the two apart and we never claim to have tried ORC when the
  // runtime was compiled without it.
  DefineEasyException(LightBackendCompileError,
                      "Light backend cannot compile function: ");
}

Function::Function(void* Addr, std::unique_ptr<LLVMHolder> H)
  : Address(Addr), Holder(std::move(H)) {
}

namespace {
class OriginalFunctionHolder : public easy::LLVMHolder {
public:
  explicit OriginalFunctionHolder(std::string Reason)
      : Reason_(std::move(Reason)) {}

  llvm::Module* getModule() const override { return nullptr; }

  std::string const& reason() const { return Reason_; }

private:
  std::string Reason_;
};

static void LogNameRef(const char *Label, llvm::StringRef Name) {
  EASYJIT_RT_LOG("%s%.*s\n", Label, (int)Name.size(), Name.data());
}

static const char *TypeKindName(llvm::Type *T) {
  if (!T)
    return "null";
  if (T->isVoidTy())
    return "void";
  if (T->isIntegerTy())
    return "int";
  if (T->isPointerTy())
    return "ptr";
  if (T->isFloatTy())
    return "float";
  if (T->isDoubleTy())
    return "double";
  if (T->isFunctionTy())
    return "func";
  if (T->isStructTy())
    return "struct";
  if (T->isArrayTy())
    return "array";
  if (T->isVectorTy())
    return "vector";
  if (T->isLabelTy())
    return "label";
  if (T->isMetadataTy())
    return "metadata";
  return "other";
}

static unsigned TypeBitHint(llvm::Type *T) {
  if (!T)
    return 0;
  if (auto *IT = llvm::dyn_cast<llvm::IntegerType>(T))
    return IT->getBitWidth();
  if (T->isFloatTy())
    return 32;
  if (T->isDoubleTy())
    return 64;
  if (auto *PT = llvm::dyn_cast<llvm::PointerType>(T))
    return PT->getAddressSpace();
  return 0;
}

static void LogTypeBrief(const char *Label, llvm::Type *T) {
  EASYJIT_RT_LOG("%s type=%p kind=%s hint=%u\n", Label, (void *)T,
                 TypeKindName(T), TypeBitHint(T));
}

static void LogValueBrief(const char *Label, llvm::Value *V) {
  if (!V) {
    EASYJIT_RT_LOG("%s value=<null>\n", Label);
    return;
  }
  llvm::Type *T = V->getType();
  EASYJIT_RT_LOG("%s value=%p type=%p kind=%s hint=%u\n", Label, (void *)V,
                 (void *)T, TypeKindName(T), TypeBitHint(T));
}

static bool IsInterestingGlobalName(llvm::StringRef N) {
  return N == "llvm.global_ctors" || N == "llvm.global_dtors" ||
         N.startswith("llvm.") || N == "stderr" || N == "__dso_handle";
}

static const char *ValueClassName(llvm::Value *V) {
  if (!V)
    return "null";
  if (llvm::isa<llvm::Argument>(V))
    return "arg";
  if (llvm::isa<llvm::BasicBlock>(V))
    return "bb";
  if (llvm::isa<llvm::Function>(V))
    return "func";
  if (llvm::isa<llvm::GlobalVariable>(V))
    return "global";
  if (llvm::isa<llvm::Instruction>(V))
    return "inst";
  if (llvm::isa<llvm::ConstantInt>(V))
    return "const-int";
  if (llvm::isa<llvm::ConstantFP>(V))
    return "const-fp";
  if (llvm::isa<llvm::ConstantPointerNull>(V))
    return "const-null";
  if (llvm::isa<llvm::UndefValue>(V))
    return "undef";
  if (llvm::isa<llvm::ConstantExpr>(V))
    return "const-expr";
  if (llvm::isa<llvm::Constant>(V))
    return "const";
  return "value";
}

static void LogPseudoValue(const char *Prefix, llvm::Value *V) {
  if (!V) {
    EASYJIT_RT_LOG("%s value=<null>\n", Prefix);
    return;
  }

  llvm::StringRef N;
  if (V->hasName())
    N = V->getName();

  EASYJIT_RT_LOG("%s value=%p class=%s type_kind=%s type_hint=%u name=%.*s\n",
                 Prefix, (void *)V, ValueClassName(V),
                 TypeKindName(V->getType()), TypeBitHint(V->getType()),
                 (int)N.size(), N.data());

  if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(V)) {
    unsigned Bits = CI->getBitWidth();
    if (Bits <= 64)
      EASYJIT_RT_LOG("%s const_int bits=%u zext=%llu sext=%lld\n", Prefix,
                     Bits, (unsigned long long)CI->getZExtValue(),
                     (long long)CI->getSExtValue());
    else
      EASYJIT_RT_LOG("%s const_int bits=%u wide=1\n", Prefix, Bits);
  } else if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(V)) {
    EASYJIT_RT_LOG("%s const_expr opcode=%u ops=%u\n", Prefix,
                   CE->getOpcode(), CE->getNumOperands());
  } else if (auto *BB = llvm::dyn_cast<llvm::BasicBlock>(V)) {
    llvm::StringRef BBN = BB->hasName() ? BB->getName() : llvm::StringRef();
    EASYJIT_RT_LOG("%s bb=%p bb_name=%.*s insts=%zu\n", Prefix, (void *)BB,
                   (int)BBN.size(), BBN.data(), (size_t)BB->size());
  }
}

static void LogPseudoLlValue(const char *Tag, llvm::Value *V) {
  if (!V) {
    EASYJIT_RT_LOG("PSEUDO_LL_%s: value=<null>\n", Tag);
    return;
  }

  llvm::StringRef N = V->hasName() ? V->getName() : llvm::StringRef();
  EASYJIT_RT_LOG("PSEUDO_LL_%s: ref=%p class=%s type=%s hint=%u name=%.*s\n",
                 Tag, (void *)V, ValueClassName(V),
                 TypeKindName(V->getType()), TypeBitHint(V->getType()),
                 (int)N.size(), N.data());
  if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(V)) {
    unsigned Bits = CI->getBitWidth();
    if (Bits <= 64)
      EASYJIT_RT_LOG("PSEUDO_LL_%s: const_int bits=%u zext=%llu sext=%lld\n",
                     Tag, Bits, (unsigned long long)CI->getZExtValue(),
                     (long long)CI->getSExtValue());
    else
      EASYJIT_RT_LOG("PSEUDO_LL_%s: const_int bits=%u wide=1\n", Tag, Bits);
  }
}

static const char *PredicateName(llvm::CmpInst::Predicate P) {
  switch (P) {
  case llvm::CmpInst::ICMP_EQ: return "eq";
  case llvm::CmpInst::ICMP_NE: return "ne";
  case llvm::CmpInst::ICMP_UGT: return "ugt";
  case llvm::CmpInst::ICMP_UGE: return "uge";
  case llvm::CmpInst::ICMP_ULT: return "ult";
  case llvm::CmpInst::ICMP_ULE: return "ule";
  case llvm::CmpInst::ICMP_SGT: return "sgt";
  case llvm::CmpInst::ICMP_SGE: return "sge";
  case llvm::CmpInst::ICMP_SLT: return "slt";
  case llvm::CmpInst::ICMP_SLE: return "sle";
  case llvm::CmpInst::FCMP_FALSE: return "false";
  case llvm::CmpInst::FCMP_OEQ: return "oeq";
  case llvm::CmpInst::FCMP_OGT: return "ogt";
  case llvm::CmpInst::FCMP_OGE: return "oge";
  case llvm::CmpInst::FCMP_OLT: return "olt";
  case llvm::CmpInst::FCMP_OLE: return "ole";
  case llvm::CmpInst::FCMP_ONE: return "one";
  case llvm::CmpInst::FCMP_ORD: return "ord";
  case llvm::CmpInst::FCMP_UNO: return "uno";
  case llvm::CmpInst::FCMP_UEQ: return "ueq";
  case llvm::CmpInst::FCMP_UGT: return "ugt";
  case llvm::CmpInst::FCMP_UGE: return "uge";
  case llvm::CmpInst::FCMP_ULT: return "ult";
  case llvm::CmpInst::FCMP_ULE: return "ule";
  case llvm::CmpInst::FCMP_UNE: return "une";
  case llvm::CmpInst::FCMP_TRUE: return "true";
  default: return "pred";
  }
}

static void PrintTypeFrag(llvm::Type *T, unsigned Depth = 0) {
  if (!T) {
    EASYJIT_RT_RAW("<nullty>");
    return;
  }
  if (Depth > 2) {
    EASYJIT_RT_RAW("<deep>");
    return;
  }
  if (T->isVoidTy()) {
    EASYJIT_RT_RAW("void");
  } else if (auto *IT = llvm::dyn_cast<llvm::IntegerType>(T)) {
    EASYJIT_RT_RAW("i%u", IT->getBitWidth());
  } else if (T->isPointerTy()) {
    EASYJIT_RT_RAW("ptr");
  } else if (T->isFloatTy()) {
    EASYJIT_RT_RAW("float");
  } else if (T->isDoubleTy()) {
    EASYJIT_RT_RAW("double");
  } else if (auto *AT = llvm::dyn_cast<llvm::ArrayType>(T)) {
    EASYJIT_RT_RAW("[%llu x ", (unsigned long long)AT->getNumElements());
    PrintTypeFrag(AT->getElementType(), Depth + 1);
    EASYJIT_RT_RAW("]");
  } else if (auto *ST = llvm::dyn_cast<llvm::StructType>(T)) {
    if (ST->hasName()) {
      llvm::StringRef N = ST->getName();
      EASYJIT_RT_RAW("%%\"%.*s\"", (int)N.size(), N.data());
    } else {
      EASYJIT_RT_RAW("struct");
    }
  } else if (T->isFunctionTy()) {
    EASYJIT_RT_RAW("fn");
  } else if (T->isVectorTy()) {
    EASYJIT_RT_RAW("vector");
  } else if (T->isLabelTy()) {
    EASYJIT_RT_RAW("label");
  } else if (T->isMetadataTy()) {
    EASYJIT_RT_RAW("metadata");
  } else {
    EASYJIT_RT_RAW("type");
  }
}

static void PrintValueFrag(llvm::Value *V) {
  if (!V) {
    EASYJIT_RT_RAW("<null>");
    return;
  }
  if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(V)) {
    if (CI->getBitWidth() == 1) {
      EASYJIT_RT_RAW("%s", CI->isOne() ? "true" : "false");
    } else if (CI->getBitWidth() <= 64) {
      if (CI->getValue().isNegative())
        EASYJIT_RT_RAW("%lld", (long long)CI->getSExtValue());
      else
        EASYJIT_RT_RAW("%llu", (unsigned long long)CI->getZExtValue());
    } else {
      EASYJIT_RT_RAW("constint%p", (void *)CI);
    }
  } else if (llvm::isa<llvm::ConstantPointerNull>(V)) {
    EASYJIT_RT_RAW("null");
  } else if (llvm::isa<llvm::UndefValue>(V)) {
    EASYJIT_RT_RAW("undef");
  } else if (auto *BB = llvm::dyn_cast<llvm::BasicBlock>(V)) {
    llvm::StringRef N = BB->hasName() ? BB->getName() : llvm::StringRef();
    if (!N.empty())
      EASYJIT_RT_RAW("%%%.*s", (int)N.size(), N.data());
    else
      EASYJIT_RT_RAW("%%bb%p", (void *)BB);
  } else if (auto *F = llvm::dyn_cast<llvm::Function>(V)) {
    llvm::StringRef N = F->hasName() ? F->getName() : llvm::StringRef();
    if (!N.empty())
      EASYJIT_RT_RAW("@%.*s", (int)N.size(), N.data());
    else
      EASYJIT_RT_RAW("@fn%p", (void *)F);
  } else if (auto *GV = llvm::dyn_cast<llvm::GlobalValue>(V)) {
    llvm::StringRef N = GV->hasName() ? GV->getName() : llvm::StringRef();
    if (!N.empty())
      EASYJIT_RT_RAW("@%.*s", (int)N.size(), N.data());
    else
      EASYJIT_RT_RAW("@g%p", (void *)GV);
  } else if (llvm::isa<llvm::ConstantFP>(V)) {
    EASYJIT_RT_RAW("constfp%p", (void *)V);
  } else if (llvm::isa<llvm::ConstantExpr>(V)) {
    EASYJIT_RT_RAW("constexpr%p", (void *)V);
  } else if (llvm::isa<llvm::Constant>(V)) {
    EASYJIT_RT_RAW("const%p", (void *)V);
  } else {
    EASYJIT_RT_RAW("%%v%p", (void *)V);
  }
}

static void PrintTypedValueFrag(llvm::Value *V) {
  if (!V) {
    EASYJIT_RT_RAW("<nullty> <null>");
    return;
  }
  PrintTypeFrag(V->getType());
  EASYJIT_RT_RAW(" ");
  PrintValueFrag(V);
}

static void PrintLlTextInst(llvm::Instruction &I) {
  EASYJIT_RT_RAW("[easyjit][sre] [runtime] PSEUDO_LL_TEXT:   ");
  if (!I.getType()->isVoidTy()) {
    PrintValueFrag(&I);
    EASYJIT_RT_RAW(" = ");
  }

  if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
    EASYJIT_RT_RAW("%s ", BO->getOpcodeName());
    PrintTypeFrag(BO->getType());
    EASYJIT_RT_RAW(" ");
    PrintValueFrag(BO->getOperand(0));
    EASYJIT_RT_RAW(", ");
    PrintValueFrag(BO->getOperand(1));
  } else if (auto *CI = llvm::dyn_cast<llvm::CmpInst>(&I)) {
    EASYJIT_RT_RAW("%s %s ", CI->getOpcodeName(),
                   PredicateName(CI->getPredicate()));
    PrintTypeFrag(CI->getOperand(0)->getType());
    EASYJIT_RT_RAW(" ");
    PrintValueFrag(CI->getOperand(0));
    EASYJIT_RT_RAW(", ");
    PrintValueFrag(CI->getOperand(1));
  } else if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
    EASYJIT_RT_RAW("load ");
    PrintTypeFrag(LI->getType());
    EASYJIT_RT_RAW(", ");
    PrintTypedValueFrag(LI->getPointerOperand());
  } else if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
    EASYJIT_RT_RAW("store ");
    PrintTypedValueFrag(SI->getValueOperand());
    EASYJIT_RT_RAW(", ");
    PrintTypedValueFrag(SI->getPointerOperand());
  } else if (auto *RI = llvm::dyn_cast<llvm::ReturnInst>(&I)) {
    if (llvm::Value *RV = RI->getReturnValue()) {
      EASYJIT_RT_RAW("ret ");
      PrintTypedValueFrag(RV);
    } else {
      EASYJIT_RT_RAW("ret void");
    }
  } else if (auto *BR = llvm::dyn_cast<llvm::BranchInst>(&I)) {
    if (BR->isConditional()) {
      EASYJIT_RT_RAW("br ");
      PrintTypedValueFrag(BR->getCondition());
      EASYJIT_RT_RAW(", label ");
      PrintValueFrag(BR->getSuccessor(0));
      EASYJIT_RT_RAW(", label ");
      PrintValueFrag(BR->getSuccessor(1));
    } else {
      EASYJIT_RT_RAW("br label ");
      PrintValueFrag(BR->getSuccessor(0));
    }
  } else if (auto *PN = llvm::dyn_cast<llvm::PHINode>(&I)) {
    EASYJIT_RT_RAW("phi ");
    PrintTypeFrag(PN->getType());
    for (unsigned P = 0, E = PN->getNumIncomingValues(); P != E; ++P) {
      EASYJIT_RT_RAW("%s[ ", P ? ", " : " ");
      PrintValueFrag(PN->getIncomingValue(P));
      EASYJIT_RT_RAW(", ");
      PrintValueFrag(PN->getIncomingBlock(P));
      EASYJIT_RT_RAW(" ]");
    }
  } else if (auto *CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
    EASYJIT_RT_RAW("call ");
    PrintTypeFrag(CB->getType());
    EASYJIT_RT_RAW(" ");
    if (llvm::Function *CF = CB->getCalledFunction())
      PrintValueFrag(CF);
    else
      PrintValueFrag(CB->getCalledOperand());
    EASYJIT_RT_RAW("(");
    for (unsigned A = 0, E = CB->arg_size(); A != E; ++A) {
      if (A)
        EASYJIT_RT_RAW(", ");
      PrintTypedValueFrag(CB->getArgOperand(A));
    }
    EASYJIT_RT_RAW(")");
  } else if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
    EASYJIT_RT_RAW("getelementptr ");
    PrintTypeFrag(GEP->getSourceElementType());
    EASYJIT_RT_RAW(", ");
    PrintTypedValueFrag(GEP->getPointerOperand());
    for (auto Idx = GEP->idx_begin(), End = GEP->idx_end(); Idx != End; ++Idx) {
      EASYJIT_RT_RAW(", ");
      PrintTypedValueFrag(*Idx);
    }
  } else if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
    EASYJIT_RT_RAW("alloca ");
    PrintTypeFrag(AI->getAllocatedType());
  } else if (auto *Sel = llvm::dyn_cast<llvm::SelectInst>(&I)) {
    EASYJIT_RT_RAW("select ");
    PrintTypedValueFrag(Sel->getCondition());
    EASYJIT_RT_RAW(", ");
    PrintTypedValueFrag(Sel->getTrueValue());
    EASYJIT_RT_RAW(", ");
    PrintTypedValueFrag(Sel->getFalseValue());
  } else if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(&I)) {
    EASYJIT_RT_RAW("%s ", Cast->getOpcodeName());
    PrintTypedValueFrag(Cast->getOperand(0));
    EASYJIT_RT_RAW(" to ");
    PrintTypeFrag(Cast->getType());
  } else {
    EASYJIT_RT_RAW("%s", I.getOpcodeName());
    for (unsigned Op = 0, E = I.getNumOperands(); Op != E; ++Op) {
      EASYJIT_RT_RAW("%s", Op ? ", " : " ");
      PrintTypedValueFrag(I.getOperand(Op));
    }
  }
  EASYJIT_RT_RAW("\n");
}

static void LogPseudoLlInst(llvm::Instruction &I, unsigned BBIndex,
                            unsigned InstIndex) {
  llvm::StringRef IN = I.hasName() ? I.getName() : llvm::StringRef();
  EASYJIT_RT_LOG("PSEUDO_LL_INST: bb=%u inst=%u result=%p result_name=%.*s opcode=%s result_type=%s result_hint=%u ops=%u\n",
                 BBIndex, InstIndex, (void *)&I, (int)IN.size(), IN.data(),
                 I.getOpcodeName(), TypeKindName(I.getType()),
                 TypeBitHint(I.getType()), I.getNumOperands());
  PrintLlTextInst(I);

  if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
    EASYJIT_RT_LOG("PSEUDO_LL_BINOP: result=%p op=%s lhs=%p rhs=%p ty=%s hint=%u\n",
                   (void *)BO, BO->getOpcodeName(), (void *)BO->getOperand(0),
                   (void *)BO->getOperand(1), TypeKindName(BO->getType()),
                   TypeBitHint(BO->getType()));
  } else if (auto *CI = llvm::dyn_cast<llvm::CmpInst>(&I)) {
    EASYJIT_RT_LOG("PSEUDO_LL_CMP: result=%p op=%s pred=%s lhs=%p rhs=%p operand_ty=%s operand_hint=%u\n",
                   (void *)CI, CI->getOpcodeName(),
                   CI->getPredicateName(CI->getPredicate()).data(),
                   (void *)CI->getOperand(0), (void *)CI->getOperand(1),
                   TypeKindName(CI->getOperand(0)->getType()),
                   TypeBitHint(CI->getOperand(0)->getType()));
  } else if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
    EASYJIT_RT_LOG("PSEUDO_LL_LOAD: result=%p value_ty=%s value_hint=%u ptr=%p ptr_ty=%s ptr_hint=%u volatile=%d align=%u\n",
                   (void *)LI, TypeKindName(LI->getType()),
                   TypeBitHint(LI->getType()),
                   (void *)LI->getPointerOperand(),
                   TypeKindName(LI->getPointerOperand()->getType()),
                   TypeBitHint(LI->getPointerOperand()->getType()),
                   (int)LI->isVolatile(), (unsigned)LI->getAlign().value());
  } else if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
    EASYJIT_RT_LOG("PSEUDO_LL_STORE: val=%p val_ty=%s val_hint=%u ptr=%p ptr_ty=%s ptr_hint=%u volatile=%d align=%u\n",
                   (void *)SI->getValueOperand(),
                   TypeKindName(SI->getValueOperand()->getType()),
                   TypeBitHint(SI->getValueOperand()->getType()),
                   (void *)SI->getPointerOperand(),
                   TypeKindName(SI->getPointerOperand()->getType()),
                   TypeBitHint(SI->getPointerOperand()->getType()),
                   (int)SI->isVolatile(), (unsigned)SI->getAlign().value());
  } else if (auto *RI = llvm::dyn_cast<llvm::ReturnInst>(&I)) {
    EASYJIT_RT_LOG("PSEUDO_LL_RET: ret=%p\n", (void *)RI->getReturnValue());
  } else if (auto *BR = llvm::dyn_cast<llvm::BranchInst>(&I)) {
    EASYJIT_RT_LOG("PSEUDO_LL_BR: conditional=%d cond=%p succ0=%p succ1=%p\n",
                   (int)BR->isConditional(),
                   BR->isConditional() ? (void *)BR->getCondition() : nullptr,
                   BR->getNumSuccessors() > 0 ? (void *)BR->getSuccessor(0) : nullptr,
                   BR->getNumSuccessors() > 1 ? (void *)BR->getSuccessor(1) : nullptr);
  } else if (auto *PN = llvm::dyn_cast<llvm::PHINode>(&I)) {
    EASYJIT_RT_LOG("PSEUDO_LL_PHI: result=%p ty=%s hint=%u incoming=%u\n",
                   (void *)PN, TypeKindName(PN->getType()),
                   TypeBitHint(PN->getType()), PN->getNumIncomingValues());
    for (unsigned P = 0, E = PN->getNumIncomingValues(); P != E; ++P)
      EASYJIT_RT_LOG("PSEUDO_LL_PHI_IN: result=%p idx=%u value=%p block=%p\n",
                     (void *)PN, P, (void *)PN->getIncomingValue(P),
                     (void *)PN->getIncomingBlock(P));
  } else if (auto *CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
    llvm::Function *CalledFn = CB->getCalledFunction();
    llvm::StringRef CN =
        CalledFn ? CalledFn->getName() : llvm::StringRef();
    EASYJIT_RT_LOG("PSEUDO_LL_CALL: result=%p ret_ty=%s ret_hint=%u called=%p called_fn=%p called_name=%.*s args=%u\n",
                   (void *)CB, TypeKindName(CB->getType()),
                   TypeBitHint(CB->getType()), (void *)CB->getCalledOperand(),
                   (void *)CalledFn, (int)CN.size(), CN.data(),
                   (unsigned)CB->arg_size());
  } else if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
    EASYJIT_RT_LOG("PSEUDO_LL_GEP: result=%p source_ty=%s ptr=%p indices=%u inbounds=%d\n",
                   (void *)GEP, TypeKindName(GEP->getSourceElementType()),
                   (void *)GEP->getPointerOperand(), GEP->getNumIndices(),
                   (int)GEP->isInBounds());
  } else if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
    EASYJIT_RT_LOG("PSEUDO_LL_ALLOCA: result=%p alloc_ty=%s alloc_hint=%u array_size=%p align=%u\n",
                   (void *)AI, TypeKindName(AI->getAllocatedType()),
                   TypeBitHint(AI->getAllocatedType()),
                   (void *)AI->getArraySize(), (unsigned)AI->getAlign().value());
  } else if (auto *Sel = llvm::dyn_cast<llvm::SelectInst>(&I)) {
    EASYJIT_RT_LOG("PSEUDO_LL_SELECT: result=%p cond=%p true=%p false=%p ty=%s hint=%u\n",
                   (void *)Sel, (void *)Sel->getCondition(),
                   (void *)Sel->getTrueValue(), (void *)Sel->getFalseValue(),
                   TypeKindName(Sel->getType()), TypeBitHint(Sel->getType()));
  } else if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(&I)) {
    EASYJIT_RT_LOG("PSEUDO_LL_CAST: result=%p op=%s src=%p src_ty=%s src_hint=%u dst_ty=%s dst_hint=%u\n",
                   (void *)Cast, Cast->getOpcodeName(),
                   (void *)Cast->getOperand(0),
                   TypeKindName(Cast->getOperand(0)->getType()),
                   TypeBitHint(Cast->getOperand(0)->getType()),
                   TypeKindName(Cast->getType()), TypeBitHint(Cast->getType()));
  }

  for (unsigned Op = 0, E = I.getNumOperands(); Op != E; ++Op) {
    EASYJIT_RT_LOG("PSEUDO_LL_OP: owner=%p op=%u\n", (void *)&I, Op);
    LogPseudoLlValue("OP", I.getOperand(Op));
  }
}

static void DumpFunctionPseudoIRToSre(llvm::Module &M, const char *Name,
                                      const char *Reason) {
  EASYJIT_RT_LOG("PSEUDOIR: begin reason=%s module=%p target=%s\n",
                 Reason ? Reason : "<null>", (void *)&M,
                 Name ? Name : "<null>");

  llvm::Function *F = Name ? M.getFunction(Name) : nullptr;
  if (!F) {
    EASYJIT_RT_LOG("PSEUDOIR: target missing, listing functions only\n");
    for (llvm::Function &MF : M) {
      llvm::StringRef FN = MF.getName();
      EASYJIT_RT_LOG("PSEUDOIR_FUNC_LIST: name=%.*s fn=%p decl=%d broken=%d args=%u\n",
                     (int)FN.size(), FN.data(), (void *)&MF,
                     (int)MF.isDeclaration(),
                     (int)llvm::verifyFunction(MF, nullptr),
                     (unsigned)MF.arg_size());
    }
    EASYJIT_RT_LOG("PSEUDOIR: end\n");
    return;
  }

  llvm::StringRef FN = F->getName();
  EASYJIT_RT_LOG("PSEUDO_LL_FUNC: define ret_ty=%s ret_hint=%u name=%.*s fn=%p args=%u\n",
                 TypeKindName(F->getReturnType()), TypeBitHint(F->getReturnType()),
                 (int)FN.size(), FN.data(), (void *)F,
                 (unsigned)F->arg_size());
  EASYJIT_RT_RAW("[easyjit][sre] [runtime] PSEUDO_LL_TEXT: define ");
  PrintTypeFrag(F->getReturnType());
  EASYJIT_RT_RAW(" @%.*s(", (int)FN.size(), FN.data());
  unsigned TextArgIndex = 0;
  for (llvm::Argument &Arg : F->args()) {
    if (TextArgIndex)
      EASYJIT_RT_RAW(", ");
    PrintTypedValueFrag(&Arg);
    ++TextArgIndex;
  }
  EASYJIT_RT_RAW(") {\n");

  EASYJIT_RT_LOG("PSEUDOIR_FUNC: name=%.*s fn=%p decl=%d broken=%d ret_kind=%s ret_hint=%u args=%u bbs=%zu attrs_sets=%u cc=%u linkage=%u\n",
                 (int)FN.size(), FN.data(), (void *)F, (int)F->isDeclaration(),
                 (int)llvm::verifyFunction(*F, nullptr),
                 TypeKindName(F->getReturnType()), TypeBitHint(F->getReturnType()),
                 (unsigned)F->arg_size(), (size_t)F->size(),
                 F->getAttributes().getNumAttrSets(),
                 (unsigned)F->getCallingConv(), (unsigned)F->getLinkage());

  unsigned ArgIndex = 0;
  for (llvm::Argument &Arg : F->args()) {
    llvm::StringRef AN = Arg.hasName() ? Arg.getName() : llvm::StringRef();
    EASYJIT_RT_LOG("PSEUDOIR_ARG: idx=%u arg=%p type_kind=%s type_hint=%u name=%.*s\n",
                   ArgIndex, (void *)&Arg, TypeKindName(Arg.getType()),
                   TypeBitHint(Arg.getType()), (int)AN.size(), AN.data());
    ++ArgIndex;
  }

  unsigned BBIndex = 0;
  for (llvm::BasicBlock &BB : *F) {
    llvm::StringRef BBN = BB.hasName() ? BB.getName() : llvm::StringRef();
    unsigned PredCount = 0;
    for (llvm::BasicBlock *Pred : llvm::predecessors(&BB)) {
      (void)Pred;
      ++PredCount;
    }
    EASYJIT_RT_LOG("PSEUDOIR_BB: index=%u bb=%p name=%.*s preds=%u insts=%zu term=%p\n",
                   BBIndex, (void *)&BB, (int)BBN.size(), BBN.data(),
                   PredCount, (size_t)BB.size(), (void *)BB.getTerminator());
    EASYJIT_RT_LOG("PSEUDO_LL_BB: bb=%p index=%u name=%.*s preds=%u insts=%zu\n",
                   (void *)&BB, BBIndex, (int)BBN.size(), BBN.data(),
                   PredCount, (size_t)BB.size());
    EASYJIT_RT_RAW("[easyjit][sre] [runtime] PSEUDO_LL_TEXT: ");
    if (!BBN.empty())
      EASYJIT_RT_RAW("%.*s", (int)BBN.size(), BBN.data());
    else
      EASYJIT_RT_RAW("bb%p", (void *)&BB);
    EASYJIT_RT_RAW(":\n");

    unsigned InstIndex = 0;
    for (llvm::Instruction &I : BB) {
      llvm::StringRef IN = I.hasName() ? I.getName() : llvm::StringRef();
      EASYJIT_RT_LOG("PSEUDOIR_INST: bb=%u inst=%u ptr=%p opcode=%u opcode_name=%s result_name=%.*s type_kind=%s type_hint=%u ops=%u parent=%p\n",
                     BBIndex, InstIndex, (void *)&I, I.getOpcode(),
                     I.getOpcodeName(), (int)IN.size(), IN.data(),
                     TypeKindName(I.getType()), TypeBitHint(I.getType()),
                     I.getNumOperands(), (void *)I.getParent());
      LogPseudoLlInst(I, BBIndex, InstIndex);

      if (auto *CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
        llvm::Value *Called = CB->getCalledOperand();
        llvm::Function *CalledFn = CB->getCalledFunction();
        llvm::StringRef CN =
            CalledFn ? CalledFn->getName() : llvm::StringRef();
        EASYJIT_RT_LOG("PSEUDOIR_CALL: inst=%p called=%p called_fn=%p called_name=%.*s args=%u fty=%p\n",
                       (void *)CB, (void *)Called, (void *)CalledFn,
                       (int)CN.size(), CN.data(), (unsigned)CB->arg_size(),
                       (void *)CB->getFunctionType());
      }

      if (auto *BR = llvm::dyn_cast<llvm::BranchInst>(&I)) {
        EASYJIT_RT_LOG("PSEUDOIR_BR: inst=%p conditional=%d successors=%u\n",
                       (void *)BR, (int)BR->isConditional(),
                       BR->getNumSuccessors());
      } else if (auto *RI = llvm::dyn_cast<llvm::ReturnInst>(&I)) {
        EASYJIT_RT_LOG("PSEUDOIR_RET: inst=%p retv=%p\n", (void *)RI,
                       (void *)RI->getReturnValue());
      } else if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
        EASYJIT_RT_LOG("PSEUDOIR_LOAD: inst=%p ptr=%p volatile=%d align=%u\n",
                       (void *)LI, (void *)LI->getPointerOperand(),
                       (int)LI->isVolatile(), (unsigned)LI->getAlign().value());
      } else if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
        EASYJIT_RT_LOG("PSEUDOIR_STORE: inst=%p val=%p ptr=%p volatile=%d align=%u\n",
                       (void *)SI, (void *)SI->getValueOperand(),
                       (void *)SI->getPointerOperand(), (int)SI->isVolatile(),
                       (unsigned)SI->getAlign().value());
      }

      for (unsigned Op = 0, E = I.getNumOperands(); Op != E; ++Op) {
        llvm::Value *V = I.getOperand(Op);
        EASYJIT_RT_LOG("PSEUDOIR_OP: bb=%u inst=%u op=%u owner=%p\n",
                       BBIndex, InstIndex, Op, (void *)&I);
        LogPseudoValue("PSEUDOIR_OP_VALUE:", V);
      }
      ++InstIndex;
    }
    ++BBIndex;
  }

  EASYJIT_RT_LOG("PSEUDO_LL_FUNC_END: fn=%p\n", (void *)F);
  EASYJIT_RT_RAW("[easyjit][sre] [runtime] PSEUDO_LL_TEXT: }\n");
  EASYJIT_RT_LOG("PSEUDOIR: end\n");
}

static void LogBasicIRShape(llvm::Module &M) {
  EASYJIT_RT_LOG("IRCHK: begin module=%p funcs=%zu globals=%zu named_md=%zu triple=%s dl_empty=%d\n",
                 (void *)&M, (size_t)M.size(), (size_t)M.global_size(),
                 (size_t)M.named_metadata_size(), M.getTargetTriple().c_str(),
                 (int)M.getDataLayoutStr().empty());

  for (llvm::NamedMDNode &NMD : M.named_metadata()) {
    llvm::StringRef NN = NMD.getName();
    EASYJIT_RT_LOG("IRCHK: named_md %.*s operands=%u\n", (int)NN.size(),
                   NN.data(), NMD.getNumOperands());
  }

  for (llvm::GlobalVariable &GV : M.globals()) {
    llvm::StringRef GN = GV.getName();
    bool InitMismatch =
        GV.hasInitializer() && GV.getInitializer()->getType() != GV.getValueType();
    bool Interesting = IsInterestingGlobalName(GN) || InitMismatch ||
                       GV.hasAppendingLinkage() || GV.hasCommonLinkage();
    if (!Interesting)
      continue;
    EASYJIT_RT_LOG("IRCHK_GLOBAL: %.*s gv=%p value_ty=%p kind=%s has_init=%d linkage=%u constant=%d init_mismatch=%d\n",
                   (int)GN.size(), GN.data(), (void *)&GV,
                   (void *)GV.getValueType(), TypeKindName(GV.getValueType()),
                   (int)GV.hasInitializer(), (unsigned)GV.getLinkage(),
                   (int)GV.isConstant(), (int)InitMismatch);
    if (InitMismatch)
      LogValueBrief("IRCHK_GLOBAL: bad initializer", GV.getInitializer());
  }

  for (llvm::Function &F : M) {
    llvm::StringRef FN = F.getName();
    bool FunctionBroken = llvm::verifyFunction(F, nullptr);
    llvm::FunctionType *FT = F.getFunctionType();
    llvm::AttributeList Attrs = F.getAttributes();
    bool CtxOk = &F.getContext() == &M.getContext();
    bool ParamCountOk = FT && FT->getNumParams() == F.arg_size();
    bool AttrCtxOk = Attrs.hasParentContext(M.getContext());
    bool AttrCountOk = Attrs.getNumAttrSets() <= F.arg_size() + 2;
    EASYJIT_RT_LOG("IRCHK_FUNC: %.*s fn=%p decl=%d broken=%d ctx_ok=%d ft=%p ft_params=%u args=%u param_count_ok=%d attrs=%p attrs_empty=%d attrs_ctx_ok=%d attrs_sets=%u attrs_count_ok=%d ret_kind=%s ret_hint=%u cc=%u linkage=%u intrinsic=%d\n",
                   (int)FN.size(), FN.data(), (void *)&F,
                   (int)F.isDeclaration(), (int)FunctionBroken, (int)CtxOk,
                   (void *)FT, FT ? FT->getNumParams() : 0,
                   (unsigned)F.arg_size(), (int)ParamCountOk,
                   Attrs.getRawPointer(), (int)Attrs.isEmpty(),
                   (int)AttrCtxOk, Attrs.getNumAttrSets(), (int)AttrCountOk,
                   TypeKindName(F.getReturnType()),
                   TypeBitHint(F.getReturnType()),
                   (unsigned)F.getCallingConv(), (unsigned)F.getLinkage(),
                   (int)F.isIntrinsic());
    if (!CtxOk || !ParamCountOk || !AttrCtxOk || !AttrCountOk)
      EASYJIT_RT_LOG("IRCHK_FUNC_ERR: %.*s ctx_ok=%d param_count_ok=%d attrs_ctx_ok=%d attrs_count_ok=%d\n",
                     (int)FN.size(), FN.data(), (int)CtxOk,
                     (int)ParamCountOk, (int)AttrCtxOk, (int)AttrCountOk);

    if (F.isDeclaration() || !FunctionBroken)
      continue;

    unsigned ArgIndex = 0;
    for (llvm::Argument &Arg : F.args()) {
      EASYJIT_RT_LOG("IRCHK_ARG: fn=%p idx=%u arg=%p kind=%s hint=%u\n",
                     (void *)&F, ArgIndex, (void *)&Arg,
                     TypeKindName(Arg.getType()), TypeBitHint(Arg.getType()));
      ++ArgIndex;
    }

    LogNameRef("IRCHK: function ", FN);

    unsigned BBIndex = 0;
    for (llvm::BasicBlock &BB : F) {
      unsigned PredCount = 0;
      for (llvm::BasicBlock *Pred : llvm::predecessors(&BB)) {
        (void)Pred;
        ++PredCount;
      }

      llvm::Instruction *Term = BB.getTerminator();
      EASYJIT_RT_LOG("IRCHK: bb=%p index=%u preds=%u term=%p insts=%zu\n",
                     (void *)&BB, BBIndex, PredCount, (void *)Term,
                     (size_t)BB.size());
      if (!Term)
        EASYJIT_RT_LOG("IRCHK_ERR: bb=%p has no terminator\n", (void *)&BB);

      bool SeenNonPhi = false;
      unsigned InstIndex = 0;
      for (llvm::Instruction &I : BB) {
        EASYJIT_RT_LOG("IRCHK_INST: bb=%u inst=%u ptr=%p opcode=%s ty=%p kind=%s hint=%u ops=%u\n",
                       BBIndex, InstIndex, (void *)&I, I.getOpcodeName(),
                       (void *)I.getType(), TypeKindName(I.getType()),
                       TypeBitHint(I.getType()), I.getNumOperands());

        if (I.getParent() != &BB)
          EASYJIT_RT_LOG("IRCHK_ERR: inst=%p opcode=%s parent=%p expected=%p\n",
                         (void *)&I, I.getOpcodeName(), (void *)I.getParent(),
                         (void *)&BB);

        if (llvm::isa<llvm::PHINode>(&I)) {
          if (SeenNonPhi)
            EASYJIT_RT_LOG("IRCHK_ERR: phi after non-phi inst=%p bb=%p\n",
                           (void *)&I, (void *)&BB);
        } else {
          SeenNonPhi = true;
        }

        if (auto *PN = llvm::dyn_cast<llvm::PHINode>(&I)) {
          unsigned Incoming = PN->getNumIncomingValues();
          EASYJIT_RT_LOG("IRCHK: phi=%p incoming=%u preds=%u\n", (void *)PN,
                         Incoming, PredCount);
          if (Incoming != PredCount)
            EASYJIT_RT_LOG("IRCHK_ERR: phi incoming/pred mismatch phi=%p incoming=%u preds=%u bb=%p\n",
                           (void *)PN, Incoming, PredCount, (void *)&BB);

          for (unsigned IIdx = 0; IIdx < Incoming; ++IIdx) {
            llvm::BasicBlock *IncBB = PN->getIncomingBlock(IIdx);
            bool FoundPred = false;
            for (llvm::BasicBlock *Pred : llvm::predecessors(&BB)) {
              if (Pred == IncBB) {
                FoundPred = true;
                break;
              }
            }
            if (!FoundPred)
              EASYJIT_RT_LOG("IRCHK_ERR: phi=%p incoming block not predecessor idx=%u incbb=%p bb=%p\n",
                             (void *)PN, IIdx, (void *)IncBB, (void *)&BB);
          }
        }

        for (unsigned Op = 0, E = I.getNumOperands(); Op != E; ++Op) {
          llvm::Value *V = I.getOperand(Op);
          if (!V)
            EASYJIT_RT_LOG("IRCHK_ERR: null operand inst=%p opcode=%s op=%u\n",
                           (void *)&I, I.getOpcodeName(), Op);
          else
            EASYJIT_RT_LOG("IRCHK_OP: inst=%u op=%u value=%p kind=%s hint=%u\n",
                           InstIndex, Op, (void *)V, TypeKindName(V->getType()),
                           TypeBitHint(V->getType()));
        }

        if (auto *RI = llvm::dyn_cast<llvm::ReturnInst>(&I)) {
          llvm::Value *RV = RI->getReturnValue();
          EASYJIT_RT_LOG("IRCHK_RET: inst=%p retv=%p fn_ret_kind=%s val_kind=%s\n",
                         (void *)RI, (void *)RV, TypeKindName(F.getReturnType()),
                         RV ? TypeKindName(RV->getType()) : "void");
          if ((RV == nullptr) != F.getReturnType()->isVoidTy())
            EASYJIT_RT_LOG("IRCHK_ERR: ret void/value mismatch inst=%p fn=%p\n",
                           (void *)RI, (void *)&F);
          if (RV && RV->getType() != F.getReturnType())
            EASYJIT_RT_LOG("IRCHK_ERR: ret type mismatch inst=%p val_ty=%p fn_ret_ty=%p\n",
                           (void *)RI, (void *)RV->getType(),
                           (void *)F.getReturnType());
        }

        if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
          llvm::Value *Ptr = LI->getPointerOperand();
          EASYJIT_RT_LOG("IRCHK_LOAD: inst=%p ptr=%p ptr_kind=%s result_kind=%s\n",
                         (void *)LI, (void *)Ptr,
                         Ptr ? TypeKindName(Ptr->getType()) : "null",
                         TypeKindName(LI->getType()));
          if (!Ptr || !Ptr->getType()->isPointerTy())
            EASYJIT_RT_LOG("IRCHK_ERR: load ptr operand not pointer inst=%p\n",
                           (void *)LI);
        }

        if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
          llvm::Value *Val = SI->getValueOperand();
          llvm::Value *Ptr = SI->getPointerOperand();
          EASYJIT_RT_LOG("IRCHK_STORE: inst=%p val=%p val_kind=%s ptr=%p ptr_kind=%s\n",
                         (void *)SI, (void *)Val,
                         Val ? TypeKindName(Val->getType()) : "null",
                         (void *)Ptr, Ptr ? TypeKindName(Ptr->getType()) : "null");
          if (!Ptr || !Ptr->getType()->isPointerTy())
            EASYJIT_RT_LOG("IRCHK_ERR: store ptr operand not pointer inst=%p\n",
                           (void *)SI);
        }

        if (auto *CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
          llvm::FunctionType *FTy = CB->getFunctionType();
          llvm::Value *Called = CB->getCalledOperand();
          EASYJIT_RT_LOG("IRCHK_CALL: inst=%p called=%p called_kind=%s fty=%p ret_kind=%s params=%u args=%u\n",
                         (void *)CB, (void *)Called,
                         Called ? TypeKindName(Called->getType()) : "null",
                         (void *)FTy, FTy ? TypeKindName(FTy->getReturnType()) : "null",
                         FTy ? FTy->getNumParams() : 0, (unsigned)CB->arg_size());
          if (FTy && !FTy->isVarArg() && FTy->getNumParams() != CB->arg_size())
            EASYJIT_RT_LOG("IRCHK_ERR: call arg count mismatch inst=%p params=%u args=%u\n",
                           (void *)CB, FTy->getNumParams(),
                           (unsigned)CB->arg_size());
          if (FTy) {
            unsigned N = FTy->getNumParams();
            if (N > CB->arg_size())
              N = CB->arg_size();
            for (unsigned A = 0; A != N; ++A) {
              llvm::Type *PT = FTy->getParamType(A);
              llvm::Value *AV = CB->getArgOperand(A);
              EASYJIT_RT_LOG("IRCHK_CALL_ARG: inst=%p arg=%u param_kind=%s val_kind=%s\n",
                             (void *)CB, A, TypeKindName(PT),
                             AV ? TypeKindName(AV->getType()) : "null");
              if (!AV || AV->getType() != PT)
                EASYJIT_RT_LOG("IRCHK_ERR: call arg type mismatch inst=%p arg=%u param_ty=%p val_ty=%p\n",
                               (void *)CB, A, (void *)PT,
                               AV ? (void *)AV->getType() : nullptr);
            }
          }
        }

        ++InstIndex;
      }
      EASYJIT_RT_LOG("IRCHK: bb done index=%u insts_seen=%u\n", BBIndex,
                     InstIndex);
      ++BBIndex;
    }
  }
  EASYJIT_RT_LOG("IRCHK: end\n");
}

static bool CanFallbackToOriginalFunction(easy::Context const& C,
                                          llvm::Module const& M,
                                          const char* Name) {
  EASYJIT_RT_LOG("CanFallbackToOriginalFunction: begin name=%s ctx_size=%zu\n",
                 Name ? Name : "<null>", C.size());
  if (!Name)
    return false;

  llvm::Function *F = M.getFunction(Name);
  if (!F) {
    EASYJIT_RT_LOG("CanFallbackToOriginalFunction: no function\n");
    return false;
  }

  if (C.size() != F->arg_size()) {
    EASYJIT_RT_LOG("CanFallbackToOriginalFunction: arg mismatch ctx=%zu fn=%u\n",
                   C.size(), (unsigned)F->arg_size());
    return false;
  }

  for (size_t I = 0; I < C.size(); ++I) {
    auto const *Forward = C.getArgumentMapping(I).as<easy::ForwardArgument>();
    if (!Forward || Forward->get() != I) {
      EASYJIT_RT_LOG("CanFallbackToOriginalFunction: arg %zu not identity forward\n", I);
      return false;
    }
  }

  EASYJIT_RT_LOG("CanFallbackToOriginalFunction: yes\n");
  return true;
}

static std::unique_ptr<easy::Function>
MakeOriginalFunctionFallback(void *Addr, std::string Reason) {
  EASYJIT_RT_LOG("Function::Compile: using original function fallback: %s\n",
                 Reason.c_str());
  if (const char *Verbose = std::getenv("EASYJIT_LIGHT_VERBOSE");
      Verbose && Verbose[0] != '\0' && Verbose[0] != '0') {
    std::fprintf(stderr, "[easyjit/light] fallback: %s\n", Reason.c_str());
    std::fflush(stderr);
  }
  std::unique_ptr<easy::LLVMHolder> Holder(
      new OriginalFunctionHolder(std::move(Reason)));
  return std::unique_ptr<easy::Function>(
      new easy::Function(Addr, std::move(Holder)));
}
} // namespace

#if !EASYJIT_LIGHT_BACKEND_ONLY
static std::unique_ptr<llvm::TargetMachine> GetTargetMachineForModule(llvm::Module const& M) {
  std::string TripleStr = M.getTargetTriple();
  if (TripleStr.empty()) {
    TripleStr = llvm::sys::getProcessTriple();
  }

  llvm::Triple Triple(TripleStr);
  std::string Error;
  const llvm::Target* Target = llvm::TargetRegistry::lookupTarget(TripleStr, Error);

  EASYJIT_RT_LOG("GetTargetMachineForModule: module_triple=%s effective_triple=%s cpu=%s\n",
                 M.getTargetTriple().c_str(),
                 TripleStr.c_str(),
                 llvm::sys::getHostCPUName().str().c_str());

  if (!Target) {
    EASYJIT_RT_LOG("GetTargetMachineForModule: lookup failed error=%s\n", Error.c_str());
    return nullptr;
  }

  llvm::TargetOptions Options;
  std::unique_ptr<llvm::TargetMachine> TM(
      Target->createTargetMachine(TripleStr,
                                  llvm::sys::getHostCPUName().str(),
                                  "",
                                  Options,
                                  llvm::Reloc::PIC_,
                                  llvm::CodeModel::Small,
                                  llvm::CodeGenOpt::Aggressive));
  EASYJIT_RT_LOG("GetTargetMachineForModule: result=%p\n", (void*)TM.get());
  return TM;
}
#endif // !EASYJIT_LIGHT_BACKEND_ONLY

static void Optimize(llvm::Module& M, const char* Name, const easy::Context& C, unsigned OptLevel, unsigned OptSize) {

  std::string TripleStr = M.getTargetTriple();
  if (TripleStr.empty()) {
    TripleStr = llvm::sys::getProcessTriple();
  }
  llvm::Triple Triple{TripleStr};
  EASYJIT_RT_LOG("Optimize: begin name=%s triple=%s opt=%u size=%u module_triple=%s datalayout=%s\n",
                 Name ? Name : "<null>",
                 TripleStr.c_str(),
                 OptLevel,
                 OptSize,
                 M.getTargetTriple().c_str(),
                 M.getDataLayoutStr().c_str());

#if !EASYJIT_LIGHT_BACKEND_ONLY
  std::unique_ptr<llvm::TargetMachine> TM = GetTargetMachineForModule(M);
  if (!TM) {
    EASYJIT_RT_LOG("Optimize: target machine creation failed for %s\n", Name ? Name : "<null>");
    return;
  }
  M.setTargetTriple(TripleStr);
  M.setDataLayout(TM->createDataLayout());
  EASYJIT_RT_LOG("Optimize: adjusted module triple=%s datalayout=%s\n",
                 M.getTargetTriple().c_str(),
                 M.getDataLayoutStr().c_str());
#else
  // Light-only: no LLVMCodeGen / LLVMMC available. Keep the module's
  // existing triple/DataLayout (set by clang at -emit-llvm time) and
  // skip TargetMachine-derived analyses (TTI). The lightweight pass
  // pipeline below works without TTI.
  if (M.getTargetTriple().empty())
    M.setTargetTriple(TripleStr);
  EASYJIT_RT_LOG("Optimize: light-only mode, using module triple=%s datalayout=%s\n",
                 M.getTargetTriple().c_str(),
                 M.getDataLayoutStr().c_str());
#endif

  // Lightweight optimization pipeline (synced from llvm15_trim).
  //
  // We replace the legacy O2/O3 PassManagerBuilder pipeline with an
  // explicit, minimal sequence that exactly covers what EasyJIT needs
  // to fold the bound snapshot constants:
  //
  //   ContextAnalysis        - bind easy::Context to the module
  //   InlineParameters       - rewrite the wrapper to feed in the
  //                            captured argument constants
  //   DevirtualizeConstant   - rewrite indirect calls when the
  //                            callee is now a known constant
  //   FunctionInlining       - inline wrapper -> original (critical)
  //   ConstStructPropagate   - custom lightweight pass that walks
  //                            alloca -> store(GEP, const) -> load(GEP)
  //                            chains, propagates the constant to the
  //                            load, folds resulting branches and ICmps,
  //                            and removes dead code. Replaces
  //                            SROA + SCCP + InstCombine + Reassociate
  //                            + ADCE for the snapshot pattern.
  //   mem2reg                - lift remaining non-struct allocas to SSA
  //   ConstStructPropagate   - second round, picks up constants that
  //                            mem2reg exposed
  //   InstCombine            - cheap canonical cleanup after specialization
  //   LoopSimplify/LCSSA     - canonical form for loop transforms (Opt>=2)
  //   LoopRotate             - canonical do-while shape (Opt>=2)
  //   LoopUnroll             - fully unroll loops whose trip count became
  //                            a compile-time constant after specialization
  //                            (Opt>=2). Light backend friendly: produces
  //                            straight-line scalar IR, no vector IR.
  //   InstCombine (2nd)      - cleanup after unroll exposes new constants
  //   CFGSimplification      - prune now-dead branches/blocks
  //   Internalize            - hide everything but the JIT entry
  //   GlobalDCE              - drop now-unreachable globals/functions
  //   StripDeadPrototypes    - drop dangling external decls
  //
  // Snapshot/InlineParameters/DevirtualizeConstant/global mapping
  // mechanics are unchanged; only the *cleanup* pipeline shrinks.
  // LLJIT/ORC backend (CreateJIT, CompileAndWrap, MapGlobals) is
  // unchanged.

  llvm::legacy::PassManager MPM;
  EASYJIT_RT_LOG("Optimize: skip TargetTransformInfo pass\n");
  // Do not install TargetTransformInfo here.  The target SDK used by the
  // embedded light-runtime path has an unstable libc++ std::function
  // implementation, and createTargetTransformInfoWrapperPass constructs a
  // TargetIRAnalysis callback through std::function.  The EasyJIT pipeline
  // below is driven by binding/snapshot propagation and simple scalar cleanup;
  // it does not require a target cost model.  Skipping TTI avoids the
  // std::function::swap crash while preserving the light backend's IR shape.

#define EASYJIT_ADD_OPT_PASS(Label, CreateExpr)                               \
  do {                                                                        \
    EASYJIT_RT_LOG("Optimize: before create %s\n", Label);                   \
    llvm::Pass *EasyJitOptPass = (CreateExpr);                                \
    EASYJIT_RT_LOG("Optimize: after create %s pass=%p\n", Label,             \
                   (void *)EasyJitOptPass);                                   \
    EASYJIT_RT_LOG("Optimize: before add %s pass=%p\n", Label,               \
                   (void *)EasyJitOptPass);                                   \
    MPM.add(EasyJitOptPass);                                                  \
    EASYJIT_RT_LOG("Optimize: after add %s pass=%p\n", Label,                \
                   (void *)EasyJitOptPass);                                   \
  } while (false)

  EASYJIT_ADD_OPT_PASS("ContextAnalysis",
                       easy::createContextAnalysisPass(C));
  EASYJIT_ADD_OPT_PASS("InlineParameters",
                       easy::createInlineParametersPass(Name));
  EASYJIT_ADD_OPT_PASS("DevirtualizeConstant",
                       easy::createDevirtualizeConstantPass(Name));
  // Inline the wrapper -> original-function call (critical).
  EASYJIT_ADD_OPT_PASS("FunctionInlining",
                       llvm::createFunctionInliningPass(OptLevel, OptSize,
                                                        false));
  // Custom lightweight propagator: alloca/store/GEP/load -> const.
  EASYJIT_ADD_OPT_PASS("ConstStructPropagate#1",
                       easy::createConstStructPropagatePass(Name));
  // Promote remaining allocas to SSA.
  EASYJIT_ADD_OPT_PASS("PromoteMemoryToRegister",
                       llvm::createPromoteMemoryToRegisterPass());
  // Second round picks up constants exposed by mem2reg.
  EASYJIT_ADD_OPT_PASS("ConstStructPropagate#2",
                       easy::createConstStructPropagatePass(Name));
  EASYJIT_RT_LOG("Optimize: before InstCombine verifyModule begin\n");
  bool BrokenDebugInfoBeforeInstCombine = false;
  bool BrokenBeforeInstCombine =
      llvm::verifyModule(M, nullptr, &BrokenDebugInfoBeforeInstCombine);
  EASYJIT_RT_LOG("Optimize: before InstCombine verifyModule end ir_broken=%d debug_broken=%d\n",
                 (int)BrokenBeforeInstCombine,
                 (int)BrokenDebugInfoBeforeInstCombine);
  if (BrokenDebugInfoBeforeInstCombine && !BrokenBeforeInstCombine) {
    EASYJIT_RT_LOG("Optimize: stripping broken debug info before InstCombine\n");
    bool Stripped = llvm::StripDebugInfo(M);
    EASYJIT_RT_LOG("Optimize: StripDebugInfo changed=%d\n", (int)Stripped);
    bool BrokenDebugInfoAfterStrip = false;
    bool BrokenAfterStrip =
        llvm::verifyModule(M, nullptr, &BrokenDebugInfoAfterStrip);
    EASYJIT_RT_LOG("Optimize: after StripDebugInfo verify ir_broken=%d debug_broken=%d\n",
                   (int)BrokenAfterStrip, (int)BrokenDebugInfoAfterStrip);
    BrokenBeforeInstCombine = BrokenAfterStrip;
    BrokenDebugInfoBeforeInstCombine = BrokenDebugInfoAfterStrip;
  }
  if (BrokenBeforeInstCombine) {
    DumpFunctionPseudoIRToSre(M, Name, "broken-before-instcombine");
    LogBasicIRShape(M);
    EASYJIT_RT_LOG("Optimize: module broken before InstCombine, stop optimize\n");
    return;
  }
  // Canonicalize simple arithmetic and casts after constants are exposed.
  EASYJIT_ADD_OPT_PASS("InstCombine",
                       llvm::createInstructionCombiningPass());
  // Scalar loop unroll (light backend friendly).
  //
  // EasyJIT specialization typically turns runtime-fixed loop bounds
  // (e.g. cfg->n bound to a constant via snapshot/InlineParameters /
  // ConstStructPropagate) into compile-time constants. Once the trip
  // count is statically known, LLVM's legacy LoopUnroll pass can fully
  // unroll the loop into straight-line scalar IR -- which is exactly
  // the shape the light AArch64 backend can lower (it does not support
  // vector IR or NEON). We only enable this at OptLevel >= 2 so a
  // user explicitly opting into -O0/-O1 sees the original loop shape.
  //
  // We do NOT add the LoopVectorize / SLPVectorize passes -- the light
  // backend cannot consume vector IR. Users with vectorizer-friendly
  // hot loops should compile their JIT-relevant TUs with
  //   -fno-vectorize -fno-slp-vectorize
  // see runtime/LightBackend_LIMITATIONS.md.
  if (OptLevel >= 2) {
    EASYJIT_ADD_OPT_PASS("LoopSimplify", llvm::createLoopSimplifyPass());
    EASYJIT_ADD_OPT_PASS("LCSSA", llvm::createLCSSAPass());
    EASYJIT_ADD_OPT_PASS("LoopRotate", llvm::createLoopRotatePass());
    EASYJIT_ADD_OPT_PASS("LoopUnroll", llvm::createLoopUnrollPass(
        /*OptLevel=*/(int)OptLevel,
        /*OnlyWhenForced=*/false,
        /*ForgetAllSCEV=*/false));
    // NOTE: a 2nd InstCombine here would further canonicalise the
    // unrolled body, but it also re-shapes IR in non-loop functions
    // (gauntlet kernel hit "cast src not in reg" after the rerun).
    // The downstream CFGSimplify + the light backend's per-instruction
    // emitter handle the loop-unroll output without a second pass.
  }
  // Minimal cleanup.
  EASYJIT_ADD_OPT_PASS("CFGSimplification",
                       llvm::createCFGSimplificationPass());
  EASYJIT_ADD_OPT_PASS("Internalize", llvm::createInternalizePass(
      [Name](const llvm::GlobalValue &GV) {
    return GV.getName() == Name || GV.getName() == "__dso_handle";
  }));
  EASYJIT_ADD_OPT_PASS("GlobalDCE", llvm::createGlobalDCEPass());
  EASYJIT_ADD_OPT_PASS("StripDeadPrototypes",
                       llvm::createStripDeadPrototypesPass());

#ifdef NDEBUG
  EASYJIT_ADD_OPT_PASS("Verifier", llvm::createVerifierPass());
#endif

#undef EASYJIT_ADD_OPT_PASS

  EASYJIT_RT_LOG("Optimize: running pass manager for %s\n", Name ? Name : "<null>");
  MPM.run(M);
  EASYJIT_RT_LOG("Optimize: finished for %s\n", Name ? Name : "<null>");

  // Optional IR dump for benchmarking / debugging the pass pipeline.
  if (const char *DumpPath = std::getenv("EASYJIT_DUMP_IR")) {
    std::error_code EC;
    llvm::raw_fd_ostream OS(DumpPath, EC);
    if (!EC) M.print(OS, nullptr);
  }
}

static void DisableRecursiveJit(llvm::Module &M, const char *EntryName) {
  EASYJIT_RT_LOG("DisableRecursiveJit: begin entry=%s module=%p\n",
                 EntryName ? EntryName : "<null>", (void*)&M);
  if (!EntryName)
    return;

  for (const char *CtorDtorName : {"llvm.global_ctors", "llvm.global_dtors"}) {
    if (llvm::GlobalVariable *GV = M.getGlobalVariable(CtorDtorName)) {
      EASYJIT_RT_LOG("DisableRecursiveJit: erase %s\n", CtorDtorName);
      GV->replaceAllUsesWith(llvm::UndefValue::get(GV->getType()));
      GV->eraseFromParent();
    }
  }

  for (llvm::Function &F : M) {
    if (F.isDeclaration() || F.getName() == EntryName)
      continue;
    if (F.getName().startswith("llvm."))
      continue;
    if (F.getName() == "register_layout")
      continue;

    EASYJIT_RT_LOG("DisableRecursiveJit: externalize helper %s\n",
                   F.getName().str().c_str());
    F.deleteBody();
    F.setComdat(nullptr);
    F.setSection("");
    F.setVisibility(llvm::GlobalValue::DefaultVisibility);
    F.setLinkage(llvm::GlobalValue::ExternalLinkage);
  }
}

static std::string TakeError(llvm::Error Err) {
  return llvm::toString(std::move(Err));
}

#if !EASYJIT_LIGHT_BACKEND_ONLY
static llvm::orc::JITTargetMachineBuilder
GetJITTargetMachineBuilderForModule(llvm::Module const& M) {
  std::string TripleStr = M.getTargetTriple();
  if (TripleStr.empty()) {
    TripleStr = llvm::sys::getProcessTriple();
  }

  llvm::orc::JITTargetMachineBuilder JTMB{llvm::Triple(TripleStr)};
  JTMB.setCPU(llvm::sys::getHostCPUName().str());
  JTMB.setRelocationModel(llvm::Reloc::PIC_);
  JTMB.setCodeModel(llvm::CodeModel::Small);
  JTMB.setCodeGenOptLevel(llvm::CodeGenOpt::Aggressive);

  EASYJIT_RT_LOG("GetJITTargetMachineBuilderForModule: module_triple=%s effective_triple=%s cpu=%s\n",
                 M.getTargetTriple().c_str(),
                 TripleStr.c_str(),
                 llvm::sys::getHostCPUName().str().c_str());
  return JTMB;
}

static std::unique_ptr<easy::detail::MinimalOrcJIT>
CreateJIT(llvm::Module const& M, const char *Name) {
  EASYJIT_RT_LOG("CreateJIT: begin name=%s triple=%s datalayout=%s\n",
                 Name ? Name : "<null>",
                 M.getTargetTriple().c_str(),
                 M.getDataLayoutStr().c_str());

  auto JITOrErr = easy::detail::MinimalOrcJIT::Create(
      GetJITTargetMachineBuilderForModule(M), M.getDataLayout());
  if (!JITOrErr) {
    auto Err = TakeError(JITOrErr.takeError());
    EASYJIT_RT_LOG("CreateJIT: failed name=%s error=%s\n",
                   Name ? Name : "<null>", Err.c_str());
    return nullptr;
  }

  EASYJIT_RT_LOG("CreateJIT: success name=%s jit=%p\n",
                 Name ? Name : "<null>", (void*)JITOrErr->get());
  return std::move(*JITOrErr);
}

static bool MapGlobals(easy::detail::MinimalOrcJIT& JIT, GlobalMapping* Globals) {
  EASYJIT_RT_LOG("MapGlobals: begin jit=%p globals=%p\n", (void*)&JIT, (void*)Globals);

  llvm::orc::MangleAndInterner Mangle(JIT.getExecutionSession(), JIT.getDataLayout());
  llvm::orc::SymbolMap Symbols;

  for(GlobalMapping *GM = Globals; GM && GM->Name; ++GM) {
    EASYJIT_RT_LOG("MapGlobals: map %s -> %p\n", GM->Name, GM->Address);
    Symbols[Mangle(GM->Name)] =
      llvm::JITEvaluatedSymbol(llvm::pointerToJITTargetAddress(GM->Address),
                               llvm::JITSymbolFlags::Exported);
  }

  EASYJIT_RT_LOG("MapGlobals: map __dso_handle -> %p\n", &__dso_handle);
  Symbols[Mangle("__dso_handle")] =
    llvm::JITEvaluatedSymbol(llvm::pointerToJITTargetAddress(&__dso_handle),
                             llvm::JITSymbolFlags::Exported);

  if (auto Err = JIT.getMainJITDylib().define(absoluteSymbols(std::move(Symbols)))) {
    auto ErrStr = TakeError(std::move(Err));
    EASYJIT_RT_LOG("MapGlobals: define failed error=%s\n", ErrStr.c_str());
    return false;
  }

  auto GeneratorOrErr =
      llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
          JIT.getDataLayout().getGlobalPrefix());
  if (!GeneratorOrErr) {
    auto ErrStr = TakeError(GeneratorOrErr.takeError());
    EASYJIT_RT_LOG("MapGlobals: current-process generator failed error=%s\n", ErrStr.c_str());
    return false;
  }

  JIT.getMainJITDylib().addGenerator(std::move(*GeneratorOrErr));
  EASYJIT_RT_LOG("MapGlobals: end\n");
  return true;
}
#endif // !EASYJIT_LIGHT_BACKEND_ONLY

static void WriteOptimizedToFile(llvm::Module const &M, std::string const& File) {
  EASYJIT_RT_LOG("WriteOptimizedToFile: file=%s module=%p\n",
                 File.empty() ? "<empty>" : File.c_str(), (const void*)&M);
  if(File.empty())
    return;
  std::error_code Error;
  llvm::raw_fd_ostream Out(File, Error, llvm::sys::fs::OF_None);

  if(Error) {
    EASYJIT_RT_LOG("WriteOptimizedToFile: open failed file=%s error=%s\n",
                   File.c_str(), Error.message().c_str());
    return;
  }

  Out << M;
  EASYJIT_RT_LOG("WriteOptimizedToFile: done file=%s\n", File.c_str());
}

static std::string GetDumpFileWithSuffix(std::string File, llvm::StringRef Suffix) {
  if(File.empty())
    return File;
  llvm::SmallString<256> Path(File);
  llvm::StringRef Extension = llvm::sys::path::extension(Path);
  if(Extension.empty()) {
    Path += Suffix;
  } else {
    llvm::SmallString<256> Stem(Path);
    Stem.resize(Stem.size() - Extension.size());
    Stem += Suffix;
    Stem += Extension;
    Path = Stem;
  }
  return std::string(Path.str());
}

#if !EASYJIT_LIGHT_BACKEND_ONLY
std::unique_ptr<Function>
CompileAndWrap(const char*Name, GlobalMapping* Globals,
               std::unique_ptr<llvm::LLVMContext> Ctx,
               std::unique_ptr<llvm::Module> M) {

  EASYJIT_RT_LOG("CompileAndWrap: begin name=%s globals=%p module=%p ctx=%p\n",
                 Name ? Name : "<null>",
                 (void*)Globals,
                 (void*)M.get(),
                 (void*)Ctx.get());
  auto StoredCtx = std::make_unique<llvm::LLVMContext>();
  auto StoredModule = easy::CloneModuleWithContext(*M, *StoredCtx);
  if (!StoredModule) {
    EASYJIT_RT_LOG("CompileAndWrap: failed to clone optimized module name=%s\n",
                   Name ? Name : "<null>");
    return nullptr;
  }

  auto JIT = CreateJIT(*M, Name);
  if (!JIT)
    return nullptr;

  if(Globals) {
    if (!MapGlobals(*JIT, Globals))
      return nullptr;
  }

  EASYJIT_RT_LOG("CompileAndWrap: addIRModule begin name=%s\n", Name ? Name : "<null>");
  auto TSCtx = std::make_unique<llvm::LLVMContext>();
  auto JITModule = easy::CloneModuleWithContext(*M, *TSCtx);
  if (!JITModule) {
    EASYJIT_RT_LOG("CompileAndWrap: failed to clone jit module name=%s\n",
                   Name ? Name : "<null>");
    return nullptr;
  }

  if (auto Err = JIT->addIRModule(std::move(JITModule), std::move(TSCtx))) {
    auto ErrStr = TakeError(std::move(Err));
    EASYJIT_RT_LOG("CompileAndWrap: addIRModule failed name=%s error=%s\n",
                   Name ? Name : "<null>", ErrStr.c_str());
    return nullptr;
  }
  EASYJIT_RT_LOG("CompileAndWrap: addIRModule end name=%s\n", Name ? Name : "<null>");

  EASYJIT_RT_LOG("CompileAndWrap: lookup begin name=%s\n", Name ? Name : "<null>");
  auto AddressOrErr = JIT->lookup(Name);
  if (!AddressOrErr) {
    auto ErrStr = TakeError(AddressOrErr.takeError());
    EASYJIT_RT_LOG("CompileAndWrap: lookup failed name=%s error=%s\n",
                   Name ? Name : "<null>", ErrStr.c_str());
    return nullptr;
  }

  void *Address = llvm::jitTargetAddressToPointer<void *>(AddressOrErr->getAddress());
  EASYJIT_RT_LOG("CompileAndWrap: lookup end name=%s address=%p\n",
                 Name ? Name : "<null>", Address);

  std::unique_ptr<LLVMHolder> Holder(
      new easy::LLVMHolderImpl{std::move(JIT), std::move(StoredCtx), std::move(StoredModule)});
  EASYJIT_RT_LOG("CompileAndWrap: success name=%s holder=%p\n",
                 Name ? Name : "<null>", (void*)Holder.get());
  return std::unique_ptr<Function>(new Function(Address, std::move(Holder)));
}
#endif // !EASYJIT_LIGHT_BACKEND_ONLY

llvm::Module const& Function::getLLVMModule() const {
  llvm::Module *M = this->Holder->getModule();
  if (!M) {
    EASYJIT_RT_LOG("Function::getLLVMModule: no module, aborting\n");
    std::abort();
  }
  return *M;
}

std::unique_ptr<Function> Function::Compile(void *Addr, easy::Context const& C) {
  // llvm::DebugFlag = true;
  // llvm::setCurrentDebugType("jit");

  auto &BT = BitcodeTracker::GetTracker();
  EASYJIT_RT_LOG("Function::Compile: begin addr=%p ctx_size=%zu\n", Addr, C.size());

  const char* Name;
  GlobalMapping* Globals;
  std::tie(Name, Globals) = BT.getNameAndGlobalMapping(Addr);
  if (!Name) {
    EASYJIT_RT_LOG("Function::Compile: tracker lookup failed addr=%p\n", Addr);
    return nullptr;
  }
  EASYJIT_RT_LOG("Function::Compile: tracker name=%s globals=%p\n",
                 Name ? Name : "<null>", (void*)Globals);

  std::unique_ptr<llvm::Module> M;
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::tie(M, Ctx) = BT.getModule(Addr);
  if (!M || !Ctx) {
    EASYJIT_RT_LOG("Function::Compile: null module/context after tracker lookup addr=%p module=%p ctx=%p\n",
                   Addr, (void*)M.get(), (void*)Ctx.get());
    return nullptr;
  }
  EASYJIT_RT_LOG("Function::Compile: module loaded module=%p ctx=%p module_triple=%s datalayout=%s\n",
                 (void*)M.get(),
                 (void*)Ctx.get(),
                 M ? M->getTargetTriple().c_str() : "<null>",
                 M ? M->getDataLayoutStr().c_str() : "<null>");

  unsigned OptLevel;
  unsigned OptSize;
  std::tie(OptLevel, OptSize) = C.getOptLevel();
  EASYJIT_RT_LOG("Function::Compile: optlevel=%u optsz=%u debugfile=%s\n",
                 OptLevel, OptSize, C.getDebugFile().c_str());

  EASYJIT_RT_LOG("Function::Compile: write before-ir begin\n");
  WriteOptimizedToFile(*M, GetDumpFileWithSuffix(C.getDebugFile(), ".before"));
  EASYJIT_RT_LOG("Function::Compile: write before-ir end\n");

  if (!C.getRecursiveJit()) {
    EASYJIT_RT_LOG("Function::Compile: disabling recursive JIT for %s\n",
                   Name ? Name : "<null>");
    DisableRecursiveJit(*M, Name);
  }

  Optimize(*M, Name, C, OptLevel, OptSize);
  EASYJIT_RT_LOG("Function::Compile: Optimize complete module=%p ctx=%p\n",
                 (void*)M.get(), (void*)Ctx.get());

  EASYJIT_RT_LOG("Function::Compile: write final-ir begin\n");
  WriteOptimizedToFile(*M, C.getDebugFile());
  EASYJIT_RT_LOG("Function::Compile: write final-ir end\n");
  EASYJIT_RT_LOG("Function::Compile: write after-ir begin\n");
  WriteOptimizedToFile(*M, GetDumpFileWithSuffix(C.getDebugFile(), ".after"));
  EASYJIT_RT_LOG("Function::Compile: write after-ir end\n");

  // ORC emits object code through an object streamer, which cannot handle
  // module-level raw inline asm. C++ standard-library headers may inject
  // harmless directives such as ".globl _ZSt21ios_base_library_initv"; strip
  // them before handing the module to the JIT compiler.
  if (!M->getModuleInlineAsm().empty()) {
    EASYJIT_RT_LOG("Function::Compile: stripping module inline asm before JIT\n");
    M->setModuleInlineAsm("");
  }

#if EASYJIT_LIGHT_BACKEND_ENABLED
  {
    auto policy = easy::light_backend::GetPolicyFromEnv();
#if EASYJIT_LIGHT_BACKEND_ONLY
    // Light-only build: no ORC fallback exists. Always go through the
    // light backend; treat any non-success as a hard JIT failure.
    if (policy == easy::light_backend::Policy::Off)
      policy = easy::light_backend::Policy::Try;
#endif
    if (policy != easy::light_backend::Policy::Off) {
      EASYJIT_RT_LOG("Function::Compile: TryLightCompile begin policy=%s\n",
                     easy::light_backend::PolicyName(policy));
      std::unique_ptr<Function> lightFn;
      auto rep = easy::light_backend::TryLightCompile(
          Name, Globals, Ctx, M, lightFn, policy);

      // Helper: when we are about to fail out of Function::Compile, the
      // module must be torn down BEFORE the LLVMContext (M's destructor
      // calls LLVMContext::removeModule). Reset M here so the implicit
      // reverse-declaration order of locals (Ctx destroyed before M) does
      // not crash on the unwind path.
      auto failCleanup = [&]() {
        M.reset();
      };

      switch (rep.outcome) {
        case easy::light_backend::Outcome::Succeeded:
          EASYJIT_RT_LOG("Function::Compile: light path used name=%s\n",
                         Name ? Name : "<null>");
          return lightFn;
        case easy::light_backend::Outcome::FailedForce:
          EASYJIT_RT_LOG("Function::Compile: EASYJIT_LIGHT=force rejected: %s\n",
                         rep.reason.c_str());
          failCleanup();
          return nullptr;
        case easy::light_backend::Outcome::Unsupported:
#if EASYJIT_LIGHT_BACKEND_ONLY
          EASYJIT_RT_LOG("Function::Compile: light unsupported in light-only build: %s\n",
                         rep.reason.c_str());
          if (CanFallbackToOriginalFunction(C, *M, Name)) {
            std::string reason =
                std::string(Name ? Name : "<null>") +
                " (unsupported IR, reason: " + rep.reason +
                "; light-only runtime fell back to original function pointer)";
            failCleanup();
            return MakeOriginalFunctionFallback(Addr, std::move(reason));
          }
          failCleanup();
          return nullptr;
#else
          EASYJIT_RT_LOG("Function::Compile: light unsupported (%s), ORC fallback\n",
                         rep.reason.c_str());
          break;
#endif
        case easy::light_backend::Outcome::SkippedByPolicy:
#if EASYJIT_LIGHT_BACKEND_ONLY
          EASYJIT_RT_LOG("Function::Compile: light skipped in light-only build: %s\n",
                         rep.reason.c_str());
          failCleanup();
          return nullptr;
#else
          EASYJIT_RT_LOG("Function::Compile: light skipped (%s)\n",
                         rep.reason.c_str());
          break;
#endif
      }
    }
  }
#endif

#if EASYJIT_LIGHT_BACKEND_ONLY
  // Light-only build: no ORC fallback compiled in. Tear down M before
  // Ctx (locals destroy in reverse declaration order, but ~Module needs
  // a live LLVMContext).
  M.reset();
  EASYJIT_RT_LOG("Function::Compile: light-only build cannot fall back\n");
  return nullptr;
#else
  EASYJIT_RT_LOG("Function::Compile: CompileAndWrap begin\n");
  auto Result = CompileAndWrap(Name, Globals, std::move(Ctx), std::move(M));
  EASYJIT_RT_LOG("Function::Compile: CompileAndWrap returned function=%p raw=%p\n",
                 (void*)Result.get(), Result ? Result->getRawPointer() : nullptr);
  return Result;
#endif
}

void easy::Function::serialize(std::ostream& os) const {
  llvm::SmallString<0> buf;
  llvm::raw_svector_ostream stream(buf);

  llvm::Module *M = Holder->getModule();
  if (M) {
    llvm::WriteBitcodeToFile(*M, stream);
  }

  os.write(buf.data(), buf.size());
}

std::unique_ptr<easy::Function> easy::Function::deserialize(std::istream& is) {

  auto &BT = BitcodeTracker::GetTracker();

  std::string buf(std::istreambuf_iterator<char>(is), {}); // read the entire istream
  auto MemBuf = llvm::MemoryBuffer::getMemBuffer(llvm::StringRef(buf));

  std::unique_ptr<llvm::LLVMContext> Ctx(new llvm::LLVMContext());
  auto ModuleOrError = llvm::parseBitcodeFile(*MemBuf, *Ctx);
  if(ModuleOrError.takeError()) {
    return nullptr;
  }

  auto M = std::move(ModuleOrError.get());

  std::string FunName = easy::GetEntryFunctionName(*M).str();

  GlobalMapping* Globals = nullptr;
  if(void* OrigFunPtr = BT.getAddress(FunName)) {
    std::tie(std::ignore, Globals) = BT.getNameAndGlobalMapping(OrigFunPtr);
  }

#if EASYJIT_LIGHT_BACKEND_ONLY
  // Light-only build: no ORC fallback. Drive deserialization through
  // the lightweight backend. Force-mode so that any non-success is a
  // hard, observable error rather than a silent nullptr.
#if !EASYJIT_LIGHT_BACKEND_ENABLED
#error "EASYJIT_LIGHT_BACKEND_ONLY=1 requires EASYJIT_LIGHT_BACKEND_ENABLED=1"
#endif
  std::unique_ptr<Function> lightFn;
  auto rep = easy::light_backend::TryLightCompile(
      FunName.c_str(), Globals, Ctx, M, lightFn,
      easy::light_backend::Policy::Force);
  if (rep.outcome == easy::light_backend::Outcome::Succeeded) {
    EASYJIT_RT_LOG("Function::deserialize: light path used name=%s\n",
                   FunName.c_str());
    return lightFn;
  }
  EASYJIT_RT_LOG("Function::deserialize: light path rejected name=%s reason=%s\n",
                 FunName.c_str(), rep.reason.c_str());
  // Tear down M before its parent LLVMContext goes out of scope.
  M.reset();
  return nullptr;
#else
  return CompileAndWrap(FunName.c_str(), Globals, std::move(Ctx), std::move(M));
#endif // EASYJIT_LIGHT_BACKEND_ONLY
}

bool Function::operator==(easy::Function const& other) const {
  return this->Holder->getModule() == other.Holder->getModule();
}

std::hash<easy::Function>::result_type
std::hash<easy::Function>::operator()(argument_type const& F) const noexcept {
  return std::hash<llvm::Module*>{}(F.Holder->getModule());
}
