#ifndef LLVMHOLDER
#define LLVMHOLDER
namespace llvm { class Module; }
namespace easy {
class LLVMHolder {
  public:
  virtual ~LLVMHolder() = default;
  // Return the LLVM module associated with the compiled function, if any.
  // Holders produced by codegen paths that do not keep a module around
  // (e.g. a raw machine-code mmap page) may return nullptr; callers MUST
  // handle nullptr.
  virtual llvm::Module* getModule() const { return nullptr; }
};
}

#endif
