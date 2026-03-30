#ifndef BITCODETRACKER
#define BITCODETRACKER

#include <easy/param.h>

#include <unordered_map>
#include <memory>
#include <tuple>
#include <string>
#include <cstddef>

namespace easy {

struct GlobalMapping {
  const char* Name;
  void* Address;
};

struct FunctionInfo {
  const char* Name;
  GlobalMapping* Globals;
  const char* Bitcode;
  size_t BitcodeLen;

  FunctionInfo(const char* N, GlobalMapping* G, const char* B, size_t BL)
    : Name(N), Globals(G), Bitcode(B), BitcodeLen(BL)
  { }
};

struct LayoutInfo {
  size_t NumFields;
};

class BitcodeTracker {

  // map function to all the info required for jit compilation
  std::unordered_map<void*, FunctionInfo> Functions;
  std::unordered_map<std::string, void*> NameToAddress;

  // map the addresses of the layout_id with the number of parameters
  std::unordered_map<void*, LayoutInfo> Layouts;

  public:

  void registerFunction(void* FPtr, const char* Name, GlobalMapping* Globals, const char* Bitcode, size_t BitcodeLen) {
    Functions.emplace(FPtr, FunctionInfo{Name, Globals, Bitcode, BitcodeLen});
    NameToAddress.emplace(Name, FPtr);
  }

  void registerLayout(layout_id Id, size_t N) {
    Layouts.emplace(Id, LayoutInfo{N});
  }

  void* getAddress(std::string const &Name);
  std::tuple<const char*, GlobalMapping*> getNameAndGlobalMapping(void* FPtr);
  bool hasGlobalMapping(void* FPtr) const;
  LayoutInfo const & getLayoutInfo(easy::layout_id id) const;

  FunctionInfo const* getFunctionInfo(void* FPtr) const {
    auto it = Functions.find(FPtr);
    if (it == Functions.end()) return nullptr;
    return &it->second;
  }

  // get the singleton object
  static BitcodeTracker& GetTracker();
};

}

#endif
