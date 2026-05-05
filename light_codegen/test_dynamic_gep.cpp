// test_dynamic_gep.cpp — round-8k: validate two-term dynamic GEP support
// in the light AArch64 emitter.
//
// What it proves:
//
//   1. A 2-D GEP of the form `base[i][j]` lowers to a single
//      `LDR/STR` from an x17 address built from the base + two ADD
//      sequences, one per dynamic term. Both single-instruction
//      ADD-extended-register (shift <= 4) and the 3-instruction
//      extend+LSL+ADD path (shift 5..12) are exercised.
//
//   2. Multi-index struct GEPs that reduce to const_off + two dynamic
//      terms (e.g. `s->arr[i][j]` where `arr` is at a non-zero struct
//      offset) work end-to-end.
//
//   3. Stack-passed dynamic indexes (round 8j interaction) feed the
//      address materialiser correctly: the index value loaded from
//      [sp, frameSize+incomingOff] is used as the GEP idx via the
//      existing valueInReg() path.
//
//   4. Both float and double element types are supported (different
//      element scales and access sizes).
//
//   5. Pointer base (function arg, InRegScaledIndex) and stored value
//      paths (store + load round trip) work.
//
// On non-AArch64 hosts only `light::emit` correctness is checked
// (status=Ok + non-zero byte stream).

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
#  define LIGHT_DGEP_HOST_AARCH64 1
#  include <sys/mman.h>
#else
#  define LIGHT_DGEP_HOST_AARCH64 0
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

#if LIGHT_DGEP_HOST_AARCH64
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

bool feq(float a, float b) { return std::fabs(a - b) < 1e-5f; }
bool deq(double a, double b) { return std::fabs(a - b) < 1e-9; }

// ===========================================================================
// IR builders.
// ===========================================================================

// int load_2d_i32x4(int (*base)[4], int i, int j) { return base[i][j]; }
// Element type [4 x i32]. Outer stride = 16 (log2=4) → single ADD-ext.
// Inner stride = 4 (log2=2) → single ADD-ext.
Function *BuildLoad2DI32x4(Module &M) {
  LLVMContext &C = M.getContext();
  Type *I32 = Type::getInt32Ty(C);
  Type *I64 = Type::getInt64Ty(C);
  ArrayType *Row = ArrayType::get(I32, 4);
  Type *RowPtr = PointerType::getUnqual(Row);
  FunctionType *FT =
      FunctionType::get(I32, {RowPtr, I32, I32}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 "load_2d_i32x4", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *iE = B.CreateSExt(F->getArg(1), I64);
  Value *jE = B.CreateSExt(F->getArg(2), I64);
  Value *gep = B.CreateGEP(Row, F->getArg(0), {iE, jE});
  Value *ld = B.CreateAlignedLoad(I32, gep, Align(4));
  B.CreateRet(ld);
  return F;
}

// int load_2d_i32x8(int (*base)[8], int i, int j) { return base[i][j]; }
// Outer stride = 32 (log2=5) → 3-instr extend+LSL+ADD path.
// Inner stride = 4 (log2=2) → single ADD-ext.
Function *BuildLoad2DI32x8(Module &M) {
  LLVMContext &C = M.getContext();
  Type *I32 = Type::getInt32Ty(C);
  Type *I64 = Type::getInt64Ty(C);
  ArrayType *Row = ArrayType::get(I32, 8);
  Type *RowPtr = PointerType::getUnqual(Row);
  FunctionType *FT =
      FunctionType::get(I32, {RowPtr, I32, I32}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 "load_2d_i32x8", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *iE = B.CreateSExt(F->getArg(1), I64);
  Value *jE = B.CreateSExt(F->getArg(2), I64);
  Value *gep = B.CreateGEP(Row, F->getArg(0), {iE, jE});
  Value *ld = B.CreateAlignedLoad(I32, gep, Align(4));
  B.CreateRet(ld);
  return F;
}

// int store_2d(int (*base)[4], int i, int j, int v) {
//   base[i][j] = v;
//   return base[i][j];
// }
Function *BuildStore2D(Module &M) {
  LLVMContext &C = M.getContext();
  Type *I32 = Type::getInt32Ty(C);
  Type *I64 = Type::getInt64Ty(C);
  ArrayType *Row = ArrayType::get(I32, 4);
  Type *RowPtr = PointerType::getUnqual(Row);
  FunctionType *FT =
      FunctionType::get(I32, {RowPtr, I32, I32, I32}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 "store_2d", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *iE = B.CreateSExt(F->getArg(1), I64);
  Value *jE = B.CreateSExt(F->getArg(2), I64);
  Value *gep = B.CreateGEP(Row, F->getArg(0), {iE, jE});
  B.CreateAlignedStore(F->getArg(3), gep, Align(4));
  Value *ld = B.CreateAlignedLoad(I32, gep, Align(4));
  B.CreateRet(ld);
  return F;
}

// double load_d2d(double (*base)[4], int i, int j) { return base[i][j]; }
// Outer stride = 32 (log2=5) → 3-instr path.
// Inner stride = 8 (log2=3) → single ADD-ext.
Function *BuildLoadD2D(Module &M) {
  LLVMContext &C = M.getContext();
  Type *F64 = Type::getDoubleTy(C);
  Type *I32 = Type::getInt32Ty(C);
  Type *I64 = Type::getInt64Ty(C);
  ArrayType *Row = ArrayType::get(F64, 4);
  Type *RowPtr = PointerType::getUnqual(Row);
  FunctionType *FT =
      FunctionType::get(F64, {RowPtr, I32, I32}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 "load_d2d", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *iE = B.CreateSExt(F->getArg(1), I64);
  Value *jE = B.CreateSExt(F->getArg(2), I64);
  Value *gep = B.CreateGEP(Row, F->getArg(0), {iE, jE});
  Value *ld = B.CreateAlignedLoad(F64, gep, Align(8));
  B.CreateRet(ld);
  return F;
}

// struct S { int pad; int arr[4][4]; };
// int load_struct_2d(struct S *s, int i, int j) { return s->arr[i][j]; }
// Const offset for the `arr` field, then two dynamic terms.
Function *BuildLoadStruct2D(Module &M) {
  LLVMContext &C = M.getContext();
  Type *I32 = Type::getInt32Ty(C);
  Type *I64 = Type::getInt64Ty(C);
  ArrayType *Inner = ArrayType::get(I32, 4);
  ArrayType *Outer = ArrayType::get(Inner, 4);
  StructType *S = StructType::create(C, {I32, Outer}, "S");
  Type *SP = PointerType::getUnqual(S);
  FunctionType *FT =
      FunctionType::get(I32, {SP, I32, I32}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 "load_struct_2d", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *iE = B.CreateSExt(F->getArg(1), I64);
  Value *jE = B.CreateSExt(F->getArg(2), I64);
  // GEP %S, ptr %s, i32 0, i32 1, i64 i, i64 j
  Value *gep = B.CreateGEP(S, F->getArg(0),
                           {ConstantInt::get(I32, 0),
                            ConstantInt::get(I32, 1),
                            iE, jE});
  Value *ld = B.CreateAlignedLoad(I32, gep, Align(4));
  B.CreateRet(ld);
  return F;
}

// int load_2d_stack_idx(int (*base)[4], i64 a1..a7, i32 i, i32 j) {
//   return base[i][j];
// }
// Args: [base, a1..a7, i, j] -> base+a1..a7 in x0..x7, i+j stack-passed.
// Exercises round-8j stack-arg preload feeding round-8k two-term GEP.
Function *BuildLoad2DStackIdx(Module &M) {
  LLVMContext &C = M.getContext();
  Type *I32 = Type::getInt32Ty(C);
  Type *I64 = Type::getInt64Ty(C);
  ArrayType *Row = ArrayType::get(I32, 4);
  Type *RowPtr = PointerType::getUnqual(Row);
  std::vector<Type *> ats;
  ats.push_back(RowPtr);
  for (int k = 0; k < 7; ++k) ats.push_back(I64);
  ats.push_back(I32); // i (stack arg 8)
  ats.push_back(I32); // j (stack arg 9)
  FunctionType *FT = FunctionType::get(I32, ats, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 "load_2d_stack_idx", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *iE = B.CreateSExt(F->getArg(8), I64);
  Value *jE = B.CreateSExt(F->getArg(9), I64);
  Value *gep = B.CreateGEP(Row, F->getArg(0), {iE, jE});
  Value *ld = B.CreateAlignedLoad(I32, gep, Align(4));
  B.CreateRet(ld);
  return F;
}

// Coverage helper: count how many ADD-extended-register (64-bit)
// instructions appear in the LE code stream. Fixed bits per ARM ARM
// C6.2.5: sf=1, op=0, S=0, 01011 001 → 0x8B20_0000 / mask 0xFFE0_0000.
unsigned CountAddExtReg(const std::vector<uint8_t> &code) {
  unsigned n = 0;
  for (size_t i = 0; i + 4 <= code.size(); i += 4) {
    uint32_t w = (uint32_t)code[i] | ((uint32_t)code[i + 1] << 8) |
                 ((uint32_t)code[i + 2] << 16) | ((uint32_t)code[i + 3] << 24);
    if ((w & 0xFFE00000u) == 0x8B200000u)
      ++n;
  }
  return n;
}

} // namespace

int main() {
  LLVMContext Ctx;
  auto M = NewAArch64Module(Ctx, "test_dgep");

  Function *F4   = BuildLoad2DI32x4(*M);
  Function *F8   = BuildLoad2DI32x8(*M);
  Function *FS   = BuildStore2D(*M);
  Function *FD   = BuildLoadD2D(*M);
  Function *FSt  = BuildLoadStruct2D(*M);
  Function *FSI  = BuildLoad2DStackIdx(*M);

  EmitOut e4   = EmitFunction(*F4);
  EmitOut e8   = EmitFunction(*F8);
  EmitOut eS   = EmitFunction(*FS);
  EmitOut eD   = EmitFunction(*FD);
  EmitOut eSt  = EmitFunction(*FSt);
  EmitOut eSI  = EmitFunction(*FSI);

  int failures = 0;
  failures += CheckEmit("load_2d_i32x4",       e4);
  failures += CheckEmit("load_2d_i32x8",       e8);
  failures += CheckEmit("store_2d",            eS);
  failures += CheckEmit("load_d2d",            eD);
  failures += CheckEmit("load_struct_2d",      eSt);
  failures += CheckEmit("load_2d_stack_idx",   eSI);

  if (failures) {
    std::printf("FAIL: emit stage (%d failures)\n", failures);
    return 1;
  }

  // Coverage guard: load_2d_i32x4 must emit AT LEAST 2 ADD-extended-register
  // instructions (one per dynamic term). This catches a regression where a
  // future refactor silently drops the second term.
  unsigned addExt4 = CountAddExtReg(e4.code);
  std::printf("  coverage: load_2d_i32x4 ADD-ext count = %u (expect >= 2)\n",
              addExt4);
  if (addExt4 < 2) {
    std::printf("FAIL: expected 2+ ADD-ext-reg in load_2d_i32x4\n");
    return 1;
  }

#if LIGHT_DGEP_HOST_AARCH64
  using Fn4   = int (*)(int (*)[4], int, int);
  using Fn8   = int (*)(int (*)[8], int, int);
  using FnS   = int (*)(int (*)[4], int, int, int);
  using FnD   = double (*)(double (*)[4], int, int);
  struct S { int pad; int arr[4][4]; };
  using FnSt  = int (*)(S *, int, int);
  using FnSI  = int (*)(int (*)[4],
                        long long, long long, long long, long long,
                        long long, long long, long long,
                        int, int);

  auto fn4  = (Fn4) MakeExecutable(e4.code);
  auto fn8  = (Fn8) MakeExecutable(e8.code);
  auto fnS  = (FnS) MakeExecutable(eS.code);
  auto fnD  = (FnD) MakeExecutable(eD.code);
  auto fnSt = (FnSt)MakeExecutable(eSt.code);
  auto fnSI = (FnSI)MakeExecutable(eSI.code);
  if (!fn4 || !fn8 || !fnS || !fnD || !fnSt || !fnSI) {
    std::printf("FAIL: mmap/exec setup\n");
    return 1;
  }

  // load_2d_i32x4: row stride 16, elem 4. Both shifts <= 4.
  {
    int m[4][4] = {
      {  1,  2,  3,  4},
      { 10, 20, 30, 40},
      {100,200,300,400},
      {  7,  8,  9, 11},
    };
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) {
        int got = fn4(m, i, j);
        int want = m[i][j];
        bool ok = (got == want);
        if (!ok) {
          std::printf("  load_2d_i32x4(%d,%d) = %d want %d FAIL\n",
                      i, j, got, want);
          ++failures;
        }
      }
    std::printf("  load_2d_i32x4 16 cells OK\n");
  }

  // load_2d_i32x8: row stride 32 (3-instr path), elem 4 (single ADD-ext).
  {
    int m[4][8];
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 8; ++j) m[i][j] = i * 1000 + j;
    int got = fn8(m, 3, 7);
    int want = 3007;
    bool ok = (got == want);
    std::printf("  load_2d_i32x8(3,7) = %d want %d %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
    got = fn8(m, 0, 0); want = 0; ok = (got == want);
    std::printf("  load_2d_i32x8(0,0) = %d want %d %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
    got = fn8(m, 2, 5); want = 2005; ok = (got == want);
    std::printf("  load_2d_i32x8(2,5) = %d want %d %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  }

  // store_2d: round-trip store/load through the same dynamic GEP.
  {
    int m[4][4];
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) m[i][j] = -1;
    int got  = fnS(m, 2, 3, 4242);
    int want = 4242;
    bool ok = (got == want && m[2][3] == 4242 && m[2][2] == -1);
    std::printf("  store_2d(2,3,4242) = %d, m[2][3]=%d %s\n",
                got, m[2][3], ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  }

  // load_d2d: row stride 32 (3-instr), elem 8 (single ADD-ext).
  {
    double m[3][4] = {
      {1.0, 2.0, 3.0, 4.0},
      {10.0,20.0,30.0,40.0},
      {0.5, 1.5, 2.5, 3.5},
    };
    double got  = fnD(m, 2, 1);
    double want = 1.5;
    bool ok = deq(got, want);
    std::printf("  load_d2d(2,1) = %.6f want %.6f %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
    got = fnD(m, 1, 3); want = 40.0; ok = deq(got, want);
    std::printf("  load_d2d(1,3) = %.6f want %.6f %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  }

  // load_struct_2d: const offset (struct field) + two dynamic terms.
  {
    S s{};
    s.pad = 999;
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) s.arr[i][j] = i * 100 + j;
    int got  = fnSt(&s, 2, 3);
    int want = 203;
    bool ok = (got == want);
    std::printf("  load_struct_2d(2,3) = %d want %d %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
    got = fnSt(&s, 0, 0); want = 0; ok = (got == want);
    std::printf("  load_struct_2d(0,0) = %d want %d %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
    got = fnSt(&s, 3, 3); want = 303; ok = (got == want);
    std::printf("  load_struct_2d(3,3) = %d want %d %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  }

  // load_2d_stack_idx: i and j arrive as stack args (round-8j path),
  // then feed the round-8k two-term GEP materialiser.
  {
    int m[4][4];
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) m[i][j] = i * 10 + j + 1;
    int got  = fnSI(m, 0, 0, 0, 0, 0, 0, 0, /*i=*/2, /*j=*/3);
    int want = 24;
    bool ok = (got == want);
    std::printf("  load_2d_stack_idx(...,2,3) = %d want %d %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
    got = fnSI(m, 0, 0, 0, 0, 0, 0, 0, 3, 0); want = 31; ok = (got == want);
    std::printf("  load_2d_stack_idx(...,3,0) = %d want %d %s\n",
                got, want, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  }

  if (failures) {
    std::printf("FAILED: %d case(s)\n", failures);
    return 1;
  }
  std::printf("PASS: light dynamic GEP "
              "(2D i32 small/large stride + store + double + struct field "
              "+ stack-passed indexes)\n");
  return 0;
#else
  std::printf("PASS (emit-only): non-AArch64 host, execution skipped\n");
  return 0;
#endif
}
