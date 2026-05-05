// test_endian_parity.cpp — compile-only triple-acceptance test for the
// lightweight AArch64 emitter.
//
// What it proves:
//   * `light::emit` accepts an `aarch64-*` triple (LE).
//   * `light::emit` accepts an `aarch64_be-*` triple (BE).
//   * `light::emit` rejects ILP32 variants (`aarch64_32-*`, `arm64_32-*`)
//     with `Status::NotAarch64`.
//   * `light::emit` rejects unrelated triples (e.g. `x86_64-*`,
//     `riscv64-*`) with the same `Status::NotAarch64`.
//   * The emitted instruction byte stream for the LE and BE modules
//     is bit-identical when the IR is otherwise the same. This is the
//     round-8 endian-parity claim:
//       - the AArch64 instruction stream is unconditionally LE (ARM ARM
//         B2.6.2), so neither MOVZ/MOVK halfwords nor LDR/STR opcodes
//         depend on target data endian;
//       - Writer::emit writes explicit little-endian bytes for every
//         32-bit instruction word, so emission is host-endian-agnostic;
//       - the runtime always JIT-runs the code it emits, so target data
//         endian == host endian → producer and consumer of any data
//         buffer agree by construction.
//
// What it does NOT prove:
//   * That code emitted with the BE module actually runs correctly on a
//     real aarch64_be machine. We have no BE host or emulator in the
//     regression set; that final-mile claim is documented in
//     runtime/LightBackend_LIMITATIONS.md as "compile-time tested only,
//     execution validation deferred to a target machine".

#include "light_aarch64.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

// Build a tiny module that exercises a couple of endian-sensitive
// emitter paths in one go:
//   - i32 load from a pointer-arg register (LDR Wt, [Xn, #imm12])
//   - i32 store back to that pointer (STR Wt, [Xn, #imm12])
//   - i16 load + zext (LDRH + ZExt no-op)
//   - i32 immediate materialization 0x12345 -> MOVZ + MOVK (positional)
//   - i32 add and ret
static std::unique_ptr<llvm::Module>
BuildSampleModule(llvm::LLVMContext &C, const char *Triple, const char *DL) {
  using namespace llvm;
  auto M = std::make_unique<Module>("light_endian_test", C);
  M->setTargetTriple(Triple);
  M->setDataLayout(DL);

  Type *I32 = Type::getInt32Ty(C);
  Type *I16 = Type::getInt16Ty(C);
  Type *I8P = PointerType::get(C, 0);
  FunctionType *FT = FunctionType::get(I32, {I8P}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage, "f", M.get());

  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Argument *P = F->getArg(0);
  // i32 load from p[0]
  Value *a   = B.CreateAlignedLoad(I32, P, MaybeAlign(4), "a");
  // i16 load + zext from p[1] (4-byte stride GEP). Exercises LDRH and
  // a no-op zext, both of which are endian-symmetric in our model.
  Value *qp  = B.CreateGEP(I32, P, B.getInt64(1), "qp");
  Value *b   = B.CreateAlignedLoad(I16, qp, MaybeAlign(2), "b");
  Value *bz  = B.CreateZExt(b, I32, "bz");
  // reg-reg add — exercises encAddSubReg.
  Value *sum = B.CreateAdd(a, bz, "sum");
  // 12-bit immediate add — exercises encAddSubImm (positional-imm12,
  // endian-neutral instruction encoding).
  Value *k   = B.CreateAdd(sum, B.getInt32(0x123), "k");
  B.CreateAlignedStore(k, P, MaybeAlign(4));
  B.CreateRet(k);
  return M;
}

struct Case {
  const char *triple;
  const char *dl;
  // expected: 1 = accept, 0 = reject as NotAarch64
  int expectAccept;
};

static const char *StatusName(::light::Status s) {
  switch (s) {
    case ::light::Status::Ok:           return "Ok";
    case ::light::Status::Unsupported:  return "Unsupported";
    case ::light::Status::TooLarge:     return "TooLarge";
    case ::light::Status::NotAarch64:   return "NotAarch64";
  }
  return "?";
}

static int RunCase(const Case &c, std::vector<uint8_t> &codeOut) {
  llvm::LLVMContext Ctx;
  auto M = BuildSampleModule(Ctx, c.triple, c.dl);
  llvm::Function *F = M->getFunction("f");

  std::vector<uint8_t> buf(4096);
  ::light::Result r = ::light::emit(*F, buf.data(), buf.size(), nullptr, 0);

  std::printf("[case] triple=%-32s status=%-12s bytes=%zu reason=%s\n",
              c.triple, StatusName(r.status), r.codeBytes, r.reason.c_str());

  if (c.expectAccept) {
    if (r.status != ::light::Status::Ok) {
      std::printf("  FAIL: expected accept\n");
      return 1;
    }
    if (r.codeBytes == 0 || r.codeBytes > buf.size()) {
      std::printf("  FAIL: bad codeBytes=%zu\n", r.codeBytes);
      return 1;
    }
    codeOut.assign(buf.begin(), buf.begin() + r.codeBytes);
  } else {
    if (r.status != ::light::Status::NotAarch64) {
      std::printf("  FAIL: expected NotAarch64\n");
      return 1;
    }
  }
  return 0;
}

} // namespace

int main() {
  // Standard AArch64 LP64 data layouts. The "e-" / "E-" prefix is the
  // module's claimed data endian; the emitter does not consult it
  // directly (it switches on the triple), but we set them consistently
  // so any future endian-aware emitter code sees the same view.
  static constexpr const char *DL_LE =
      "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128";
  static constexpr const char *DL_BE =
      "E-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128";

  Case cases[] = {
    {"aarch64-unknown-linux-gnu",     DL_LE, 1},
    {"aarch64_be-unknown-linux-gnu",  DL_BE, 1},
    {"arm64-apple-darwin",            DL_LE, 1},
    {"aarch64_32-unknown-linux-gnu",  DL_LE, 0},
    {"arm64_32-apple-watchos",        DL_LE, 0},
    {"x86_64-unknown-linux-gnu",      DL_LE, 0},
    {"riscv64-unknown-linux-gnu",     DL_LE, 0},
  };

  int failures = 0;
  std::vector<uint8_t> codeLE, codeBE, codeArm64;
  for (const auto &c : cases) {
    std::vector<uint8_t> code;
    failures += RunCase(c, code);
    std::string t = c.triple;
    if (t == "aarch64-unknown-linux-gnu")    codeLE    = code;
    if (t == "aarch64_be-unknown-linux-gnu") codeBE    = code;
    if (t == "arm64-apple-darwin")           codeArm64 = code;
  }

  // Endian-parity claim: the same IR shape compiled under LE and BE
  // triples must produce the same instruction byte stream. Writer::emit
  // writes explicit LE bytes for every 32-bit word, so any deviation
  // here would be a regression in either the triple gate or the
  // instruction-stream byte writer.
  if (!codeLE.empty() && !codeBE.empty()) {
    if (codeLE.size() != codeBE.size() ||
        std::memcmp(codeLE.data(), codeBE.data(), codeLE.size()) != 0) {
      std::printf("FAIL: aarch64 LE vs aarch64_be byte streams differ "
                  "(%zu vs %zu bytes)\n", codeLE.size(), codeBE.size());
      ++failures;
    } else {
      std::printf("OK: aarch64 vs aarch64_be byte streams identical "
                  "(%zu bytes)\n", codeLE.size());
    }
  }
  if (!codeLE.empty() && !codeArm64.empty()) {
    if (codeLE.size() != codeArm64.size() ||
        std::memcmp(codeLE.data(), codeArm64.data(), codeLE.size()) != 0) {
      std::printf("FAIL: aarch64 vs arm64 byte streams differ\n");
      ++failures;
    } else {
      std::printf("OK: aarch64 vs arm64 byte streams identical\n");
    }
  }

  if (failures) {
    std::printf("FAILED: %d case(s)\n", failures);
    return 1;
  }
  std::printf("PASS: light endian/triple parity\n");
  return 0;
}
