// LightBackend.h — round-10 integration of the lightweight AArch64
// emitter (light_codegen/light_aarch64.*) into the EasyJIT runtime
// compile path (Function::Compile).
//
// This replaces the previous PoC call-graph:
//     user .cpp  ->  standalone driver  ->  parseIRFile(.spec.ll)
//                                       ->  light::compile
// with a direct in-runtime path:
//     easy::jit(...)  ->  Function::Compile  ->  Optimize
//                                            ->  TryLightCompile   (new)
//                                            ->  (fallback) ORC path
//
// See light_codegen/AARCH64_BE.md for the endian model and
// light_codegen/light_aarch64.h for the supported IR subset.
//
// Policy is controlled by the env var EASYJIT_LIGHT:
//   unset / "off"    -> do not try the light path (default, zero risk).
//   "try"  / "auto"  -> try the light path; on Unsupported fall back to ORC.
//   "force"          -> try the light path; on Unsupported fail fast.
//
// TryLightCompile consumes the already-optimized module if it succeeds.
// On failure (or when the policy decides to skip it), it leaves the
// module untouched so the ORC path can run afterwards.
#ifndef EASYJIT_RUNTIME_LIGHT_BACKEND_H
#define EASYJIT_RUNTIME_LIGHT_BACKEND_H

#include <memory>
#include <string>

namespace llvm {
class LLVMContext;
class Module;
} // namespace llvm

namespace easy {

class Function;
struct GlobalMapping;

namespace light_backend {

enum class Policy {
  Off,    // do not attempt light path
  Try,    // attempt; fall back to ORC on Unsupported
  Force,  // attempt; fail fast on Unsupported
};

// Decide policy from the EASYJIT_LIGHT env var. Unknown values map to Off
// (so a typo can never accidentally engage an experimental path).
Policy GetPolicyFromEnv();

// Human-readable name, for logs.
const char *PolicyName(Policy p);

enum class Outcome {
  Succeeded,       // *out is populated; caller returns it.
  SkippedByPolicy, // caller falls back to ORC path.
  Unsupported,     // caller falls back to ORC path (or fails if Force).
  FailedForce,     // policy was Force, light path didn't work -> hard error.
};

struct Report {
  Outcome outcome = Outcome::SkippedByPolicy;
  std::string reason; // filled in on Unsupported / FailedForce
};

// Try to compile Fn (looked up by Name inside M) via the light emitter.
// On success, builds a Function wrapping the mmap'd code page and returns
// it via 'out'. On Unsupported / skipped, 'out' is left untouched.
// 'M' and 'Ctx' are consumed only on Succeeded; on any other outcome the
// caller retains ownership and can proceed to the legacy path.
Report TryLightCompile(const char *Name,
                       GlobalMapping *Globals,
                       std::unique_ptr<llvm::LLVMContext> &Ctx,
                       std::unique_ptr<llvm::Module> &M,
                       std::unique_ptr<easy::Function> &out,
                       Policy policy);

} // namespace light_backend
} // namespace easy

#endif
