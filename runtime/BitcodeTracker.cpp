#include <easy/runtime/BitcodeTracker.h>

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

std::unique_ptr<llvm::Module> BitcodeTracker::getModuleWithContext(void* FPtr, llvm::LLVMContext &C) {
  auto InfoPtr = Functions.find(FPtr);
  if(InfoPtr == Functions.end()) {
    throw easy::BitcodeNotRegistered();
  }

  auto &Info = InfoPtr->second;

  llvm::StringRef BytecodeStr(Info.Bitcode, Info.BitcodeLen);
  std::unique_ptr<llvm::MemoryBuffer> Buf(llvm::MemoryBuffer::getMemBuffer(BytecodeStr));
  auto ModuleOrErr =
      llvm::parseBitcodeFile(Buf->getMemBufferRef(), C);

  if (ModuleOrErr.takeError()) {
    throw easy::BitcodeParseError(Info.Name);
  }

  return std::move(ModuleOrErr.get());
}

BitcodeTracker::ModuleContextPair BitcodeTracker::getModule(void* FPtr) {

  std::unique_ptr<llvm::LLVMContext> Context(new llvm::LLVMContext());
  auto Module = getModuleWithContext(FPtr, *Context);
  return ModuleContextPair(std::move(Module), std::move(Context));
}

// function to interface with the generated code
extern "C" {
using easyjit_register_module_fn_t = void (*)();

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

void easy_register(void* FPtr, const char* Name, GlobalMapping* Globals, const char* Bitcode, size_t BitcodeLen) {
  BitcodeTracker::GetTracker().registerFunction(FPtr, Name, Globals, Bitcode, BitcodeLen);
}
void easy_register_layout(layout_id Id, size_t N) {
  BitcodeTracker::GetTracker().registerLayout(Id, N);
}
}

