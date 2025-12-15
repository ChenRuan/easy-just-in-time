#include <easy/runtime/RuntimePasses.h>

using namespace llvm;
using namespace easy;

ContextAnalysisResult::ContextAnalysisResult(easy::Context const &C) : C(&C) {}
ContextAnalysisResult::ContextAnalysisResult() : C(nullptr) {}

AnalysisKey ContextAnalysisPass::Key;

ContextAnalysisPass::ContextAnalysisPass(easy::Context const &C) : Result_(C) {}
ContextAnalysisPass::ContextAnalysisPass() : Result_() {}

ContextAnalysisPass::Result ContextAnalysisPass::run(Module &M, ModuleAnalysisManager &MAM) {
  return Result_;
}

