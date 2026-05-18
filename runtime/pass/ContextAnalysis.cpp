#include <easy/runtime/RuntimePasses.h>
#include "../SreDebugLog.h"

using namespace llvm;
using namespace easy;

char easy::ContextAnalysis::ID = 0;

llvm::Pass* easy::createContextAnalysisPass(easy::Context const &C) {
  EASYJIT_SRE_LOG("[pass] createContextAnalysisPass: ctx_size=%zu\n", C.size());
  return new ContextAnalysis(C);
}

static RegisterPass<easy::ContextAnalysis> X("", "", true, true);
