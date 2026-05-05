// test_fp_ops.cpp — standalone correctness test for the round-8f f32
// capability extension of the light AArch64 emitter, plus the round-8g
// hardening of fcmp deferred-operand lifetime.
//
// What it proves:
//
//   1. Scalar f32 arithmetic — `fadd`, `fsub`, `fmul`, `fdiv` — round-
//      trip an LLVM IR module through `light::emit`, producing AArch64
//      machine code that, when invoked, matches a native-double
//      reference implementation within strict tolerance.
//
//   2. Ordered fcmp (`OLT`, `OGT`) fused into both a conditional branch
//      and a `select` produces the right control flow.
//
//   3. The round-8g fix for deferred fcmp operand lifetime: an FP
//      ConstantFP materialization between the FCmp and its consumer
//      no longer corrupts the FCMP comparison.  See `BuildStaleS31Repro`
//      below: prior to round 8g the FCMP would have compared against
//      the LATER constant (2.0f) instead of the FCmp's own RHS (1.5f).
//
//   4. `ret` of a `ConstantFP` directly (round-8g extension) — covers
//      0.0f (FMOV-imm zero), 2.0f (FMOV-imm 2.0), and 1.5f (MOVZ+MOVK
//      bit-pattern + FMOV S, W).
//
// Execution:
//
//   - On an AArch64 host, the emitted code is mmap+executed and the
//     output checked against a native reference.
//   - On any other host, only the emit step is exercised. Emitter
//     correctness on the byte stream itself is covered by the
//     opcode-mask coverage guards built into each test case (see
//     `assertHasOpcode` for the fadd/fsub/fmul/fdiv/fcmp masks).

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
#  define LIGHT_FP_HOST_AARCH64 1
#  include <sys/mman.h>
#else
#  define LIGHT_FP_HOST_AARCH64 0
#endif

using namespace llvm;

namespace {

// Build an LLVM Module configured for AArch64 LP64 LE.
std::unique_ptr<Module> NewAArch64Module(LLVMContext &Ctx, const char *Name) {
  auto M = std::make_unique<Module>(Name, Ctx);
  M->setTargetTriple("aarch64-unknown-linux-gnu");
  M->setDataLayout("e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128");
  return M;
}

// emit-only helper — returns the byte stream without trying to execute.
struct EmitOut {
  ::light::Status status;
  std::vector<uint8_t> code;
  std::string reason;
};

EmitOut EmitFunction(const Function &F) {
  std::vector<uint8_t> buf(4096);
  ::light::Result r = ::light::emit(F, buf.data(), buf.size(), nullptr, 0);
  EmitOut out;
  out.status = r.status;
  out.reason = r.reason;
  if (r.status == ::light::Status::Ok) {
    out.code.assign(buf.begin(), buf.begin() + r.codeBytes);
  }
  return out;
}

// AArch64 scalar f32 opcode masks (ARM ARM C6.2.79..C6.2.84).
//   FADD S : 0001_1110_0010_mmmmm_001010_nnnnn_ddddd
//   FSUB S : 0001_1110_0010_mmmmm_001110_nnnnn_ddddd
//   FMUL S : 0001_1110_0010_mmmmm_000010_nnnnn_ddddd
//   FDIV S : 0001_1110_0010_mmmmm_000110_nnnnn_ddddd
//   FCMP S : 0001_1110_0010_mmmmm_001000_nnnnn_00000
// Common 14-bit mask: 0xFFE0_FC00 keeps everything except Rm/Rn/Rd
// (FCMP also masks the Rd field, which is fixed to 0 there).
static constexpr uint32_t kMaskFP3   = 0xFFE0FC00u;
static constexpr uint32_t kFAddS_val = 0x1E202800u;
static constexpr uint32_t kFSubS_val = 0x1E203800u;
static constexpr uint32_t kFMulS_val = 0x1E200800u;
static constexpr uint32_t kFDivS_val = 0x1E201800u;

static constexpr uint32_t kMaskFCMP   = 0xFFE0FC1Fu;
static constexpr uint32_t kFCmpS_val  = 0x1E202000u;

bool StreamHasOpcode(const std::vector<uint8_t> &code, uint32_t mask,
                     uint32_t value) {
  if ((code.size() % 4) != 0) return false;
  for (size_t i = 0; i < code.size(); i += 4) {
    uint32_t w = (uint32_t)code[i]
               | ((uint32_t)code[i+1] << 8)
               | ((uint32_t)code[i+2] << 16)
               | ((uint32_t)code[i+3] << 24);
    if ((w & mask) == value) return true;
  }
  return false;
}

// =============================== IR builders ===============================

// float calc(float a, float b) {
//   float s = a + b;
//   float d = s * 2.0f;
//   float q = a / b;
//   return d - q;
// }
// Exercises FADD/FMUL (with ConstantFP fast path)/FDIV/FSUB + ret SSA.
Function *BuildCalc(Module &M) {
  LLVMContext &C = M.getContext();
  Type *F32 = Type::getFloatTy(C);
  FunctionType *FT = FunctionType::get(F32, {F32, F32}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage, "calc", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *a = F->getArg(0);
  Value *b = F->getArg(1);
  Value *s = B.CreateFAdd(a, b, "s");
  Value *d = B.CreateFMul(s, ConstantFP::get(F32, 2.0), "d");
  Value *q = B.CreateFDiv(a, b, "q");
  Value *r = B.CreateFSub(d, q, "r");
  B.CreateRet(r);
  return F;
}

// float clamp(float x, float lo, float hi) {
//   if (x < lo) return lo;
//   if (x > hi) return hi;
//   return x;
// }
// Exercises FCMP_OLT and FCMP_OGT each fused into a cond branch, plus
// `ret <SSA float>` from three different blocks.
Function *BuildClamp(Module &M) {
  LLVMContext &C = M.getContext();
  Type *F32 = Type::getFloatTy(C);
  FunctionType *FT = FunctionType::get(F32, {F32, F32, F32}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage, "clamp", &M);
  BasicBlock *Entry = BasicBlock::Create(C, "entry", F);
  BasicBlock *RetLo = BasicBlock::Create(C, "ret_lo", F);
  BasicBlock *Chk2  = BasicBlock::Create(C, "chk2",   F);
  BasicBlock *RetHi = BasicBlock::Create(C, "ret_hi", F);
  BasicBlock *RetX  = BasicBlock::Create(C, "ret_x",  F);
  Value *x  = F->getArg(0);
  Value *lo = F->getArg(1);
  Value *hi = F->getArg(2);
  IRBuilder<> B(Entry);
  Value *xLtLo = B.CreateFCmpOLT(x, lo, "x_lt_lo");
  B.CreateCondBr(xLtLo, RetLo, Chk2);
  IRBuilder<>(RetLo).CreateRet(lo);
  IRBuilder<> B2(Chk2);
  Value *xGtHi = B2.CreateFCmpOGT(x, hi, "x_gt_hi");
  B2.CreateCondBr(xGtHi, RetHi, RetX);
  IRBuilder<>(RetHi).CreateRet(hi);
  IRBuilder<>(RetX).CreateRet(x);
  return F;
}

// float maxf(float a, float b) { return a > b ? a : b; }
// Exercises fcmp-driven select on float result.
Function *BuildMaxf(Module &M) {
  LLVMContext &C = M.getContext();
  Type *F32 = Type::getFloatTy(C);
  FunctionType *FT = FunctionType::get(F32, {F32, F32}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage, "maxf", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *a = F->getArg(0);
  Value *b = F->getArg(1);
  Value *cmp = B.CreateFCmpOGT(a, b, "cmp");
  Value *sel = B.CreateSelect(cmp, a, b, "sel");
  B.CreateRet(sel);
  return F;
}

// Reproducer for the round-8f stale-S31 bug fixed in round 8g.
//
//   float stale(float x) {
//     %cmp = fcmp olt float %x, 1.5
//     %tmp = fadd float %x, 2.0
//     br i1 %cmp, T, F
//   T: ret %tmp + 100.0f          (via ConstantFP add — exercises s31 again)
//   F: ret %tmp
// }
//
// Before round 8g, the fcmp handler eagerly materialized 1.5f into S31,
// then `%tmp = fadd %x, 2.0` materialized 2.0f into S31 (clobbering 1.5),
// and the deferred FCMP ended up comparing %x against 2.0 instead of
// 1.5.  Reference semantics: stale(1.6) should take the FALSE edge
// (1.6 >= 1.5) and return 1.6 + 2.0 = 3.6.  The buggy emit would have
// taken the TRUE edge (1.6 < 2.0) and returned 1.6 + 2.0 + 100.0 = 103.6.
Function *BuildStaleS31Repro(Module &M) {
  LLVMContext &C = M.getContext();
  Type *F32 = Type::getFloatTy(C);
  FunctionType *FT = FunctionType::get(F32, {F32}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 "stale_repro", &M);
  BasicBlock *Entry = BasicBlock::Create(C, "entry", F);
  BasicBlock *T     = BasicBlock::Create(C, "true",  F);
  BasicBlock *Fblk  = BasicBlock::Create(C, "false", F);
  Value *x = F->getArg(0);
  IRBuilder<> B(Entry);
  // %cmp records pendingFCmp (lhs=%x, rhs=1.5f) without materializing.
  Value *cmp = B.CreateFCmpOLT(x, ConstantFP::get(F32, 1.5), "cmp");
  // %tmp materializes 2.0f into S31 (FMOV-imm fast path) — this MUST
  // NOT corrupt the deferred FCmp's RHS.
  Value *tmp = B.CreateFAdd(x, ConstantFP::get(F32, 2.0), "tmp");
  B.CreateCondBr(cmp, T, Fblk);
  // TRUE path: tmp + 100.0f. 100.0f is not a special FMOV-imm value so
  // it goes through MOVZ+MOVK (0x42C80000) into S31. Returning this
  // path indicates the bug. Reference (1.6f input) returns 3.6f via
  // FALSE path; if we accidentally return 103.6f, FCMP was wrong.
  IRBuilder<> Bt(T);
  Value *plus100 = Bt.CreateFAdd(tmp, ConstantFP::get(F32, 100.0), "tplus");
  Bt.CreateRet(plus100);
  // FALSE path: just ret %tmp.
  IRBuilder<>(Fblk).CreateRet(tmp);
  return F;
}

// float ret_const_zero(void) { return 0.0f; }      // FMOV-imm zero
// float ret_const_two(void)  { return 2.0f; }      // FMOV-imm 2.0
// float ret_const_15(void)   { return 1.5f; }      // MOVZ+MOVK + FMOV S,W
Function *BuildRetConst(Module &M, const char *name, double v) {
  LLVMContext &C = M.getContext();
  Type *F32 = Type::getFloatTy(C);
  FunctionType *FT = FunctionType::get(F32, {}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage, name, &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  B.CreateRet(ConstantFP::get(F32, v));
  return F;
}

// =============================== Reference =================================

float refCalc(float a, float b)  { return (a + b) * 2.0f - a / b; }
float refClamp(float x, float lo, float hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}
float refMaxf(float a, float b) { return a > b ? a : b; }
float refStale(float x) {
  // Mirrors BuildStaleS31Repro semantics.
  float tmp = x + 2.0f;
  if (x < 1.5f) return tmp + 100.0f;
  return tmp;
}

bool feq(float x, float y) {
  float diff = std::fabs(x - y);
  float scale = std::fmax(1.0f, std::fmax(std::fabs(x), std::fabs(y)));
  return diff <= 1e-6f * scale;
}

// =============================== Run loop =================================

#if LIGHT_FP_HOST_AARCH64
void *MakeExecutable(const std::vector<uint8_t> &code) {
  void *page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) return nullptr;
  std::memcpy(page, code.data(), code.size());
  __builtin___clear_cache((char *)page,
                          (char *)page + code.size());
  return page;
}
#endif

int CheckEmit(const char *label, const EmitOut &eo) {
  if (eo.status != ::light::Status::Ok) {
    std::printf("  FAIL %s: emit status=%d reason=%s\n",
                label, (int)eo.status, eo.reason.c_str());
    return 1;
  }
  std::printf("  emit %-22s OK (%zu bytes)\n", label, eo.code.size());
  return 0;
}

} // namespace

int main() {
  LLVMContext Ctx;
  auto M = NewAArch64Module(Ctx, "light_fp_ops");

  Function *Fcalc   = BuildCalc(*M);
  Function *Fclamp  = BuildClamp(*M);
  Function *Fmaxf   = BuildMaxf(*M);
  Function *Fstale  = BuildStaleS31Repro(*M);
  Function *FrZero  = BuildRetConst(*M, "ret_zero", 0.0);
  Function *FrTwo   = BuildRetConst(*M, "ret_two",  2.0);
  Function *FrFifteen = BuildRetConst(*M, "ret_15", 1.5);

  EmitOut eCalc  = EmitFunction(*Fcalc);
  EmitOut eClamp = EmitFunction(*Fclamp);
  EmitOut eMaxf  = EmitFunction(*Fmaxf);
  EmitOut eStale = EmitFunction(*Fstale);
  EmitOut eZero  = EmitFunction(*FrZero);
  EmitOut eTwo   = EmitFunction(*FrTwo);
  EmitOut e15    = EmitFunction(*FrFifteen);

  int failures = 0;
  failures += CheckEmit("calc",         eCalc);
  failures += CheckEmit("clamp",        eClamp);
  failures += CheckEmit("maxf",         eMaxf);
  failures += CheckEmit("stale_repro",  eStale);
  failures += CheckEmit("ret_zero",     eZero);
  failures += CheckEmit("ret_two",      eTwo);
  failures += CheckEmit("ret_15",       e15);

  // Opcode-mask coverage guards — these run on any host, so the
  // emitter byte stream is verified even without an AArch64 CPU.
  auto must = [&](const char *label, bool cond) {
    if (!cond) {
      std::printf("  FAIL coverage: %s missing\n", label);
      ++failures;
    }
  };
  must("FADD-S in calc",  StreamHasOpcode(eCalc.code,  kMaskFP3, kFAddS_val));
  must("FSUB-S in calc",  StreamHasOpcode(eCalc.code,  kMaskFP3, kFSubS_val));
  must("FMUL-S in calc",  StreamHasOpcode(eCalc.code,  kMaskFP3, kFMulS_val));
  must("FDIV-S in calc",  StreamHasOpcode(eCalc.code,  kMaskFP3, kFDivS_val));
  must("FCMP-S in clamp", StreamHasOpcode(eClamp.code, kMaskFCMP, kFCmpS_val));
  must("FCMP-S in maxf",  StreamHasOpcode(eMaxf.code,  kMaskFCMP, kFCmpS_val));
  must("FCMP-S in stale", StreamHasOpcode(eStale.code, kMaskFCMP, kFCmpS_val));

  if (failures) {
    std::printf("FAIL: emit / coverage stage (%d failures)\n", failures);
    return 1;
  }

#if LIGHT_FP_HOST_AARCH64
  // Execute and check correctness on AArch64 hosts.
  using F2 = float(*)(float, float);
  using F3 = float(*)(float, float, float);
  using F1 = float(*)(float);
  using F0 = float(*)();

  F2 calc  = (F2)MakeExecutable(eCalc.code);
  F3 clamp = (F3)MakeExecutable(eClamp.code);
  F2 maxf  = (F2)MakeExecutable(eMaxf.code);
  F1 stale = (F1)MakeExecutable(eStale.code);
  F0 rZero = (F0)MakeExecutable(eZero.code);
  F0 rTwo  = (F0)MakeExecutable(eTwo.code);
  F0 r15   = (F0)MakeExecutable(e15.code);
  if (!calc || !clamp || !maxf || !stale || !rZero || !rTwo || !r15) {
    std::printf("FAIL: mmap/exec setup\n");
    return 1;
  }

  // calc
  {
    std::pair<float, float> in[] = {
      {1.0f, 2.0f}, {3.0f, 4.0f}, {-1.5f, 2.5f},
      {10.0f, -2.0f}, {0.5f, 0.25f}, {-7.0f, -3.0f}};
    for (auto [a, b] : in) {
      float got = calc(a, b), want = refCalc(a, b);
      bool ok = feq(got, want);
      std::printf("  calc(%.4f,%.4f) = %.6f want %.6f %s\n",
                  a, b, got, want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
  }

  // clamp
  {
    struct C { float x, lo, hi; };
    C in[] = {{0.5f, 0.0f, 1.0f}, {-1.0f, 0.0f, 1.0f}, {2.0f, 0.0f, 1.0f},
              {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f}, {-7.5f, -10.0f, -5.0f}};
    for (const auto &c : in) {
      float got  = clamp(c.x, c.lo, c.hi);
      float want = refClamp(c.x, c.lo, c.hi);
      bool ok = feq(got, want);
      std::printf("  clamp(%.4f,%.4f,%.4f) = %.6f want %.6f %s\n",
                  c.x, c.lo, c.hi, got, want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
  }

  // maxf
  {
    std::pair<float, float> in[] = {
      {1.0f, 2.0f}, {3.0f, -4.0f}, {-1.5f, 2.5f},
      {0.0f, 0.0f}, {7.0f, 7.0f}, {-1.0f, -2.0f}};
    for (auto [a, b] : in) {
      float got = maxf(a, b), want = refMaxf(a, b);
      bool ok = feq(got, want);
      std::printf("  maxf(%.4f,%.4f) = %.6f want %.6f %s\n",
                  a, b, got, want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
  }

  // stale_repro — THE bug check. If FCMP was emitted with stale S31,
  // x=1.6f would (wrongly) compare 1.6 < 2.0 = true and return 103.6.
  // Correct path: 1.6 < 1.5 = false → return 3.6.
  {
    float in[] = {1.6f, 0.5f, 1.5f, 5.0f, -2.0f};
    for (float x : in) {
      float got  = stale(x);
      float want = refStale(x);
      bool ok = feq(got, want);
      std::printf("  stale_repro(%.4f) = %.6f want %.6f %s\n",
                  x, got, want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
  }

  // ret_const_*
  {
    struct K { F0 fn; const char *name; float want; };
    K in[] = {{rZero, "ret_zero", 0.0f},
              {rTwo,  "ret_two",  2.0f},
              {r15,   "ret_15",   1.5f}};
    for (const auto &k : in) {
      float got = k.fn();
      bool ok = feq(got, k.want);
      std::printf("  %s() = %.6f want %.6f %s\n",
                  k.name, got, k.want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
  }

  if (failures) {
    std::printf("FAILED: %d case(s)\n", failures);
    return 1;
  }
  std::printf("PASS: light fp ops (calc + clamp + maxf + stale_repro "
              "+ ret_const)\n");
  return 0;
#else
  std::printf("PASS (emit+coverage only): non-AArch64 host, execution "
              "validation skipped\n");
  return 0;
#endif
}
