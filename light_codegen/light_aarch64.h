// light_aarch64.h — experimental narrow codegen for post-EasyJIT-specialized IR.
//
// Supports a narrow subset (light-backend-only extension):
//   - one function, multi-BB with forward/backward branches
//   - integer or pointer params in x0..x7, float params in s0..s7
//   - static alloca with fixed layout; constant-offset GEP on alloca
//   - load/store i8/i16/i32/i64 with uimm12-scaled offset from sp or from a
//     pointer-arg register; one dynamic scaled index is supported
//   - llvm.memcpy.p0.p0.i64 with ConstantInt size <= 32 bytes, no overlap
//   - icmp (eq/ne/slt/sle/sgt/sge/ult/ule/ugt/uge) — must be fused into
//     the following conditional br; icmp-as-GPR is not supported
//   - br i1 / br label, phi (lowered via predecessor-edge copy)
//   - add/sub/mul/and/or/xor/shl/lshr/ashr
//   - narrow scalar-float subset: load/store float, llvm.fmuladd.f32,
//     fptosi float->i32
//   - sext/zext/trunc between i1/i8/i16/i32/i64
//   - ret i32 / ret i64 / ret void  (with sp restore)
// Everything else -> Status::Unsupported.
//
// Endian: aarch64 (LE data) AND aarch64_be (BE data) are both supported
// for the IR subset above. Instruction stream is always LE (ARM ARM
// B2.6.2); Writer::emit writes explicit LE bytes so emission works on
// any host. See AARCH64_BE.md.
//
// The emitter writes AArch64 little-endian instruction words into a user-
// supplied buffer, returning the byte length on success.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace llvm { class Function; }

namespace light {

enum class Status { Ok, Unsupported, TooLarge, NotAarch64LE };

struct Result {
  Status status = Status::Ok;
  std::size_t codeBytes = 0;
  std::string reason;
};

struct GlobalSymbol {
  const char *name;
  const void *address;
};

Result emit(const llvm::Function &Fn, std::uint8_t *buf, std::size_t bufCap,
            const GlobalSymbol *globals = nullptr, std::size_t nglobals = 0);

void *compile(const llvm::Function &Fn, Result &out,
              const GlobalSymbol *globals = nullptr, std::size_t nglobals = 0);

} // namespace light
