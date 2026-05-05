// test_stack_args.cpp — round-8j: validate AAPCS64 stack-passed scalar
// argument support in the light AArch64 emitter.
//
// What it proves:
//
//   1. The 9th and later GPR-class scalar arguments (i32, i64, pointer)
//      are read from the caller's NSAA (stack overflow area) — slot is
//      8-byte aligned and 8 bytes wide for our scalar subset.
//
//   2. The 9th and later FP-class scalar arguments (float, double) are
//      similarly read from the same shared overflow area in original
//      parameter order (FP and GPR overflow share the area; per
//      AAPCS64 §6.4 NSAA they are not separate offset spaces).
//
//   3. Stack args correctly co-exist with a non-trivial local frame:
//      preload offset = `frameSize + incomingOff`, computed AFTER
//      `sub sp, sp, #frameSize`.
//
//   4. Stack-passed pointers can drive load/store and arithmetic just
//      like in-register pointer args.
//
// Execution:
//
//   - On an AArch64 host we mmap+execute the emitted bytes and call
//     them via the proper C function-pointer types so the host C++
//     compiler arranges the AAPCS64 stack layout for us. This is the
//     only way to validate the layout end-to-end without writing a
//     separate caller stub.
//   - On non-AArch64 hosts only `light::emit` correctness is checked
//     (status=Ok + non-zero byte stream).

#include "light_aarch64.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(__aarch64__) || defined(__arm64__)
#  define LIGHT_STACK_HOST_AARCH64 1
#  include <sys/mman.h>
#else
#  define LIGHT_STACK_HOST_AARCH64 0
#endif

using namespace llvm;

namespace {

std::unique_ptr<Module> NewAArch64Module(LLVMContext &Ctx, const char *Name) {
  auto M = std::make_unique<Module>(Name, Ctx);
  M->setTargetTriple("aarch64-unknown-linux-gnu");
  M->setDataLayout("e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128");
  return M;
}

struct EmitOut {
  ::light::Status status;
  std::vector<uint8_t> code;
  std::string reason;
};

EmitOut EmitFunction(const Function &F) {
  std::vector<uint8_t> buf(8192);
  ::light::Result r = ::light::emit(F, buf.data(), buf.size(), nullptr, 0);
  EmitOut out;
  out.status = r.status;
  out.reason = r.reason;
  if (r.status == ::light::Status::Ok) {
    out.code.assign(buf.begin(), buf.begin() + r.codeBytes);
  }
  return out;
}

#if LIGHT_STACK_HOST_AARCH64
void *MakeExecutable(const std::vector<uint8_t> &code) {
  void *page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) return nullptr;
  std::memcpy(page, code.data(), code.size());
  __builtin___clear_cache((char *)page, (char *)page + code.size());
  return page;
}
#endif

int CheckEmit(const char *label, const EmitOut &eo) {
  if (eo.status != ::light::Status::Ok) {
    std::printf("  FAIL %s: emit status=%d reason=%s\n",
                label, (int)eo.status, eo.reason.c_str());
    return 1;
  }
  std::printf("  emit %-26s OK (%zu bytes)\n", label, eo.code.size());
  return 0;
}

// ===========================================================================
// IR builders. All functions take >8 args of one or both classes so the
// AAPCS64 NSAA path is exercised by the host C++ caller as well as the
// light emitter.
// ===========================================================================

// long long ninth_i64(i64 a0..a7, i64 a8) { return a8 + 7; }
// First 8 args go in x0..x7 (in-reg). a8 spills onto the stack at SP+0
// (entry SP). The function body forces the emitter to actually USE the
// stack-loaded value — without that, an early-out optimization could
// hide a missing preload.
Function *BuildNinthI64(Module &M) {
  LLVMContext &C = M.getContext();
  Type *I64 = Type::getInt64Ty(C);
  std::vector<Type *> ats(9, I64);
  FunctionType *FT = FunctionType::get(I64, ats, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 "ninth_i64", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *r = B.CreateAdd(F->getArg(8), ConstantInt::get(I64, 7));
  B.CreateRet(r);
  return F;
}

// int ninth_i32(i32 a0..a7, i32 a8) { return a8 - a0; }
// Stresses i32 stack arg loaded with LDR Wt, [sp, #off] using only the
// low 4 bytes of the 8-byte NSAA slot.
Function *BuildNinthI32(Module &M) {
  LLVMContext &C = M.getContext();
  Type *I32 = Type::getInt32Ty(C);
  std::vector<Type *> ats(9, I32);
  FunctionType *FT = FunctionType::get(I32, ats, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 "ninth_i32", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *r = B.CreateSub(F->getArg(8), F->getArg(0));
  B.CreateRet(r);
  return F;
}

// int load_from_ninth_ptr(i32 a0..a7, i32 *p) { return *p + 100; }
// Stack-passed pointer must work as a base for a load (drives the
// pointer-tracking path: ptrLoc[&A] = InReg{regOf[&A]}).
Function *BuildLoadFromNinthPtr(Module &M) {
  LLVMContext &C = M.getContext();
  Type *I32 = Type::getInt32Ty(C);
  Type *I32P = PointerType::getUnqual(I32);
  std::vector<Type *> ats;
  for (int i = 0; i < 8; ++i) ats.push_back(I32);
  ats.push_back(I32P);
  FunctionType *FT = FunctionType::get(I32, ats, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 "load_from_ninth_ptr", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *ld = B.CreateAlignedLoad(I32, F->getArg(8), Align(4));
  Value *r = B.CreateAdd(ld, ConstantInt::get(I32, 100));
  B.CreateRet(r);
  return F;
}

// float ninth_float(float f0..f7, float f8) { return f8 + 1.5f; }
Function *BuildNinthFloat(Module &M) {
  LLVMContext &C = M.getContext();
  Type *F32 = Type::getFloatTy(C);
  std::vector<Type *> ats(9, F32);
  FunctionType *FT = FunctionType::get(F32, ats, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 "ninth_float", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *r = B.CreateFAdd(F->getArg(8), ConstantFP::get(F32, 1.5));
  B.CreateRet(r);
  return F;
}

// double ninth_double(double d0..d7, double d8) { return d8 * 2.0; }
Function *BuildNinthDouble(Module &M) {
  LLVMContext &C = M.getContext();
  Type *F64 = Type::getDoubleTy(C);
  std::vector<Type *> ats(9, F64);
  FunctionType *FT = FunctionType::get(F64, ats, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 "ninth_double", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *r = B.CreateFMul(F->getArg(8), ConstantFP::get(F64, 2.0));
  B.CreateRet(r);
  return F;
}

// double mixed(i64 i0..i7, double d0..d7, i64 i8, double d8, i64 i9, double d9)
//   return (double)i8 + d8 + (double)i9 + d9;
//
// This catches the bug where GPR-overflow and FP-overflow each start
// their stack slot at offset 0 — they must SHARE the overflow area in
// original parameter order: i8 at +0, d8 at +8, i9 at +16, d9 at +24.
Function *BuildMixedOverflow(Module &M) {
  LLVMContext &C = M.getContext();
  Type *I64 = Type::getInt64Ty(C);
  Type *F64 = Type::getDoubleTy(C);
  std::vector<Type *> ats;
  for (int i = 0; i < 8; ++i) ats.push_back(I64);   // i0..i7 -> x0..x7
  for (int i = 0; i < 8; ++i) ats.push_back(F64);   // d0..d7 -> d0..d7
  ats.push_back(I64);   // i8 -> stack
  ats.push_back(F64);   // d8 -> stack
  ats.push_back(I64);   // i9 -> stack
  ats.push_back(F64);   // d9 -> stack
  FunctionType *FT = FunctionType::get(F64, ats, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 "mixed_overflow", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *i8d = B.CreateSIToFP(F->getArg(16), F64);
  Value *d8  = F->getArg(17);
  Value *i9d = B.CreateSIToFP(F->getArg(18), F64);
  Value *d9  = F->getArg(19);
  Value *r1 = B.CreateFAdd(i8d, d8);
  Value *r2 = B.CreateFAdd(r1, i9d);
  Value *r3 = B.CreateFAdd(r2, d9);
  B.CreateRet(r3);
  return F;
}

// long long frame_plus_stack(i64 a0..a7, i64 a8) {
//   long long t = 42;
//   return a8 + t;          // alloca → frameSize > 0; stack arg at SP+frameSize.
// }
Function *BuildFramePlusStack(Module &M) {
  LLVMContext &C = M.getContext();
  Type *I64 = Type::getInt64Ty(C);
  std::vector<Type *> ats(9, I64);
  FunctionType *FT = FunctionType::get(I64, ats, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 "frame_plus_stack", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *slot = B.CreateAlloca(I64, nullptr, "slot");
  B.CreateAlignedStore(ConstantInt::get(I64, 42), slot, Align(8));
  Value *t  = B.CreateAlignedLoad(I64, slot, Align(8));
  Value *r  = B.CreateAdd(F->getArg(8), t);
  B.CreateRet(r);
  return F;
}

} // namespace

// =============================== Reference =================================

namespace {
inline bool feq(float x, float y) {
  float diff = std::fabs(x - y);
  float scale = std::fmax(1.0f, std::fmax(std::fabs(x), std::fabs(y)));
  return diff <= 1e-5f * scale;
}
inline bool deq(double x, double y) {
  double diff = std::fabs(x - y);
  double scale = std::fmax(1.0, std::fmax(std::fabs(x), std::fabs(y)));
  return diff <= 1e-12 * scale;
}
} // namespace

// =============================== Run loop ==================================

int main() {
  LLVMContext Ctx;
  auto M = NewAArch64Module(Ctx, "light_stack_args");

  Function *FNinthI64 = BuildNinthI64(*M);
  Function *FNinthI32 = BuildNinthI32(*M);
  Function *FLoadPtr  = BuildLoadFromNinthPtr(*M);
  Function *FNinthF   = BuildNinthFloat(*M);
  Function *FNinthD   = BuildNinthDouble(*M);
  Function *FMixed    = BuildMixedOverflow(*M);
  Function *FFrame    = BuildFramePlusStack(*M);

  EmitOut eNinthI64 = EmitFunction(*FNinthI64);
  EmitOut eNinthI32 = EmitFunction(*FNinthI32);
  EmitOut eLoadPtr  = EmitFunction(*FLoadPtr);
  EmitOut eNinthF   = EmitFunction(*FNinthF);
  EmitOut eNinthD   = EmitFunction(*FNinthD);
  EmitOut eMixed    = EmitFunction(*FMixed);
  EmitOut eFrame    = EmitFunction(*FFrame);

  int failures = 0;
  failures += CheckEmit("ninth_i64",            eNinthI64);
  failures += CheckEmit("ninth_i32",            eNinthI32);
  failures += CheckEmit("load_from_ninth_ptr",  eLoadPtr);
  failures += CheckEmit("ninth_float",          eNinthF);
  failures += CheckEmit("ninth_double",         eNinthD);
  failures += CheckEmit("mixed_overflow",       eMixed);
  failures += CheckEmit("frame_plus_stack",     eFrame);

  if (failures) {
    std::printf("FAIL: emit stage (%d failures)\n", failures);
    return 1;
  }

#if LIGHT_STACK_HOST_AARCH64
  using FnNinthI64 = long long (*)(long long, long long, long long, long long,
                                   long long, long long, long long, long long,
                                   long long);
  using FnNinthI32 = int (*)(int, int, int, int, int, int, int, int, int);
  using FnLoadPtr  = int (*)(int, int, int, int, int, int, int, int, int *);
  using FnNinthF   = float (*)(float, float, float, float, float, float, float,
                               float, float);
  using FnNinthD   = double (*)(double, double, double, double, double, double,
                                double, double, double);
  using FnMixed    = double (*)(long long, long long, long long, long long,
                                long long, long long, long long, long long,
                                double, double, double, double, double, double,
                                double, double,
                                long long, double, long long, double);
  using FnFrame    = long long (*)(long long, long long, long long, long long,
                                   long long, long long, long long, long long,
                                   long long);

  auto fnNinthI64 = (FnNinthI64)MakeExecutable(eNinthI64.code);
  auto fnNinthI32 = (FnNinthI32)MakeExecutable(eNinthI32.code);
  auto fnLoadPtr  = (FnLoadPtr) MakeExecutable(eLoadPtr.code);
  auto fnNinthF   = (FnNinthF)  MakeExecutable(eNinthF.code);
  auto fnNinthD   = (FnNinthD)  MakeExecutable(eNinthD.code);
  auto fnMixed    = (FnMixed)   MakeExecutable(eMixed.code);
  auto fnFrame    = (FnFrame)   MakeExecutable(eFrame.code);
  if (!fnNinthI64 || !fnNinthI32 || !fnLoadPtr || !fnNinthF || !fnNinthD ||
      !fnMixed || !fnFrame) {
    std::printf("FAIL: mmap/exec setup\n");
    return 1;
  }

  // ninth_i64
  {
    long long got = fnNinthI64(0, 1, 2, 3, 4, 5, 6, 7, 1000);
    long long want = 1007;
    bool ok = (got == want);
    std::printf("  ninth_i64(...,1000) = %lld want %lld %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
    // Negative stack arg.
    got  = fnNinthI64(0, 0, 0, 0, 0, 0, 0, 0, -50);
    want = -43;
    ok = (got == want);
    std::printf("  ninth_i64(...,-50)  = %lld want %lld %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  }

  // ninth_i32 — exercises i32 stack arg (low 4 bytes of 8B slot).
  {
    int got  = fnNinthI32(10, 0, 0, 0, 0, 0, 0, 0, 100);
    int want = 90;
    bool ok = (got == want);
    std::printf("  ninth_i32(10,...,100) = %d want %d %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
    // i32 stack arg with sign-extended bit pattern via host's standard call.
    got  = fnNinthI32(0, 0, 0, 0, 0, 0, 0, 0, -7);
    want = -7;
    ok = (got == want);
    std::printf("  ninth_i32(0,...,-7)   = %d want %d %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  }

  // load_from_ninth_ptr — pointer stack arg used as load base.
  {
    int  buf  = 1234;
    int  got  = fnLoadPtr(0, 0, 0, 0, 0, 0, 0, 0, &buf);
    int  want = 1334;
    bool ok = (got == want);
    std::printf("  load_from_ninth_ptr(*p=%d) = %d want %d %s\n",
                buf, got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  }

  // ninth_float
  {
    float got  = fnNinthF(0, 0, 0, 0, 0, 0, 0, 0, 2.25f);
    float want = 3.75f;
    bool ok = feq(got, want);
    std::printf("  ninth_float(...,2.25) = %.6f want %.6f %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  }

  // ninth_double
  {
    double got  = fnNinthD(0, 0, 0, 0, 0, 0, 0, 0, 3.5);
    double want = 7.0;
    bool ok = deq(got, want);
    std::printf("  ninth_double(...,3.5) = %.12f want %.12f %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  }

  // mixed_overflow — GPR + FP overflow share NSAA in original order.
  {
    double got = fnMixed(0, 0, 0, 0, 0, 0, 0, 0,
                         0, 0, 0, 0, 0, 0, 0, 0,
                         /*i8*/ 100, /*d8*/ 0.5,
                         /*i9*/ 200, /*d9*/ 0.25);
    double want = 100.0 + 0.5 + 200.0 + 0.25;
    bool ok = deq(got, want);
    std::printf("  mixed_overflow(...) = %.6f want %.6f %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
    // Re-run with different values to make sure each slot is loaded
    // independently (regression for "all reads come from the same offset").
    got = fnMixed(0, 0, 0, 0, 0, 0, 0, 0,
                  0, 0, 0, 0, 0, 0, 0, 0,
                  -1, -2.0, -3, -4.0);
    want = -1.0 + -2.0 + -3.0 + -4.0;
    ok = deq(got, want);
    std::printf("  mixed_overflow(neg) = %.6f want %.6f %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  }

  // frame_plus_stack — alloca + stack arg in same function.
  {
    long long got  = fnFrame(0, 0, 0, 0, 0, 0, 0, 0, 1000);
    long long want = 1042;
    bool ok = (got == want);
    std::printf("  frame_plus_stack(...,1000) = %lld want %lld %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  }

  if (failures) {
    std::printf("FAILED: %d case(s)\n", failures);
    return 1;
  }
  std::printf("PASS: light stack args (i64 + i32 + ptr + float + double "
              "+ mixed overflow + frame_plus_stack)\n");
  return 0;
#else
  std::printf("PASS (emit-only): non-AArch64 host, execution skipped\n");
  return 0;
#endif
}
