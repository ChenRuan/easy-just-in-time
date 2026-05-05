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

// AArch64 scalar f64 opcode masks. The double variants flip bit 22
// (type=01 vs single's type=00); same op-slot encoding otherwise.
//   FADD D : 0001_1110_0110_mmmmm_001010_nnnnn_ddddd  -> 0x1E602800
//   FSUB D : 0001_1110_0110_mmmmm_001110_nnnnn_ddddd  -> 0x1E603800
//   FMUL D : 0001_1110_0110_mmmmm_000010_nnnnn_ddddd  -> 0x1E600800
//   FDIV D : 0001_1110_0110_mmmmm_000110_nnnnn_ddddd  -> 0x1E601800
//   FCMP D : 0001_1110_0110_mmmmm_001000_nnnnn_00000  -> 0x1E602000
static constexpr uint32_t kFAddD_val = 0x1E602800u;
static constexpr uint32_t kFSubD_val = 0x1E603800u;
static constexpr uint32_t kFMulD_val = 0x1E600800u;
static constexpr uint32_t kFDivD_val = 0x1E601800u;
static constexpr uint32_t kFCmpD_val = 0x1E602000u;

// FMOV Dd, Xn (used to materialize ConstantFP doubles via x16 bit-
// pattern):  1001_1110_0110_0111_000000_nnnnn_ddddd -> 0x9E670000
// Mask: drop Rn/Rd only.
static constexpr uint32_t kMaskFMovDX  = 0xFFFFFC00u;
static constexpr uint32_t kFMovDX_val  = 0x9E670000u;

// FCVTZS Wd, Dn (fptosi double -> i32):
// 0001_1110_0111_1000_000000_nnnnn_ddddd -> 0x1E780000
static constexpr uint32_t kMaskFcvtzs   = 0xFFFFFC00u;
static constexpr uint32_t kFcvtzsWD_val = 0x1E780000u;

// Round-8i: full FP<->int conversion family. Each instruction shares
// the FP-int conversion bit slot layout, so a single mask `kMaskFcvtzs`
// (drop Rn/Rd only) catches all of them by exact opc/rmode/sf/type
// match.  Distinct masks are introduced where the canonical name in
// the test case is different.
static constexpr uint32_t kFcvtzsXS_val = 0x9E380000u;
static constexpr uint32_t kFcvtzsXD_val = 0x9E780000u;
static constexpr uint32_t kFcvtzuWS_val = 0x1E390000u;
static constexpr uint32_t kFcvtzuWD_val = 0x1E790000u;
static constexpr uint32_t kFcvtzuXS_val = 0x9E390000u;
static constexpr uint32_t kFcvtzuXD_val = 0x9E790000u;
static constexpr uint32_t kScvtfSW_val  = 0x1E220000u;
static constexpr uint32_t kScvtfSX_val  = 0x9E220000u;
static constexpr uint32_t kScvtfDW_val  = 0x1E620000u;
static constexpr uint32_t kScvtfDX_val  = 0x9E620000u;
static constexpr uint32_t kUcvtfSW_val  = 0x1E230000u;
static constexpr uint32_t kUcvtfSX_val  = 0x9E230000u;
static constexpr uint32_t kUcvtfDW_val  = 0x1E630000u;
static constexpr uint32_t kUcvtfDX_val  = 0x9E630000u;
// FCVT precision change uses bits 16:15 (opc) inside the conversion
// family slot; the mask still drops only Rn/Rd.
static constexpr uint32_t kFcvtDS_val   = 0x1E22C000u;
static constexpr uint32_t kFcvtSD_val   = 0x1E624000u;
// FMOV reinterpret (W<->S, X<->D). Mask drops Rn/Rd.
static constexpr uint32_t kFmovWFromS_val = 0x1E260000u;
static constexpr uint32_t kFmovXFromD_val = 0x9E660000u;
static constexpr uint32_t kFmovSFromW_val = 0x1E270000u;
// (kFMovDX_val above already aliases FMOV Dd,Xn for the dret_15 test.)

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

// =========================== Double IR builders ============================
// Round-8h: parallel double-precision coverage. Same shapes as the
// float tests but with f64 operand/result types so that emit picks the
// D-form encoders (bit 22 set) and 8-byte LDR/STR D scaling.

// double dcalc(double a, double b) { return (a + b) * 2.0 - a / b; }
Function *BuildDcalc(Module &M) {
  LLVMContext &C = M.getContext();
  Type *F64 = Type::getDoubleTy(C);
  FunctionType *FT = FunctionType::get(F64, {F64, F64}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage, "dcalc", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *a = F->getArg(0);
  Value *b = F->getArg(1);
  Value *s = B.CreateFAdd(a, b, "s");
  Value *d = B.CreateFMul(s, ConstantFP::get(F64, 2.0), "d");
  Value *q = B.CreateFDiv(a, b, "q");
  Value *r = B.CreateFSub(d, q, "r");
  B.CreateRet(r);
  return F;
}

// double dclamp(double x, double lo, double hi) — early-return shape,
// exercises FCMP-D fused into a conditional branch (twice) and ret of
// SSA double from three blocks.
Function *BuildDclamp(Module &M) {
  LLVMContext &C = M.getContext();
  Type *F64 = Type::getDoubleTy(C);
  FunctionType *FT = FunctionType::get(F64, {F64, F64, F64}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage, "dclamp", &M);
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

// double dmax(double a, double b) { return a > b ? a : b; }
// Exercises FCMP-D-driven select on double result (FMOV D copy path).
Function *BuildDmax(Module &M) {
  LLVMContext &C = M.getContext();
  Type *F64 = Type::getDoubleTy(C);
  FunctionType *FT = FunctionType::get(F64, {F64, F64}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage, "dmax", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *a = F->getArg(0);
  Value *b = F->getArg(1);
  Value *cmp = B.CreateFCmpOGT(a, b, "cmp");
  Value *sel = B.CreateSelect(cmp, a, b, "sel");
  B.CreateRet(sel);
  return F;
}

// double dret_15(void) { return 1.5; } — bit-pattern materialization
// via 64-bit MOVZ/MOVK chain on x16 followed by FMOV D0, X16.
Function *BuildDretConst(Module &M, const char *name, double v) {
  LLVMContext &C = M.getContext();
  Type *F64 = Type::getDoubleTy(C);
  FunctionType *FT = FunctionType::get(F64, {}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage, name, &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  B.CreateRet(ConstantFP::get(F64, v));
  return F;
}

// int dtoi(double x) { return (int)x; } — fptosi double -> i32
// (FCVTZS Wd, Dn).
Function *BuildDtoi(Module &M) {
  LLVMContext &C = M.getContext();
  Type *F64 = Type::getDoubleTy(C);
  Type *I32 = Type::getInt32Ty(C);
  FunctionType *FT = FunctionType::get(I32, {F64}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage, "dtoi", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *x = F->getArg(0);
  Value *r = B.CreateFPToSI(x, I32, "r");
  B.CreateRet(r);
  return F;
}

// ====================== Round-8i: scalar FP conversions ====================
// Generic helper: build a function `name : (srcTy) -> dstTy` whose
// body is `ret CastOp(arg)`. Used for nearly every conversion case.
template <Instruction::CastOps Op>
Function *BuildCast1(Module &M, const char *name, Type *srcTy, Type *dstTy) {
  LLVMContext &C = M.getContext();
  FunctionType *FT = FunctionType::get(dstTy, {srcTy}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage, name, &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *r = B.CreateCast(Op, F->getArg(0), dstTy, "r");
  B.CreateRet(r);
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

// Double-precision references and tolerance check.
double refDcalc(double a, double b) { return (a + b) * 2.0 - a / b; }
double refDclamp(double x, double lo, double hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}
double refDmax(double a, double b) { return a > b ? a : b; }

bool deq(double x, double y) {
  double diff = std::fabs(x - y);
  double scale = std::fmax(1.0, std::fmax(std::fabs(x), std::fabs(y)));
  return diff <= 1e-12 * scale;
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

  // Round-8h: double-precision counterparts.
  Function *Fdcalc   = BuildDcalc(*M);
  Function *Fdclamp  = BuildDclamp(*M);
  Function *Fdmax    = BuildDmax(*M);
  Function *Fdret15  = BuildDretConst(*M, "dret_15", 1.5);
  Function *Fdtoi    = BuildDtoi(*M);

  // Round-8i: scalar FP conversions (fptosi64, fptoui, sitofp, uitofp,
  // fpext, fptrunc, FP/int bitcasts). Each one is built as a single-
  // instruction wrapper so opcode coverage exactly identifies which
  // conversion encoder the test exercises.
  Type *F32 = Type::getFloatTy(Ctx);
  Type *F64 = Type::getDoubleTy(Ctx);
  Type *I32 = Type::getInt32Ty(Ctx);
  Type *I64 = Type::getInt64Ty(Ctx);

  // FPToSI -> i64
  Function *FfToI64 = BuildCast1<Instruction::FPToSI>(*M, "f_to_i64", F32, I64);
  Function *FdToI64 = BuildCast1<Instruction::FPToSI>(*M, "d_to_i64", F64, I64);
  // FPToUI
  Function *FfToU32 = BuildCast1<Instruction::FPToUI>(*M, "f_to_u32", F32, I32);
  Function *FfToU64 = BuildCast1<Instruction::FPToUI>(*M, "f_to_u64", F32, I64);
  Function *FdToU32 = BuildCast1<Instruction::FPToUI>(*M, "d_to_u32", F64, I32);
  Function *FdToU64 = BuildCast1<Instruction::FPToUI>(*M, "d_to_u64", F64, I64);
  // SIToFP
  Function *Fi32ToF = BuildCast1<Instruction::SIToFP>(*M, "i32_to_f", I32, F32);
  Function *Fi32ToD = BuildCast1<Instruction::SIToFP>(*M, "i32_to_d", I32, F64);
  Function *Fi64ToF = BuildCast1<Instruction::SIToFP>(*M, "i64_to_f", I64, F32);
  Function *Fi64ToD = BuildCast1<Instruction::SIToFP>(*M, "i64_to_d", I64, F64);
  // UIToFP
  Function *Fu32ToF = BuildCast1<Instruction::UIToFP>(*M, "u32_to_f", I32, F32);
  Function *Fu32ToD = BuildCast1<Instruction::UIToFP>(*M, "u32_to_d", I32, F64);
  Function *Fu64ToF = BuildCast1<Instruction::UIToFP>(*M, "u64_to_f", I64, F32);
  Function *Fu64ToD = BuildCast1<Instruction::UIToFP>(*M, "u64_to_d", I64, F64);
  // FPExt / FPTrunc
  Function *Fwiden  = BuildCast1<Instruction::FPExt  >(*M, "widen",  F32, F64);
  Function *Fnarrow = BuildCast1<Instruction::FPTrunc>(*M, "narrow", F64, F32);
  // BitCast
  Function *Ffbits     = BuildCast1<Instruction::BitCast>(*M, "fbits",     F32, I32);
  Function *FfromFbits = BuildCast1<Instruction::BitCast>(*M, "from_fbits", I32, F32);
  Function *Fdbits     = BuildCast1<Instruction::BitCast>(*M, "dbits",     F64, I64);
  Function *FfromDbits = BuildCast1<Instruction::BitCast>(*M, "from_dbits", I64, F64);

  EmitOut eCalc  = EmitFunction(*Fcalc);
  EmitOut eClamp = EmitFunction(*Fclamp);
  EmitOut eMaxf  = EmitFunction(*Fmaxf);
  EmitOut eStale = EmitFunction(*Fstale);
  EmitOut eZero  = EmitFunction(*FrZero);
  EmitOut eTwo   = EmitFunction(*FrTwo);
  EmitOut e15    = EmitFunction(*FrFifteen);
  EmitOut eDcalc  = EmitFunction(*Fdcalc);
  EmitOut eDclamp = EmitFunction(*Fdclamp);
  EmitOut eDmax   = EmitFunction(*Fdmax);
  EmitOut eDret15 = EmitFunction(*Fdret15);
  EmitOut eDtoi   = EmitFunction(*Fdtoi);

  // Round-8i conversion emits.
  EmitOut eFtoI64 = EmitFunction(*FfToI64);
  EmitOut eDtoI64 = EmitFunction(*FdToI64);
  EmitOut eFtoU32 = EmitFunction(*FfToU32);
  EmitOut eFtoU64 = EmitFunction(*FfToU64);
  EmitOut eDtoU32 = EmitFunction(*FdToU32);
  EmitOut eDtoU64 = EmitFunction(*FdToU64);
  EmitOut eI32ToF = EmitFunction(*Fi32ToF);
  EmitOut eI32ToD = EmitFunction(*Fi32ToD);
  EmitOut eI64ToF = EmitFunction(*Fi64ToF);
  EmitOut eI64ToD = EmitFunction(*Fi64ToD);
  EmitOut eU32ToF = EmitFunction(*Fu32ToF);
  EmitOut eU32ToD = EmitFunction(*Fu32ToD);
  EmitOut eU64ToF = EmitFunction(*Fu64ToF);
  EmitOut eU64ToD = EmitFunction(*Fu64ToD);
  EmitOut eWiden  = EmitFunction(*Fwiden);
  EmitOut eNarrow = EmitFunction(*Fnarrow);
  EmitOut eFbits     = EmitFunction(*Ffbits);
  EmitOut eFromFbits = EmitFunction(*FfromFbits);
  EmitOut eDbits     = EmitFunction(*Fdbits);
  EmitOut eFromDbits = EmitFunction(*FfromDbits);

  int failures = 0;
  failures += CheckEmit("calc",         eCalc);
  failures += CheckEmit("clamp",        eClamp);
  failures += CheckEmit("maxf",         eMaxf);
  failures += CheckEmit("stale_repro",  eStale);
  failures += CheckEmit("ret_zero",     eZero);
  failures += CheckEmit("ret_two",      eTwo);
  failures += CheckEmit("ret_15",       e15);
  failures += CheckEmit("dcalc",        eDcalc);
  failures += CheckEmit("dclamp",       eDclamp);
  failures += CheckEmit("dmax",         eDmax);
  failures += CheckEmit("dret_15",      eDret15);
  failures += CheckEmit("dtoi",         eDtoi);
  failures += CheckEmit("f_to_i64",     eFtoI64);
  failures += CheckEmit("d_to_i64",     eDtoI64);
  failures += CheckEmit("f_to_u32",     eFtoU32);
  failures += CheckEmit("f_to_u64",     eFtoU64);
  failures += CheckEmit("d_to_u32",     eDtoU32);
  failures += CheckEmit("d_to_u64",     eDtoU64);
  failures += CheckEmit("i32_to_f",     eI32ToF);
  failures += CheckEmit("i32_to_d",     eI32ToD);
  failures += CheckEmit("i64_to_f",     eI64ToF);
  failures += CheckEmit("i64_to_d",     eI64ToD);
  failures += CheckEmit("u32_to_f",     eU32ToF);
  failures += CheckEmit("u32_to_d",     eU32ToD);
  failures += CheckEmit("u64_to_f",     eU64ToF);
  failures += CheckEmit("u64_to_d",     eU64ToD);
  failures += CheckEmit("widen",        eWiden);
  failures += CheckEmit("narrow",       eNarrow);
  failures += CheckEmit("fbits",        eFbits);
  failures += CheckEmit("from_fbits",   eFromFbits);
  failures += CheckEmit("dbits",        eDbits);
  failures += CheckEmit("from_dbits",   eFromDbits);

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

  // Round-8h double-precision opcode coverage. Each builder targets a
  // specific D-form encoding so a missing match means the lowering
  // never went through the new path.
  must("FADD-D in dcalc",  StreamHasOpcode(eDcalc.code,  kMaskFP3, kFAddD_val));
  must("FSUB-D in dcalc",  StreamHasOpcode(eDcalc.code,  kMaskFP3, kFSubD_val));
  must("FMUL-D in dcalc",  StreamHasOpcode(eDcalc.code,  kMaskFP3, kFMulD_val));
  must("FDIV-D in dcalc",  StreamHasOpcode(eDcalc.code,  kMaskFP3, kFDivD_val));
  must("FCMP-D in dclamp", StreamHasOpcode(eDclamp.code, kMaskFCMP, kFCmpD_val));
  must("FCMP-D in dmax",   StreamHasOpcode(eDmax.code,   kMaskFCMP, kFCmpD_val));
  must("FMOV D,X in dret_15",
       StreamHasOpcode(eDret15.code, kMaskFMovDX, kFMovDX_val));
  must("FCVTZS W,D in dtoi",
       StreamHasOpcode(eDtoi.code, kMaskFcvtzs, kFcvtzsWD_val));

  // Round-8i conversion opcode coverage.
  must("FCVTZS X,S in f_to_i64",
       StreamHasOpcode(eFtoI64.code, kMaskFcvtzs, kFcvtzsXS_val));
  must("FCVTZS X,D in d_to_i64",
       StreamHasOpcode(eDtoI64.code, kMaskFcvtzs, kFcvtzsXD_val));
  must("FCVTZU W,S in f_to_u32",
       StreamHasOpcode(eFtoU32.code, kMaskFcvtzs, kFcvtzuWS_val));
  must("FCVTZU X,S in f_to_u64",
       StreamHasOpcode(eFtoU64.code, kMaskFcvtzs, kFcvtzuXS_val));
  must("FCVTZU W,D in d_to_u32",
       StreamHasOpcode(eDtoU32.code, kMaskFcvtzs, kFcvtzuWD_val));
  must("FCVTZU X,D in d_to_u64",
       StreamHasOpcode(eDtoU64.code, kMaskFcvtzs, kFcvtzuXD_val));
  must("SCVTF S,W in i32_to_f",
       StreamHasOpcode(eI32ToF.code, kMaskFcvtzs, kScvtfSW_val));
  must("SCVTF D,W in i32_to_d",
       StreamHasOpcode(eI32ToD.code, kMaskFcvtzs, kScvtfDW_val));
  must("SCVTF S,X in i64_to_f",
       StreamHasOpcode(eI64ToF.code, kMaskFcvtzs, kScvtfSX_val));
  must("SCVTF D,X in i64_to_d",
       StreamHasOpcode(eI64ToD.code, kMaskFcvtzs, kScvtfDX_val));
  must("UCVTF S,W in u32_to_f",
       StreamHasOpcode(eU32ToF.code, kMaskFcvtzs, kUcvtfSW_val));
  must("UCVTF D,W in u32_to_d",
       StreamHasOpcode(eU32ToD.code, kMaskFcvtzs, kUcvtfDW_val));
  must("UCVTF S,X in u64_to_f",
       StreamHasOpcode(eU64ToF.code, kMaskFcvtzs, kUcvtfSX_val));
  must("UCVTF D,X in u64_to_d",
       StreamHasOpcode(eU64ToD.code, kMaskFcvtzs, kUcvtfDX_val));
  must("FCVT D,S in widen",
       StreamHasOpcode(eWiden.code, kMaskFcvtzs, kFcvtDS_val));
  must("FCVT S,D in narrow",
       StreamHasOpcode(eNarrow.code, kMaskFcvtzs, kFcvtSD_val));
  must("FMOV W,S in fbits",
       StreamHasOpcode(eFbits.code, kMaskFcvtzs, kFmovWFromS_val));
  must("FMOV S,W in from_fbits",
       StreamHasOpcode(eFromFbits.code, kMaskFcvtzs, kFmovSFromW_val));
  must("FMOV X,D in dbits",
       StreamHasOpcode(eDbits.code, kMaskFcvtzs, kFmovXFromD_val));
  must("FMOV D,X in from_dbits",
       StreamHasOpcode(eFromDbits.code, kMaskFMovDX, kFMovDX_val));

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
  // Double-precision execution wrappers.
  using D2  = double(*)(double, double);
  using D3  = double(*)(double, double, double);
  using D0  = double(*)();
  using DI1 = int   (*)(double);
  D2  dcalc  = (D2 )MakeExecutable(eDcalc.code);
  D3  dclamp = (D3 )MakeExecutable(eDclamp.code);
  D2  dmax   = (D2 )MakeExecutable(eDmax.code);
  D0  dret15 = (D0 )MakeExecutable(eDret15.code);
  DI1 dtoi   = (DI1)MakeExecutable(eDtoi.code);
  if (!calc || !clamp || !maxf || !stale || !rZero || !rTwo || !r15 ||
      !dcalc || !dclamp || !dmax || !dret15 || !dtoi) {
    std::printf("FAIL: mmap/exec setup\n");
    return 1;
  }

  // Round-8i conversion executables.
  using FnFtoI64 = long long          (*)(float);
  using FnDtoI64 = long long          (*)(double);
  using FnFtoU32 = unsigned int       (*)(float);
  using FnFtoU64 = unsigned long long (*)(float);
  using FnDtoU32 = unsigned int       (*)(double);
  using FnDtoU64 = unsigned long long (*)(double);
  using FnI32ToF = float              (*)(int);
  using FnI32ToD = double             (*)(int);
  using FnI64ToF = float              (*)(long long);
  using FnI64ToD = double             (*)(long long);
  using FnU32ToF = float              (*)(unsigned int);
  using FnU32ToD = double             (*)(unsigned int);
  using FnU64ToF = float              (*)(unsigned long long);
  using FnU64ToD = double             (*)(unsigned long long);
  using FnWiden  = double             (*)(float);
  using FnNarrow = float              (*)(double);
  using FnFbits     = uint32_t (*)(float);
  using FnFromFbits = float    (*)(uint32_t);
  using FnDbits     = uint64_t (*)(double);
  using FnFromDbits = double   (*)(uint64_t);

  auto fnFtoI64 = (FnFtoI64)MakeExecutable(eFtoI64.code);
  auto fnDtoI64 = (FnDtoI64)MakeExecutable(eDtoI64.code);
  auto fnFtoU32 = (FnFtoU32)MakeExecutable(eFtoU32.code);
  auto fnFtoU64 = (FnFtoU64)MakeExecutable(eFtoU64.code);
  auto fnDtoU32 = (FnDtoU32)MakeExecutable(eDtoU32.code);
  auto fnDtoU64 = (FnDtoU64)MakeExecutable(eDtoU64.code);
  auto fnI32ToF = (FnI32ToF)MakeExecutable(eI32ToF.code);
  auto fnI32ToD = (FnI32ToD)MakeExecutable(eI32ToD.code);
  auto fnI64ToF = (FnI64ToF)MakeExecutable(eI64ToF.code);
  auto fnI64ToD = (FnI64ToD)MakeExecutable(eI64ToD.code);
  auto fnU32ToF = (FnU32ToF)MakeExecutable(eU32ToF.code);
  auto fnU32ToD = (FnU32ToD)MakeExecutable(eU32ToD.code);
  auto fnU64ToF = (FnU64ToF)MakeExecutable(eU64ToF.code);
  auto fnU64ToD = (FnU64ToD)MakeExecutable(eU64ToD.code);
  auto fnWiden  = (FnWiden) MakeExecutable(eWiden.code);
  auto fnNarrow = (FnNarrow)MakeExecutable(eNarrow.code);
  auto fnFbits     = (FnFbits)    MakeExecutable(eFbits.code);
  auto fnFromFbits = (FnFromFbits)MakeExecutable(eFromFbits.code);
  auto fnDbits     = (FnDbits)    MakeExecutable(eDbits.code);
  auto fnFromDbits = (FnFromDbits)MakeExecutable(eFromDbits.code);
  if (!fnFtoI64 || !fnDtoI64 || !fnFtoU32 || !fnFtoU64 || !fnDtoU32 ||
      !fnDtoU64 || !fnI32ToF || !fnI32ToD || !fnI64ToF || !fnI64ToD ||
      !fnU32ToF || !fnU32ToD || !fnU64ToF || !fnU64ToD ||
      !fnWiden || !fnNarrow || !fnFbits || !fnFromFbits ||
      !fnDbits || !fnFromDbits) {
    std::printf("FAIL: mmap/exec setup (conversions)\n");
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

  // ============================ double tests ============================

  // dcalc
  {
    std::pair<double, double> in[] = {
      {1.0, 2.0}, {3.0, 4.0}, {-1.5, 2.5},
      {10.0, -2.0}, {0.5, 0.25}, {-7.0, -3.0}};
    for (auto [a, b] : in) {
      double got = dcalc(a, b), want = refDcalc(a, b);
      bool ok = deq(got, want);
      std::printf("  dcalc(%.4f,%.4f) = %.12f want %.12f %s\n",
                  a, b, got, want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
  }

  // dclamp
  {
    struct DC { double x, lo, hi; };
    DC in[] = {{0.5, 0.0, 1.0}, {-1.0, 0.0, 1.0}, {2.0, 0.0, 1.0},
               {0.0, 0.0, 1.0}, {1.0, 0.0, 1.0}, {-7.5, -10.0, -5.0}};
    for (const auto &c : in) {
      double got  = dclamp(c.x, c.lo, c.hi);
      double want = refDclamp(c.x, c.lo, c.hi);
      bool ok = deq(got, want);
      std::printf("  dclamp(%.4f,%.4f,%.4f) = %.12f want %.12f %s\n",
                  c.x, c.lo, c.hi, got, want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
  }

  // dmax
  {
    std::pair<double, double> in[] = {
      {1.0, 2.0}, {3.0, -4.0}, {-1.5, 2.5},
      {0.0, 0.0}, {7.0, 7.0}, {-1.0, -2.0}};
    for (auto [a, b] : in) {
      double got = dmax(a, b), want = refDmax(a, b);
      bool ok = deq(got, want);
      std::printf("  dmax(%.4f,%.4f) = %.12f want %.12f %s\n",
                  a, b, got, want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
  }

  // dret_15 — bit-pattern materialization for 1.5 via 64-bit MOVZ/MOVK
  // chain on x16 followed by FMOV D0, X16. 1.5 = 0x3FF8000000000000;
  // only one non-zero halfword (hw3 = 0x3FF8) so the chain is 1 MOVZ
  // + 1 MOVK + 1 FMOV.
  {
    double got = dret15();
    bool ok = deq(got, 1.5);
    std::printf("  dret_15() = %.12f want 1.500000000000 %s\n",
                got, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  }

  // dtoi — fptosi double -> i32 with truncation toward zero.
  {
    struct DT { double x; int want; };
    DT in[] = {{0.0, 0}, {1.0, 1}, {1.9, 1}, {-1.9, -1},
               {2.5, 2}, {-2.5, -2}, {1234567.89, 1234567}};
    for (const auto &t : in) {
      int got = dtoi(t.x);
      bool ok = (got == t.want);
      std::printf("  dtoi(%.4f) = %d want %d %s\n",
                  t.x, got, t.want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
  }

  // ============================ Round-8i conversions ====================

  // FPToSI -> i64
  {
    struct C { float x; long long want; };
    C in[] = {{0.0f, 0}, {1.0f, 1}, {-1.0f, -1}, {1.9f, 1}, {-1.9f, -1},
              {12345.5f, 12345}, {-12345.5f, -12345}};
    for (const auto &t : in) {
      long long got = fnFtoI64(t.x);
      bool ok = (got == t.want);
      std::printf("  f_to_i64(%.4f) = %lld want %lld %s\n",
                  t.x, got, t.want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
  }
  {
    struct C { double x; long long want; };
    C in[] = {{0.0, 0}, {1.0, 1}, {-1.0, -1}, {1.9, 1}, {-1.9, -1},
              {1234567890.5, 1234567890LL}, {-1234567890.5, -1234567890LL}};
    for (const auto &t : in) {
      long long got = fnDtoI64(t.x);
      bool ok = (got == t.want);
      std::printf("  d_to_i64(%.4f) = %lld want %lld %s\n",
                  t.x, got, t.want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
  }

  // FPToUI
  {
    float fin[] = {0.0f, 1.0f, 2.5f, 1234.75f, 0.99f};
    for (float x : fin) {
      unsigned int got = fnFtoU32(x);
      unsigned int want = (unsigned int)x;
      bool ok = (got == want);
      std::printf("  f_to_u32(%.4f) = %u want %u %s\n",
                  x, got, want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
      unsigned long long got64 = fnFtoU64(x);
      unsigned long long want64 = (unsigned long long)x;
      bool ok64 = (got64 == want64);
      std::printf("  f_to_u64(%.4f) = %llu want %llu %s\n",
                  x, got64, want64, ok64 ? "OK" : "FAIL");
      if (!ok64) ++failures;
    }
    double din[] = {0.0, 1.0, 2.5, 1234567.89, 0.99};
    for (double x : din) {
      unsigned int got = fnDtoU32(x);
      unsigned int want = (unsigned int)x;
      bool ok = (got == want);
      std::printf("  d_to_u32(%.4f) = %u want %u %s\n",
                  x, got, want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
      unsigned long long got64 = fnDtoU64(x);
      unsigned long long want64 = (unsigned long long)x;
      bool ok64 = (got64 == want64);
      std::printf("  d_to_u64(%.4f) = %llu want %llu %s\n",
                  x, got64, want64, ok64 ? "OK" : "FAIL");
      if (!ok64) ++failures;
    }
  }

  // SIToFP
  {
    int iin[] = {0, 1, -1, 1234567, -1234567};
    for (int x : iin) {
      float gotf = fnI32ToF(x); float wantf = (float)x;
      double gotd = fnI32ToD(x); double wantd = (double)x;
      bool okf = feq(gotf, wantf), okd = deq(gotd, wantd);
      std::printf("  i32_to_f(%d) = %.6f want %.6f %s\n",
                  x, gotf, wantf, okf ? "OK" : "FAIL");
      if (!okf) ++failures;
      std::printf("  i32_to_d(%d) = %.12f want %.12f %s\n",
                  x, gotd, wantd, okd ? "OK" : "FAIL");
      if (!okd) ++failures;
    }
    long long lin[] = {0LL, 1LL, -1LL, 1234567890123LL, -1234567890123LL};
    for (long long x : lin) {
      float gotf = fnI64ToF(x); float wantf = (float)x;
      double gotd = fnI64ToD(x); double wantd = (double)x;
      bool okf = feq(gotf, wantf), okd = deq(gotd, wantd);
      std::printf("  i64_to_f(%lld) = %.6f want %.6f %s\n",
                  x, gotf, wantf, okf ? "OK" : "FAIL");
      if (!okf) ++failures;
      std::printf("  i64_to_d(%lld) = %.12f want %.12f %s\n",
                  x, gotd, wantd, okd ? "OK" : "FAIL");
      if (!okd) ++failures;
    }
  }

  // UIToFP
  {
    unsigned int uin[] = {0u, 1u, 1234567u, 0xFFFFFFFFu};
    for (unsigned int x : uin) {
      float gotf = fnU32ToF(x); float wantf = (float)x;
      double gotd = fnU32ToD(x); double wantd = (double)x;
      bool okf = feq(gotf, wantf), okd = deq(gotd, wantd);
      std::printf("  u32_to_f(%u) = %.6f want %.6f %s\n",
                  x, gotf, wantf, okf ? "OK" : "FAIL");
      if (!okf) ++failures;
      std::printf("  u32_to_d(%u) = %.12f want %.12f %s\n",
                  x, gotd, wantd, okd ? "OK" : "FAIL");
      if (!okd) ++failures;
    }
    unsigned long long ulin[] = {0ULL, 1ULL, 1234567890123ULL,
                                 0xFFFFFFFFFFFFFFFFULL};
    for (unsigned long long x : ulin) {
      float gotf = fnU64ToF(x); float wantf = (float)x;
      double gotd = fnU64ToD(x); double wantd = (double)x;
      bool okf = feq(gotf, wantf), okd = deq(gotd, wantd);
      std::printf("  u64_to_f(%llu) = %.6f want %.6f %s\n",
                  x, gotf, wantf, okf ? "OK" : "FAIL");
      if (!okf) ++failures;
      std::printf("  u64_to_d(%llu) = %.12f want %.12f %s\n",
                  x, gotd, wantd, okd ? "OK" : "FAIL");
      if (!okd) ++failures;
    }
  }

  // FPExt / FPTrunc
  {
    float fin[] = {0.0f, 1.5f, -2.25f, 12345.0f};
    for (float x : fin) {
      double got = fnWiden(x); double want = (double)x;
      bool ok = deq(got, want);
      std::printf("  widen(%.4f) = %.12f want %.12f %s\n",
                  x, got, want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
    double din[] = {0.0, 1.5, -2.25, 12345.0};
    for (double x : din) {
      float got = fnNarrow(x); float want = (float)x;
      bool ok = feq(got, want);
      std::printf("  narrow(%.6f) = %.6f want %.6f %s\n",
                  x, got, want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
  }

  // BitCast — exact bit-pattern checks. We must match the host's
  // memcpy-based reinterpret exactly; no tolerance allowed.
  {
    auto refFbits = [](float f) {
      uint32_t b; std::memcpy(&b, &f, sizeof(b)); return b;
    };
    auto refFromFbits = [](uint32_t b) {
      float f; std::memcpy(&f, &b, sizeof(f)); return f;
    };
    auto refDbits = [](double d) {
      uint64_t b; std::memcpy(&b, &d, sizeof(b)); return b;
    };
    auto refFromDbits = [](uint64_t b) {
      double d; std::memcpy(&d, &b, sizeof(d)); return d;
    };
    float fin[] = {0.0f, 1.0f, -2.5f, 1.5f};
    for (float x : fin) {
      uint32_t got = fnFbits(x), want = refFbits(x);
      bool ok = (got == want);
      std::printf("  fbits(%.4f) = 0x%08x want 0x%08x %s\n",
                  x, got, want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
    uint32_t fbin[] = {0u, 0x3F800000u, 0xC0200000u, 0x3FC00000u};
    for (uint32_t b : fbin) {
      float got = fnFromFbits(b), want = refFromFbits(b);
      uint32_t gb, wb;
      std::memcpy(&gb, &got, 4); std::memcpy(&wb, &want, 4);
      bool ok = (gb == wb);
      std::printf("  from_fbits(0x%08x) = 0x%08x want 0x%08x %s\n",
                  b, gb, wb, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
    double din[] = {0.0, 1.0, -2.5, 1.5};
    for (double x : din) {
      uint64_t got = fnDbits(x), want = refDbits(x);
      bool ok = (got == want);
      std::printf("  dbits(%.4f) = 0x%016llx want 0x%016llx %s\n",
                  x, (unsigned long long)got,
                  (unsigned long long)want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
    uint64_t dbin[] = {0ULL, 0x3FF0000000000000ULL, 0xC004000000000000ULL,
                       0x3FF8000000000000ULL};
    for (uint64_t b : dbin) {
      double got = fnFromDbits(b), want = refFromDbits(b);
      uint64_t gb, wb;
      std::memcpy(&gb, &got, 8); std::memcpy(&wb, &want, 8);
      bool ok = (gb == wb);
      std::printf("  from_dbits(0x%016llx) = 0x%016llx want 0x%016llx %s\n",
                  (unsigned long long)b, (unsigned long long)gb,
                  (unsigned long long)wb, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
  }

  if (failures) {
    std::printf("FAILED: %d case(s)\n", failures);
    return 1;
  }
  std::printf("PASS: light fp ops (calc + clamp + maxf + stale_repro "
              "+ ret_const + dcalc + dclamp + dmax + dret_15 + dtoi "
              "+ scalar fp conversions)\n");
  return 0;
#else
  std::printf("PASS (emit+coverage only): non-AArch64 host, execution "
              "validation skipped\n");
  return 0;
#endif
}
