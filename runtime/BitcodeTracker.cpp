#include <easy/runtime/BitcodeTracker.h>

#include "SreDebugLog.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Support/raw_ostream.h>

#include <easy/exceptions.h>

using namespace easy;
using namespace llvm;

namespace easy {
  DefineEasyException(BitcodeNotRegistered, "Cannot find bitcode.");
  DefineEasyException(BitcodeParseError, "Cannot parse bitcode for: ");
}

BitcodeTracker& BitcodeTracker::GetTracker() {
  EASYJIT_SRE_LOG("[tracker] GetTracker\n");
  static BitcodeTracker TheTracker;
  return TheTracker;
}

bool BitcodeTracker::hasGlobalMapping(void* FPtr) const {
  EASYJIT_SRE_LOG("[tracker] hasGlobalMapping: fptr=%p\n", FPtr);
  auto InfoPtr = Functions.find(FPtr);
  bool Found = InfoPtr != Functions.end();
  EASYJIT_SRE_LOG("[tracker] hasGlobalMapping: fptr=%p found=%d\n", FPtr, (int)Found);
  return Found;
}

LayoutInfo const & BitcodeTracker::getLayoutInfo(easy::layout_id id) const {
  EASYJIT_SRE_LOG("[tracker] getLayoutInfo: id=%p\n", id);
  auto InfoPair = Layouts.find(id);
  assert(InfoPair != Layouts.end());
  EASYJIT_SRE_LOG("[tracker] getLayoutInfo: id=%p fields=%zu\n",
                  id, InfoPair->second.NumFields);
  return InfoPair->second;
}

void* BitcodeTracker::getAddress(std::string const &Name) {
  EASYJIT_SRE_LOG("[tracker] getAddress: name=%s\n", Name.c_str());
  auto Addr = NameToAddress.find(Name);
  if(Addr == NameToAddress.end()) {
    EASYJIT_SRE_LOG("[tracker] getAddress: name=%s not found\n", Name.c_str());
    return nullptr;
  }
  EASYJIT_SRE_LOG("[tracker] getAddress: name=%s addr=%p\n", Name.c_str(), Addr->second);
  return Addr->second;
}

std::tuple<const char*, GlobalMapping*> BitcodeTracker::getNameAndGlobalMapping(void* FPtr) {
  EASYJIT_SRE_LOG("[tracker] getNameAndGlobalMapping: fptr=%p\n", FPtr);
  auto InfoPtr = Functions.find(FPtr);
  if(InfoPtr == Functions.end()) {
    EASYJIT_SRE_LOG("[tracker] getNameAndGlobalMapping: missing fptr=%p\n", FPtr);
    throw easy::BitcodeNotRegistered();
  }

  EASYJIT_SRE_LOG("[tracker] getNameAndGlobalMapping: fptr=%p name=%s globals=%p bitcode=%p len=%zu\n",
                  FPtr,
                  InfoPtr->second.Name ? InfoPtr->second.Name : "<null>",
                  (void*)InfoPtr->second.Globals,
                  (const void*)InfoPtr->second.Bitcode,
                  InfoPtr->second.BitcodeLen);
  return std::make_tuple(InfoPtr->second.Name, InfoPtr->second.Globals);
}

std::unique_ptr<llvm::Module> BitcodeTracker::getModuleWithContext(void* FPtr, llvm::LLVMContext &C) {
  EASYJIT_SRE_LOG("[tracker] getModuleWithContext: fptr=%p ctx=%p\n", FPtr, (void*)&C);
  auto InfoPtr = Functions.find(FPtr);
  if(InfoPtr == Functions.end()) {
    EASYJIT_SRE_LOG("[tracker] getModuleWithContext: missing fptr=%p\n", FPtr);
    throw easy::BitcodeNotRegistered();
  }

  auto &Info = InfoPtr->second;
  EASYJIT_SRE_LOG("[tracker] getModuleWithContext: name=%s bitcode=%p len=%zu\n",
                  Info.Name ? Info.Name : "<null>", (const void*)Info.Bitcode,
                  Info.BitcodeLen);

  llvm::StringRef BytecodeStr(Info.Bitcode, Info.BitcodeLen);
  std::unique_ptr<llvm::MemoryBuffer> Buf(llvm::MemoryBuffer::getMemBuffer(BytecodeStr));
  auto ModuleOrErr =
      llvm::parseBitcodeFile(Buf->getMemBufferRef(), C);

  if (ModuleOrErr.takeError()) {
    EASYJIT_SRE_LOG("[tracker] getModuleWithContext: parse failed name=%s\n",
                    Info.Name ? Info.Name : "<null>");
    throw easy::BitcodeParseError(Info.Name);
  }

  auto M = std::move(ModuleOrErr.get());
  EASYJIT_SRE_LOG("[tracker] getModuleWithContext: parsed module=%p triple=%s dl=%s\n",
                  (void*)M.get(), M->getTargetTriple().c_str(),
                  M->getDataLayoutStr().c_str());
  return M;
}

BitcodeTracker::ModuleContextPair BitcodeTracker::getModule(void* FPtr) {

  EASYJIT_SRE_LOG("[tracker] getModule: fptr=%p\n", FPtr);
  std::unique_ptr<llvm::LLVMContext> Context(new llvm::LLVMContext());
  auto Module = getModuleWithContext(FPtr, *Context);
  EASYJIT_SRE_LOG("[tracker] getModule: module=%p ctx=%p\n",
                  (void*)Module.get(), (void*)Context.get());
  return ModuleContextPair(std::move(Module), std::move(Context));
}

// function to interface with the generated code
extern "C" {
using easyjit_register_module_fn_t = void (*)();

extern easyjit_register_module_fn_t __start_easyjit_reg_fns[]
    __attribute__((weak));
extern easyjit_register_module_fn_t __stop_easyjit_reg_fns[]
    __attribute__((weak));

void easyjit_register_module_range(void *Start, void *Stop) {
  auto *Begin = static_cast<easyjit_register_module_fn_t *>(Start);
  auto *End = static_cast<easyjit_register_module_fn_t *>(Stop);
  EASYJIT_SRE_LOG("[tracker] easyjit_register_module_range: begin start=%p stop=%p\n",
                  Start, Stop);
  if (!Begin || !End || Begin > End) {
    EASYJIT_SRE_LOG("[tracker] easyjit_register_module_range: no linker-set entries\n");
    return;
  }

  size_t Count = 0;
  for (easyjit_register_module_fn_t *It = Begin; It != End; ++It) {
    if (!*It) {
      continue;
    }
    EASYJIT_SRE_LOG("[tracker] easyjit_register_module_range: call entry=%p\n",
                    (void *)*It);
    (*It)();
    ++Count;
  }
  EASYJIT_SRE_LOG("[tracker] easyjit_register_module_range: done entries=%zu\n",
                  Count);
}

void easyjit_register_module(void) {
  easyjit_register_module_range((void *)__start_easyjit_reg_fns,
                                (void *)__stop_easyjit_reg_fns);
}

void easy_register(void* FPtr, const char* Name, GlobalMapping* Globals, const char* Bitcode, size_t BitcodeLen) {
  EASYJIT_SRE_LOG("[tracker] easy_register: fptr=%p name=%s globals=%p bitcode=%p len=%zu\n",
                  FPtr, Name ? Name : "<null>", (void*)Globals,
                  (const void*)Bitcode, BitcodeLen);
  BitcodeTracker::GetTracker().registerFunction(FPtr, Name, Globals, Bitcode, BitcodeLen);
  EASYJIT_SRE_LOG("[tracker] easy_register: done name=%s\n", Name ? Name : "<null>");
}
void easy_register_layout(layout_id Id, size_t N) {
  EASYJIT_SRE_LOG("[tracker] easy_register_layout: id=%p fields=%zu\n", Id, N);
  BitcodeTracker::GetTracker().registerLayout(Id, N);
  EASYJIT_SRE_LOG("[tracker] easy_register_layout: done id=%p\n", Id);
}
}
