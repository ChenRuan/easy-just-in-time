#ifndef RUNTIME_PASSES
#define RUNTIME_PASSES

#include<llvm/Pass.h>
#include<llvm/IR/PassManager.h>
#include<llvm/ADT/StringRef.h>
#include<easy/runtime/Context.h>

namespace easy {

  struct ContextAnalysisResult {
    private:
    easy::Context const *C;
    public:
    explicit ContextAnalysisResult(easy::Context const &C);
    ContextAnalysisResult();

    easy::Context const &getContext() const { return *C; }
    bool invalidate(llvm::Module &M, const llvm::PreservedAnalyses &PA, 
                    llvm::ModuleAnalysisManager::Invalidator &Inv) { return false; }
  };

  class ContextAnalysisPass : public llvm::AnalysisInfoMixin<ContextAnalysisPass> {
    friend llvm::AnalysisInfoMixin<ContextAnalysisPass>;
    static llvm::AnalysisKey Key;

    public:
    explicit ContextAnalysisPass(easy::Context const &C);
    ContextAnalysisPass();
    using Result = ContextAnalysisResult;
    Result run(llvm::Module &M, llvm::ModuleAnalysisManager &MAM);
      
    private:
    Result Result_;
  };


  class InlineParametersPass : public llvm::PassInfoMixin<InlineParametersPass> {
    public:
      explicit InlineParametersPass(llvm::StringRef TargetName);
      InlineParametersPass();
      llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &MAM);
    private:
      llvm::StringRef TargetName_;
  };
  
  class DevirtualizeConstantPass : public llvm::PassInfoMixin<DevirtualizeConstantPass> {
    public:
      explicit DevirtualizeConstantPass(llvm::StringRef TargetName);
      DevirtualizeConstantPass();
      llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &FAM);
    private:
      llvm::StringRef TargetName_;
  };

}

#endif
