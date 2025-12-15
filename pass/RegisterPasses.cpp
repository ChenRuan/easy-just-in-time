#include "StaticPasses.h"

#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/raw_ostream.h>
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "llvm/PassRegistry.h"

#include <iostream>

using namespace llvm;
using namespace easy;

static void callback(const PassManagerBuilder &,
                     legacy::PassManagerBase &PM) {
  PM.add(easy::createRegisterBitcodePass());
}

RegisterStandardPasses Register(PassManagerBuilder::EP_OptimizerLast, callback);
RegisterStandardPasses RegisterO0(PassManagerBuilder::EP_EnabledOnOptLevel0, callback);

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