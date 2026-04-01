#include <llvm/Support/TargetSelect.h>

#include <llvm/LinkAllIR.h>
#include <llvm/LinkAllPasses.h>
#include <llvm/ExecutionEngine/MCJIT.h>

using namespace llvm;

namespace {
class InitNativeTarget {
  public:
  InitNativeTarget() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
  }
} Init;
}
