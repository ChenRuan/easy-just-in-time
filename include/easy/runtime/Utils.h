#ifndef UTILS
#define UTILS

#include <string>
#include <memory>

namespace easy {

// Internal function name retrieval — used only by libeasyjit_llvm internals.
// Declared here for convenience but requires LLVM headers to call.
std::string GetEntryFunctionNameStr(void const* ModulePtr);

}

#endif
