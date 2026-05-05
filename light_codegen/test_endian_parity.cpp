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
//         B2.6.2), so LDR/STR/ADD-imm opcodes (and the MOVZ/MOVK halfword
//         encodings used elsewhere by the emitter for absolute-address
//         materialization) do not depend on target data endian;
//       - Writer::emit writes explicit little-endian bytes for every
//         32-bit instruction word, so emission is host-endian-agnostic;
//       - the runtime always JIT-runs the code it emits, so target data
//         endian == host endian → producer and consumer of any data
//         buffer agree by construction.
//
// Scope note: the IR sample below exercises LDR/LDRH/STR and the
// 12-bit-immediate ADD fast path, AND deliberately includes a wider
// constant (0x12345 — does not fit imm12 = 0xFFF) so that the binop
// RHS routes through `materializeImmAny` → `emitMovImm`, producing a
// real MOVZ + MOVK,lsl#16 halfword chain in the emitted stream. This
// is what makes the parity claim non-trivial: MOVZ/MOVK use the
// positional `hw` field (ARM ARM C6.2.193 / C6.2.194), so any LE/BE
// divergence would necessarily come from either the triple gate or
// the host-endian-agnostic `Writer::emit`. The test asserts presence
// of at least one MOVZ and one MOVK opcode word in the LE stream as
// a guard against the materializer being silently bypassed.
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
//   - 12-bit immediate add (encAddSubImm: imm12 positional field)
//   - >16-bit constant add — falls out of the imm12 fast path and
//     routes through `materializeImmAny` → `emitMovImm`, producing
//     MOVZ W16,#lo + MOVK W16,#hi,lsl#16 + ADD reg-reg. This is the
//     halfword-positional encoding (ARM ARM C6.2.193 / C6.2.194) that
//     the round-8 endian-parity claim hinges on.
//   - i32 add reg-reg and ret
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
  // Wide-immediate add — does NOT fit imm12 (max 0xFFF = 4095) so the
  // binop RHS routes through valueInReg → materializeImmAny →
  // emitMovImm, producing MOVZ W16,#0x2345 + MOVK W16,#0x1,lsl#16 +
  // ADD reg-reg. This is the path round 8d wants endian-parity-tested.
  Value *k2  = B.CreateAdd(k, B.getInt32(0x12345), "k2");
  B.CreateAlignedStore(k2, P, MaybeAlign(4));
  B.CreateRet(k2);
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

  // Coverage guard: the sample module includes a >16-bit ADD constant
  // (0x12345) that must route through `materializeImmAny` →
  // `emitMovImm` and produce at least one MOVZ + at least one MOVK
  // halfword instruction in the LE stream. If a future change silently
  // sidesteps this path (e.g. by folding the constant or by widening
  // the imm12 fast path beyond what AArch64 actually supports), this
  // test should fail loudly so the >16-bit MOVZ/MOVK encoding remains
  // covered. Opcode masks per ARM ARM C6.2.193 / C6.2.194:
  //   MOVZ (32/64-bit) : top 9 bits = 0b?10100101  → mask 0x7F800000,
  //                                                  value 0x52800000
  //   MOVK (32/64-bit) : top 9 bits = 0b?11100101  → mask 0x7F800000,
  //                                                  value 0x72800000
  // We intentionally accept both 32-bit (sf=0) and 64-bit (sf=1)
  // forms — only the opcode family matters here.
  if (!codeLE.empty()) {
    if ((codeLE.size() % 4) != 0) {
      std::printf("FAIL: code size %zu is not a multiple of 4\n",
                  codeLE.size());
      ++failures;
    } else {
      unsigned movzCount = 0, movkCount = 0;
      for (size_t i = 0; i < codeLE.size(); i += 4) {
        uint32_t w = (uint32_t)codeLE[i]
                   | ((uint32_t)codeLE[i+1] << 8)
                   | ((uint32_t)codeLE[i+2] << 16)
                   | ((uint32_t)codeLE[i+3] << 24);
        if ((w & 0x7F800000u) == 0x52800000u) ++movzCount;
        if ((w & 0x7F800000u) == 0x72800000u) ++movkCount;
      }
      if (movzCount == 0 || movkCount == 0) {
        std::printf("FAIL: expected MOVZ+MOVK in stream, got "
                    "movz=%u movk=%u (the wide-immediate path was "
                    "not exercised)\n", movzCount, movkCount);
        ++failures;
      } else {
        std::printf("OK: stream contains MOVZ x%u + MOVK x%u "
                    "(materializeImmAny exercised)\n",
                    movzCount, movkCount);
      }
    }
  }

  if (failures) {
    std::printf("FAILED: %d case(s)\n", failures);
    return 1;
  }
  std::printf("PASS: light endian/triple parity\n");
  return 0;
}
