#include "StaticPasses.h"

#include <llvm/Support/raw_ostream.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>

#include <llvm/PassRegistry.h>
#include <llvm/IR/PassManager.h>

using namespace llvm;
using namespace easy;

// The old pass manager inserts RegisterBitcodePass after last of optimization.
//@TODO: figure out the insertion point of these passes.
llvm::PassPluginLibraryInfo getEasyJitPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RegisterBitcode", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineEarlySimplificationEPCallback(
                [](llvm::ModulePassManager &PM, llvm::OptimizationLevel) {
                  easy::registerLayoutPass(PM);
                  easy::registerBitcodePass(PM);
                });
          }};
}
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getEasyJitPassPluginInfo();
}