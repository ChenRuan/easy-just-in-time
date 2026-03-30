#include <easy/runtime/BitcodeTracker.h>
#include "internal/BitcodeTrackerInternal.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include <easy/exceptions.h>

using namespace easy;
using namespace llvm;

namespace easy {
  DefineEasyException(BitcodeNotRegistered, "Cannot find bitcode.");
  DefineEasyException(BitcodeParseError, "Cannot parse bitcode for: ");
}

BitcodeTracker& BitcodeTracker::GetTracker() {
  static BitcodeTracker TheTracker;
  return TheTracker;
}

bool BitcodeTracker::hasGlobalMapping(void* FPtr) const {
  auto InfoPtr = Functions.find(FPtr);
  return InfoPtr != Functions.end();
}

LayoutInfo const & BitcodeTracker::getLayoutInfo(easy::layout_id id) const {
  auto InfoPair = Layouts.find(id);
  assert(InfoPair != Layouts.end());
  return InfoPair->second;
}

void* BitcodeTracker::getAddress(std::string const &Name) {
  auto Addr = NameToAddress.find(Name);
  if(Addr == NameToAddress.end())
    return nullptr;
  return Addr->second;
}

std::tuple<const char*, GlobalMapping*> BitcodeTracker::getNameAndGlobalMapping(void* FPtr) {
  auto InfoPtr = Functions.find(FPtr);
  if(InfoPtr == Functions.end()) {
    throw easy::BitcodeNotRegistered();
  }

  return std::make_tuple(InfoPtr->second.Name, InfoPtr->second.Globals);
}

// Free functions for LLVM-dependent module retrieval
std::unique_ptr<llvm::Module> easy::BT_getModuleWithContext(BitcodeTracker& BT, void* FPtr, llvm::LLVMContext &C) {
  auto const* Info = BT.getFunctionInfo(FPtr);
  if(!Info) {
    throw easy::BitcodeNotRegistered();
  }

  llvm::StringRef BytecodeStr(Info->Bitcode, Info->BitcodeLen);
  std::unique_ptr<llvm::MemoryBuffer> Buf(llvm::MemoryBuffer::getMemBuffer(BytecodeStr));
  auto ModuleOrErr =
      llvm::parseBitcodeFile(Buf->getMemBufferRef(), C);

  if (ModuleOrErr.takeError()) {
    throw easy::BitcodeParseError(Info->Name);
  }

  return std::move(ModuleOrErr.get());
}

easy::ModuleContextPair easy::BT_getModule(BitcodeTracker& BT, void* FPtr) {
  std::unique_ptr<llvm::LLVMContext> Context(new llvm::LLVMContext());
  auto Module = easy::BT_getModuleWithContext(BT, FPtr, *Context);
  return ModuleContextPair(std::move(Module), std::move(Context));
}

// function to interface with the generated code
extern "C" {
void easy_register(void* FPtr, const char* Name, GlobalMapping* Globals, const char* Bitcode, size_t BitcodeLen) {
  BitcodeTracker::GetTracker().registerFunction(FPtr, Name, Globals, Bitcode, BitcodeLen);
}
void easy_register_layout(layout_id Id, size_t N) {
  BitcodeTracker::GetTracker().registerLayout(Id, N);
}
}

