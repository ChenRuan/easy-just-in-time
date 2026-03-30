#ifndef BITCODETRACKER_INTERNAL_H
#define BITCODETRACKER_INTERNAL_H

#include <easy/runtime/BitcodeTracker.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <memory>

namespace easy {

// LLVM-dependent methods on BitcodeTracker, implemented in BitcodeTracker.cpp
// These are not exposed in the public header.
using ModuleContextPair = std::pair<std::unique_ptr<llvm::Module>, std::unique_ptr<llvm::LLVMContext>>;

ModuleContextPair BT_getModule(BitcodeTracker& BT, void* FPtr);
std::unique_ptr<llvm::Module> BT_getModuleWithContext(BitcodeTracker& BT, void* FPtr, llvm::LLVMContext &C);

}

#endif
