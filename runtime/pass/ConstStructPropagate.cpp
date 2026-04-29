///
/// ConstStructPropagate.cpp
///
/// A lightweight, targeted pass for EasyJIT embedded deployment.
///
/// After InlineParameters + inlining, the IR typically contains:
///   %alloca = alloca %StructTy
///   store <const>, GEP(%alloca, 0, field0)
///   store <const>, GEP(%alloca, 0, field1)
///   ...
///   %v = load GEP(%alloca, 0, fieldN)
///   br i1 (icmp %v, ...), ...
///
/// This pass directly propagates known-constant store values to
/// matching loads, folds constant branches, removes unreachable
/// blocks, and cleans up dead instructions — all without
/// depending on SROA, SCCP, InstCombine, or other heavy passes.
///
/// Design goals:
///   - Minimal code footprint (no new LLVM pass dependencies)
///   - Handles the specific alloca→store→GEP→load→branch pattern
///   - Iterates until fixpoint
///   - Safe: only propagates when the alloca is fully constant-initialized
///

#include <easy/runtime/RuntimePasses.h>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/CFG.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/Transforms/Utils/Local.h>

#include <cstdio>

using namespace llvm;

#ifndef EASYJIT_RUNTIME_DEBUG
#define EASYJIT_RUNTIME_DEBUG 0
#endif

#if EASYJIT_RUNTIME_DEBUG
#define CSP_LOG(...) do { std::fprintf(stderr, "[easyjit][csp] " __VA_ARGS__); std::fflush(stderr); } while(0)
#else
#define CSP_LOG(...) do {} while(0)
#endif

char easy::ConstStructPropagate::ID = 0;

llvm::Pass* easy::createConstStructPropagatePass(llvm::StringRef Name) {
  return new ConstStructPropagate(Name);
}

namespace {

/// Normalize a GEP chain into a single byte-offset from a base alloca.
/// Returns true if we could resolve to (Alloca, ByteOffset).
static bool resolveGEPToAllocaOffset(Value *Ptr, const DataLayout &DL,
                                     AllocaInst *&OutAlloca,
                                     int64_t &OutOffset) {
  int64_t Offset = 0;
  Value *Base = Ptr;

  // Walk through GEPs and bitcasts
  while (true) {
    if (auto *GEP = dyn_cast<GetElementPtrInst>(Base)) {
      APInt GEPOffset(DL.getPointerSizeInBits(), 0);
      if (!GEP->accumulateConstantOffset(DL, GEPOffset))
        return false;
      Offset += GEPOffset.getSExtValue();
      Base = GEP->getPointerOperand();
    } else if (auto *BC = dyn_cast<BitCastInst>(Base)) {
      Base = BC->getOperand(0);
    } else {
      break;
    }
  }

  auto *AI = dyn_cast<AllocaInst>(Base);
  if (!AI)
    return false;

  OutAlloca = AI;
  OutOffset = Offset;
  return true;
}

/// Represents a constant value stored at a specific byte offset in an alloca.
struct StoredConst {
  int64_t Offset;
  uint64_t SizeInBytes;
  Constant *Value;
  StoreInst *SI;
};

/// Collect all constant stores into an alloca, indexed by byte offset.
/// Returns false if there's any non-constant store or unknown aliasing write.
static bool collectConstStores(AllocaInst *AI, const DataLayout &DL,
                               SmallVectorImpl<StoredConst> &Stores) {
  SmallVector<Value*, 16> Worklist;
  SmallPtrSet<Value*, 16> Visited;
  Worklist.push_back(AI);

  while (!Worklist.empty()) {
    Value *V = Worklist.pop_back_val();
    if (!Visited.insert(V).second)
      continue;

    for (User *U : V->users()) {
      if (auto *GEP = dyn_cast<GetElementPtrInst>(U)) {
        Worklist.push_back(GEP);
      } else if (auto *BC = dyn_cast<BitCastInst>(U)) {
        Worklist.push_back(BC);
      } else if (auto *SI = dyn_cast<StoreInst>(U)) {
        // Must be storing TO the alloca (not storing the alloca's address)
        if (SI->getPointerOperand() != V)
          continue; // storing alloca value somewhere, not a write to alloca

        auto *C = dyn_cast<Constant>(SI->getValueOperand());
        if (!C)
          return false; // non-constant store — bail

        int64_t Off = 0;
        AllocaInst *Base = nullptr;
        if (!resolveGEPToAllocaOffset(SI->getPointerOperand(), DL, Base, Off))
          return false;
        if (Base != AI)
          return false;

        uint64_t Size = DL.getTypeStoreSize(C->getType());
        Stores.push_back({Off, Size, C, SI});
      } else if (auto *LI = dyn_cast<LoadInst>(U)) {
        // Loads are fine — we're trying to replace them
        (void)LI;
      } else if (auto *II = dyn_cast<IntrinsicInst>(U)) {
        // Allow lifetime markers
        auto IID = II->getIntrinsicID();
        if (IID == Intrinsic::lifetime_start ||
            IID == Intrinsic::lifetime_end) {
          continue;
        }
        // memcpy as destination is a write — bail conservatively
        return false;
      } else {
        // Unknown user — not safe
        return false;
      }
    }
  }
  return true;
}

/// Find the constant value stored at a given byte offset+size.
static Constant *findStoredConstAt(int64_t LoadOff, uint64_t LoadSize,
                                   ArrayRef<StoredConst> Stores) {
  for (auto &SC : Stores) {
    if (SC.Offset == LoadOff && SC.SizeInBytes == LoadSize)
      return SC.Value;
  }
  return nullptr;
}

/// Phase 1: Propagate constants from stores to loads through allocas.
static bool propagateAllocaConstants(Function &F) {
  const DataLayout &DL = F.getParent()->getDataLayout();
  bool Changed = false;

  SmallVector<AllocaInst*, 16> Allocas;
  BasicBlock &Entry = F.getEntryBlock();
  for (auto &I : Entry) {
    if (auto *AI = dyn_cast<AllocaInst>(&I))
      Allocas.push_back(AI);
  }

  for (AllocaInst *AI : Allocas) {
    if (!AI->isStaticAlloca())
      continue;

    SmallVector<StoredConst, 32> Stores;
    if (!collectConstStores(AI, DL, Stores))
      continue;

    if (Stores.empty())
      continue;

    CSP_LOG("propagateAllocaConstants: alloca %s has %zu const stores\n",
            AI->getName().str().c_str(), Stores.size());

    // Find all loads from this alloca and try to replace
    SmallVector<Value*, 16> Worklist;
    SmallPtrSet<Value*, 16> Visited;
    Worklist.push_back(AI);

    SmallVector<std::pair<LoadInst*, Constant*>, 16> Replacements;

    while (!Worklist.empty()) {
      Value *V = Worklist.pop_back_val();
      if (!Visited.insert(V).second)
        continue;

      for (User *U : V->users()) {
        if (auto *GEP = dyn_cast<GetElementPtrInst>(U)) {
          Worklist.push_back(GEP);
        } else if (auto *BC = dyn_cast<BitCastInst>(U)) {
          Worklist.push_back(BC);
        } else if (auto *LI = dyn_cast<LoadInst>(U)) {
          int64_t Off = 0;
          AllocaInst *Base = nullptr;
          if (!resolveGEPToAllocaOffset(LI->getPointerOperand(), DL, Base, Off))
            continue;
          if (Base != AI)
            continue;

          uint64_t Size = DL.getTypeStoreSize(LI->getType());
          if (Constant *C = findStoredConstAt(Off, Size, Stores)) {
            if (C->getType() == LI->getType()) {
              Replacements.push_back({LI, C});
            } else if (DL.getTypeStoreSize(C->getType()) == Size) {
              if (auto *BC = ConstantExpr::getBitCast(C, LI->getType()))
                Replacements.push_back({LI, BC});
            }
          }
        }
      }
    }

    for (auto &P : Replacements) {
      CSP_LOG("  replace load at offset -> const\n");
      P.first->replaceAllUsesWith(P.second);
      P.first->eraseFromParent();
      Changed = true;
    }
  }

  return Changed;
}

/// Phase 2: Fold constant conditional branches.
static bool foldConstantBranches(Function &F) {
  bool Changed = false;

  for (BasicBlock &BB : F) {
    auto *BI = dyn_cast<BranchInst>(BB.getTerminator());
    if (!BI || !BI->isConditional())
      continue;

    auto *Cond = dyn_cast<ConstantInt>(BI->getCondition());
    if (!Cond)
      continue;

    BasicBlock *Taken = Cond->isOne() ? BI->getSuccessor(0) : BI->getSuccessor(1);
    BasicBlock *NotTaken = Cond->isOne() ? BI->getSuccessor(1) : BI->getSuccessor(0);

    NotTaken->removePredecessor(&BB, /*KeepOneInputPHIs=*/true);
    BranchInst::Create(Taken, BI);
    BI->eraseFromParent();
    Changed = true;
  }

  return Changed;
}

/// Phase 3: Constant-fold compares.
static bool foldConstantCompares(Function &F) {
  bool Changed = false;
  SmallVector<Instruction*, 32> ToErase;

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    if (auto *Cmp = dyn_cast<ICmpInst>(&*I)) {
      auto *LHS = dyn_cast<Constant>(Cmp->getOperand(0));
      auto *RHS = dyn_cast<Constant>(Cmp->getOperand(1));
      if (LHS && RHS) {
        if (Constant *Result = ConstantExpr::getICmp(Cmp->getPredicate(), LHS, RHS)) {
          Cmp->replaceAllUsesWith(Result);
          ToErase.push_back(Cmp);
          Changed = true;
        }
      }
    } else if (auto *Cmp = dyn_cast<FCmpInst>(&*I)) {
      auto *LHS = dyn_cast<Constant>(Cmp->getOperand(0));
      auto *RHS = dyn_cast<Constant>(Cmp->getOperand(1));
      if (LHS && RHS) {
        if (Constant *Result = ConstantExpr::getFCmp(Cmp->getPredicate(), LHS, RHS)) {
          Cmp->replaceAllUsesWith(Result);
          ToErase.push_back(Cmp);
          Changed = true;
        }
      }
    }
  }

  for (auto *I : ToErase)
    I->eraseFromParent();
  return Changed;
}

/// Phase 4: Fold constant binary ops.
static bool foldConstantArithmetic(Function &F) {
  bool Changed = false;
  SmallVector<Instruction*, 32> ToErase;

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    auto *BO = dyn_cast<BinaryOperator>(&*I);
    if (!BO) continue;

    auto *LHS = dyn_cast<Constant>(BO->getOperand(0));
    auto *RHS = dyn_cast<Constant>(BO->getOperand(1));
    if (!LHS || !RHS) continue;

    if (Constant *Result = ConstantFoldBinaryOpOperands(
            BO->getOpcode(), LHS, RHS, F.getParent()->getDataLayout())) {
      BO->replaceAllUsesWith(Result);
      ToErase.push_back(BO);
      Changed = true;
    }
  }

  for (auto *I : ToErase)
    I->eraseFromParent();
  return Changed;
}

/// Phase 5: Remove trivially dead instructions + dead alloca stores.
static bool removeDeadInstructions(Function &F) {
  bool Changed = false;
  bool Progress = true;

  while (Progress) {
    Progress = false;
    SmallVector<Instruction*, 32> ToErase;

    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      if (isInstructionTriviallyDead(&*I))
        ToErase.push_back(&*I);
    }

    for (auto *I : ToErase) {
      I->eraseFromParent();
      Changed = true;
      Progress = true;
    }
  }

  // Remove allocas that have no remaining loads (only stores/GEPs/lifetime)
  SmallVector<AllocaInst*, 16> Allocas;
  for (auto &I : F.getEntryBlock()) {
    if (auto *AI = dyn_cast<AllocaInst>(&I))
      Allocas.push_back(AI);
  }

  for (AllocaInst *AI : Allocas) {
    // Check if any user (transitively through GEPs/bitcasts) is a load or
    // call (other than lifetime intrinsics)
    SmallVector<Value*, 16> Worklist;
    SmallPtrSet<Value*, 16> Visited;
    Worklist.push_back(AI);
    bool HasLiveUse = false;

    while (!Worklist.empty() && !HasLiveUse) {
      Value *V = Worklist.pop_back_val();
      if (!Visited.insert(V).second) continue;
      for (User *U : V->users()) {
        if (isa<GetElementPtrInst>(U) || isa<BitCastInst>(U)) {
          Worklist.push_back(U);
        } else if (isa<StoreInst>(U)) {
          auto *SI = cast<StoreInst>(U);
          if (SI->getPointerOperand() == V)
            continue; // store TO the alloca — dead if no loads
          HasLiveUse = true; // storing alloca address somewhere
        } else if (auto *II = dyn_cast<IntrinsicInst>(U)) {
          auto IID = II->getIntrinsicID();
          if (IID == Intrinsic::lifetime_start || IID == Intrinsic::lifetime_end)
            continue;
          HasLiveUse = true;
        } else {
          HasLiveUse = true;
        }
      }
    }

    if (!HasLiveUse) {
      // Erase all users (stores, GEPs, bitcasts, lifetime markers)
      SmallVector<Instruction*, 32> ToKill;
      Visited.clear();
      Worklist.clear();
      Worklist.push_back(AI);
      while (!Worklist.empty()) {
        Value *V = Worklist.pop_back_val();
        if (!Visited.insert(V).second) continue;
        for (User *U : V->users()) {
          if (auto *Inst = dyn_cast<Instruction>(U)) {
            if (isa<GetElementPtrInst>(Inst) || isa<BitCastInst>(Inst))
              Worklist.push_back(Inst);
            ToKill.push_back(Inst);
          }
        }
      }
      // Erase in reverse order
      for (auto It = ToKill.rbegin(); It != ToKill.rend(); ++It) {
        (*It)->replaceAllUsesWith(UndefValue::get((*It)->getType()));
        (*It)->eraseFromParent();
      }
      AI->eraseFromParent();
      Changed = true;
    }
  }

  return Changed;
}

/// Phase 6: Remove unreachable blocks.
static bool dropUnreachableBlocks(Function &F) {
  SmallPtrSet<BasicBlock*, 16> Reachable;
  SmallVector<BasicBlock*, 16> Worklist;

  Worklist.push_back(&F.getEntryBlock());
  while (!Worklist.empty()) {
    BasicBlock *BB = Worklist.pop_back_val();
    if (!Reachable.insert(BB).second)
      continue;
    for (BasicBlock *Succ : successors(BB))
      Worklist.push_back(Succ);
  }

  SmallVector<BasicBlock*, 8> Dead;
  for (BasicBlock &BB : F) {
    if (!Reachable.count(&BB))
      Dead.push_back(&BB);
  }

  if (Dead.empty())
    return false;

  for (BasicBlock *BB : Dead) {
    for (BasicBlock *Succ : successors(BB))
      Succ->removePredecessor(BB);
    for (auto &I : *BB)
      I.replaceAllUsesWith(UndefValue::get(I.getType()));
    BB->dropAllReferences();
  }
  for (BasicBlock *BB : Dead)
    BB->eraseFromParent();

  return true;
}

/// Phase 7: Simplify PHI nodes with single/uniform incoming value.
static bool simplifySingleEntryPhis(Function &F) {
  bool Changed = false;
  SmallVector<PHINode*, 16> ToSimplify;

  for (BasicBlock &BB : F) {
    for (auto &I : BB) {
      auto *PN = dyn_cast<PHINode>(&I);
      if (!PN) break;
      if (PN->getNumIncomingValues() == 1) {
        ToSimplify.push_back(PN);
      } else {
        Value *V = PN->getIncomingValue(0);
        bool AllSame = true;
        for (unsigned i = 1; i < PN->getNumIncomingValues(); ++i) {
          if (PN->getIncomingValue(i) != V) { AllSame = false; break; }
        }
        if (AllSame)
          ToSimplify.push_back(PN);
      }
    }
  }

  for (auto *PN : ToSimplify) {
    Value *V = PN->getIncomingValue(0);
    PN->replaceAllUsesWith(V);
    PN->eraseFromParent();
    Changed = true;
  }
  return Changed;
}

/// Phase 8: Fold constant casts and selects.
static bool foldConstantCasts(Function &F) {
  bool Changed = false;
  SmallVector<Instruction*, 16> ToErase;
  const DataLayout &DL = F.getParent()->getDataLayout();

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    if (auto *CI = dyn_cast<CastInst>(&*I)) {
      if (auto *Src = dyn_cast<Constant>(CI->getOperand(0))) {
        if (Constant *Result = ConstantFoldCastOperand(
                CI->getOpcode(), Src, CI->getType(), DL)) {
          CI->replaceAllUsesWith(Result);
          ToErase.push_back(CI);
          Changed = true;
        }
      }
    }

    if (auto *SI = dyn_cast<SelectInst>(&*I)) {
      if (auto *Cond = dyn_cast<ConstantInt>(SI->getCondition())) {
        Value *Replacement = Cond->isOne() ? SI->getTrueValue() : SI->getFalseValue();
        SI->replaceAllUsesWith(Replacement);
        ToErase.push_back(SI);
        Changed = true;
      }
    }
  }

  for (auto *I : ToErase)
    I->eraseFromParent();
  return Changed;
}

} // anonymous namespace

bool easy::ConstStructPropagate::runOnFunction(llvm::Function &F) {
  if (F.getName() != TargetName_)
    return false;

  CSP_LOG("ConstStructPropagate: begin on %s\n", F.getName().str().c_str());

  bool Changed = false;
  for (int Iter = 0; Iter < 20; ++Iter) {
    bool IterChanged = false;

    IterChanged |= propagateAllocaConstants(F);
    IterChanged |= foldConstantCasts(F);
    IterChanged |= foldConstantCompares(F);
    IterChanged |= foldConstantArithmetic(F);
    IterChanged |= foldConstantBranches(F);
    IterChanged |= simplifySingleEntryPhis(F);
    IterChanged |= dropUnreachableBlocks(F);
    IterChanged |= removeDeadInstructions(F);

    if (!IterChanged)
      break;

    Changed = true;
    CSP_LOG("ConstStructPropagate: iteration %d changed\n", Iter);
  }

  CSP_LOG("ConstStructPropagate: end changed=%d\n", Changed);
  return Changed;
}

static RegisterPass<easy::ConstStructPropagate> CSPReg("","",false, false);
