#ifndef STATIC_PASSES
#define STATIC_PASSES

#include <llvm/Pass.h>

#include <llvm/IR/PassManager.h>

namespace easy {
  void registerBitcodePass(llvm::ModulePassManager &PM);
  void registerLayoutPass(llvm::ModulePassManager &PM);
}

#endif
