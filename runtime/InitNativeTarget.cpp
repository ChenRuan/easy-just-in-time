#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/DynamicLibrary.h>

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
