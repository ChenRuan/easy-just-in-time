#include <llvm/Support/TargetSelect.h>

#include <llvm/LinkAllIR.h>
#include <llvm/LinkAllPasses.h>
#include <llvm/ExecutionEngine/MCJIT.h>

using namespace llvm;

namespace {
class InitNativeTarget {
  public:
  InitNativeTarget() {
#if defined(_X86)
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
#elif defined(__aarch64__)
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmPrinter();
#endif

    sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
  }
} Init;
}
