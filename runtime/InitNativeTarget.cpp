#include <llvm/Support/TargetSelect.h>

#include <llvm/LinkAllIR.h>
#include <llvm/LinkAllPasses.h>

namespace {
class InitNativeTarget {
  public:
  InitNativeTarget() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
  }
} Init;
}
