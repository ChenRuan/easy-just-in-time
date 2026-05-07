// Experimental narrow AArch64 emitter — round-6 extension.
//
// Target IR shape (derived from actual post-EasyJIT-specialization dumps):
//   - 1 function, ≤8 integer/pointer args in x0..x7, float args in s0..s7
//   - multi-BB with forward/backward branches
//   - single alloca (fixed size, compile-time known), GEP of alloca with
//     constant offsets only
//   - load/store i8/i16/i32/i64 from/to alloca-relative addresses or from a
//     pointer-arg register (with constant offset)
//   - llvm.memcpy.p0.p0.i64 with ConstantInt size, no overlap, lowered to
//     a sequence of LDR/STR pairs (at most 32 bytes; else Unsupported)
//   - icmp (eq/ne/slt/sle/sgt/sge/ult/ule/ugt/uge) fused into the following
//     conditional branch (icmp result is not otherwise used)
//   - br i1 / br label (no switch/invoke)
//   - phi i32 with N predecessors, lowered by pre-assigning one scratch
//     register and copying at each predecessor's terminator
//   - add/sub/mul/and/or/xor/shl/lshr/ashr  (reg-reg or reg-imm12; small
//     immediates up to 16 bits are materialized via MOVZ into x16)
//   - narrow scalar-float subset: load/store float, llvm.fmuladd.f32,
//     and fptosi float->i32
//   - sext/zext/trunc between i1/i8/i16/i32/i64
//   - ret (restores sp if a frame was allocated)
//
// Everything else -> Status::Unsupported. The emitter does not have a
// general SSA spill/reload allocator; it does, however, extend its GPR
// scratch pool with saved callee-saved registers x19..x28.
//
// Endian model (round-8 extension — aarch64_be enablement):
//   A. Instruction stream is always LE (ARM ARM B2.6.2). Writer::emit
//      writes explicit LE bytes, so emission is correct on any host.
//   B. Data LDR/STR honour target data endian (SCTLR_EL1.EE); producer
//      and consumer are in the same process → symmetric → no byte-swap
//      needed in the emitter.
//   C. MOVZ/MOVK halfwords are positional (hw field), not byte-
//      addressed → endian-neutral.
//   D. Sub-word (i8/i16) load/store uses LDRB/LDRH/STRB/STRH. ZExt is a
//      no-op after those loads; SExt emits SBFM so signed fields are handled
//      explicitly rather than relying on target endian details.
// See AARCH64_BE.md for the full correctness argument.

#include "light_aarch64.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DataLayout.h"

#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace light;
using namespace llvm;

namespace {

// ----------------------------- encoders -----------------------------

// ADD/SUB (imm12). Used with rd/rn=31 for SP form (when opcode is ADD/SUB
// imm family, reg 31 means SP, not XZR). Instruction word packing is LE
// (see Writer::emit), independent of target data-endian.
static uint32_t encAddSubImm(bool sub, bool is64, unsigned rd, unsigned rn,
                             unsigned imm12) {
  uint32_t op = sub ? 0x51000000u : 0x11000000u;
  if (is64) op |= 0x80000000u;
  return op | ((imm12 & 0xFFFu) << 10) | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encAddSubReg(bool sub, bool is64, unsigned rd, unsigned rn,
                             unsigned rm) {
  uint32_t op = sub ? 0x4B000000u : 0x0B000000u;
  if (is64) op |= 0x80000000u;
  return op | ((rm & 0x1Fu) << 16) | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encLogicReg(unsigned kind, bool is64, unsigned rd, unsigned rn,
                            unsigned rm) {
  static const uint32_t base[3] = {0x0A000000u, 0x2A000000u, 0x4A000000u};
  uint32_t op = base[kind];
  if (is64) op |= 0x80000000u;
  return op | ((rm & 0x1Fu) << 16) | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encMul(bool is64, unsigned rd, unsigned rn, unsigned rm) {
  uint32_t op = 0x1B000000u | (31u << 10);
  if (is64) op |= 0x80000000u;
  return op | ((rm & 0x1Fu) << 16) | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encMovz16(bool is64, unsigned rd, uint16_t imm16) {
  uint32_t op = 0x52800000u;
  if (is64) op |= 0x80000000u;
  return op | ((uint32_t)imm16 << 5) | (rd & 0x1Fu);
}
static uint32_t encMovkHw32(unsigned rd, uint16_t imm16, unsigned hw) {
  return 0x72800000u | ((hw & 1u) << 21)
       | ((uint32_t)imm16 << 5) | (rd & 0x1Fu);
}
// MOVZ with hw-shift (hw ∈ {0,1,2,3}; shift = hw * 16). 64-bit form only.
// LE-neutral: address value is just a 64-bit integer; the instruction
// stream itself is unconditionally LE regardless of data endian.
static uint32_t encMovzHw64(unsigned rd, uint16_t imm16, unsigned hw) {
  return 0xD2800000u | ((hw & 3u) << 21) | ((uint32_t)imm16 << 5) | (rd & 0x1Fu);
}
// MOVK with hw-shift (keeps other halfwords unchanged). 64-bit form.
static uint32_t encMovkHw64(unsigned rd, uint16_t imm16, unsigned hw) {
  return 0xF2800000u | ((hw & 3u) << 21) | ((uint32_t)imm16 << 5) | (rd & 0x1Fu);
}
// ADD (extended register), 64-bit. option ∈ {2=UXTW, 6=SXTW, 3=UXTX (no-op
// for 64-bit), 7=SXTX (signed no-op for 64-bit)}; imm3 is the LSL applied
// AFTER the extension, ∈ 0..4. Used to lower `base + idx * scale` where
// base is a 64-bit GPR (x17), idx is a 32- or 64-bit GPR, and scale is a
// power of two (idx is shifted by log2(scale)).
//
// Encoding ref: ARM ARM C6.2.5 ADD (extended register).
//   sf=1 op=0 S=0 | 01011 001 | Rm | option(3) | imm3(3) | Rn | Rd
static uint32_t encAddExtReg64(unsigned rd, unsigned rn, unsigned rm,
                               unsigned option, unsigned imm3) {
  return 0x8B200000u
       | ((rm & 0x1Fu) << 16)
       | ((option & 7u) << 13)
       | ((imm3 & 7u) << 10)
       | ((rn & 0x1Fu) << 5)
       | (rd & 0x1Fu);
}
static uint32_t encMovReg(bool is64, unsigned rd, unsigned rm) {
  return encLogicReg(1, is64, rd, 31u, rm);
}
static uint32_t encLslImm(bool is64, unsigned rd, unsigned rn, unsigned sh) {
  unsigned regBits = is64 ? 64u : 32u;
  unsigned immr = (regBits - sh) & (regBits - 1);
  unsigned imms = (regBits - 1) - sh;
  uint32_t op = 0x53000000u;
  if (is64) op |= 0x80400000u;
  return op | ((immr & 0x3Fu) << 16) | ((imms & 0x3Fu) << 10)
           | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encLsrImm(bool is64, unsigned rd, unsigned rn, unsigned sh) {
  unsigned regBits = is64 ? 64u : 32u;
  unsigned immr = sh & (regBits - 1);
  unsigned imms = regBits - 1;
  uint32_t op = 0x53000000u;
  if (is64) op |= 0x80400000u;
  return op | ((immr & 0x3Fu) << 16) | ((imms & 0x3Fu) << 10)
           | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encAsrImm(bool is64, unsigned rd, unsigned rn, unsigned sh) {
  unsigned regBits = is64 ? 64u : 32u;
  unsigned immr = sh & (regBits - 1);
  unsigned imms = regBits - 1;
  uint32_t op = 0x13000000u;
  if (is64) op |= 0x80400000u;
  return op | ((immr & 0x3Fu) << 16) | ((imms & 0x3Fu) << 10)
           | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}

// SBFM/UBFM aliases for sign/zero extension of sub-word values.
// SXT[BH] Wd, Wn = SBFM Wd, Wn, #0, #(7|15)
// UXT[BH] Wd, Wn = UBFM Wd, Wn, #0, #(7|15)
static uint32_t encExtendBits(bool sign, bool to64, unsigned rd, unsigned rn,
                              unsigned fromBits) {
  uint32_t op = sign ? 0x13000000u : 0x53000000u;
  if (to64) op |= 0x80400000u;
  unsigned imms = fromBits - 1;
  return op | ((0u & 0x3Fu) << 16) | ((imms & 0x3Fu) << 10)
            | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}

// LDR/STR (unsigned-offset, uimm12). size: 0=8b, 1=16b, 2=32b, 3=64b. Reads/writes
// happen in target data-endian (SCTLR_EL1.EE). Because JIT'd code reads
// the same bytes its producer wrote (same process, same endian), this is
// correct on both aarch64 and aarch64_be.
static uint32_t encLdrStrUI(bool load, unsigned size, unsigned rt,
                            unsigned rn, unsigned imm12) {
  uint32_t op = 0x39000000u;          // size=00 (8-bit) base
  op |= ((uint32_t)size << 30);       // 10=32b, 11=64b
  op |= (load ? 0x00400000u : 0u);
  return op | ((imm12 & 0xFFFu) << 10) | ((rn & 0x1Fu) << 5) | (rt & 0x1Fu);
}
static uint32_t encFpLdrStrUIS(bool load, unsigned rt, unsigned rn,
                               unsigned imm12) {
  uint32_t op = load ? 0xBD400000u : 0xBD000000u;
  return op | ((imm12 & 0xFFFu) << 10) | ((rn & 0x1Fu) << 5) | (rt & 0x1Fu);
}
// Double-precision (Dt) variant: same encoding family but size=11 (bits
// 30-31). LDR Dt: 0xFD40_0000 base, STR Dt: 0xFD00_0000 base. The
// imm12 field is scaled by 8 (for D) instead of 4 (for S); callers
// pass the already-scaled (i.e. divided) immediate.
static uint32_t encFpLdrStrUID(bool load, unsigned rt, unsigned rn,
                               unsigned imm12) {
  uint32_t op = load ? 0xFD400000u : 0xFD000000u;
  return op | ((imm12 & 0xFFFu) << 10) | ((rn & 0x1Fu) << 5) | (rt & 0x1Fu);
}
static uint32_t encFmaddS(unsigned rd, unsigned rn, unsigned rm, unsigned ra) {
  return 0x1F000000u | ((rm & 0x1Fu) << 16) | ((ra & 0x1Fu) << 10)
                    | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encFcvtzsWS(unsigned rd, unsigned rn) {
  return 0x1E380000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// FCVTZS Wd, Dn — round toward zero, double-precision source, 32-bit
// signed integer destination. Encoding: sf=0 type=01 rmode=11 opc=000
// → 0x1E78_0000 base. Out-of-range / NaN inputs saturate per ARM
// (INT_MIN / INT_MAX / 0). ARM ARM C6.2.61.
static uint32_t encFcvtzsWD(unsigned rd, unsigned rn) {
  return 0x1E780000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// Round-8i: scalar FP<->int conversion family completed below.
// Encoding template (ARM ARM C4-1, "Floating-point <-> integer
// conversions"):
//   sf [bit31] | 0011110 [30:24] | type [23:22] | 1 [21] |
//   rmode [20:19] | opc [18:16] | 000000 [15:10] | Rn [9:5] | Rd [4:0]
//
//   FCVTZS:  rmode=11 opc=000   (signed truncate)
//   FCVTZU:  rmode=11 opc=001   (unsigned truncate)
//   SCVTF :  rmode=00 opc=010   (signed int -> FP)
//   UCVTF :  rmode=00 opc=011   (unsigned int -> FP)
//   FMOV  :  rmode=00 opc=110/111 (reinterpret int<->FP, no conversion)
//
// `type=00` selects the single-precision FP register class (S);
// `type=01` selects double (D). `sf=0` selects the 32-bit GPR class
// (W); `sf=1` selects the 64-bit class (X).
//
// FCVTZS Xd,Sn  (sf=1 type=00 rmode=11 opc=000)
static uint32_t encFcvtzsXS(unsigned rd, unsigned rn) {
  return 0x9E380000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// FCVTZS Xd,Dn  (sf=1 type=01 rmode=11 opc=000)
static uint32_t encFcvtzsXD(unsigned rd, unsigned rn) {
  return 0x9E780000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// FCVTZU Wd,Sn  (sf=0 type=00 rmode=11 opc=001)
static uint32_t encFcvtzuWS(unsigned rd, unsigned rn) {
  return 0x1E390000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// FCVTZU Wd,Dn  (sf=0 type=01 rmode=11 opc=001)
static uint32_t encFcvtzuWD(unsigned rd, unsigned rn) {
  return 0x1E790000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// FCVTZU Xd,Sn  (sf=1 type=00 rmode=11 opc=001)
static uint32_t encFcvtzuXS(unsigned rd, unsigned rn) {
  return 0x9E390000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// FCVTZU Xd,Dn  (sf=1 type=01 rmode=11 opc=001)
static uint32_t encFcvtzuXD(unsigned rd, unsigned rn) {
  return 0x9E790000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// SCVTF Sd,Wn  (sf=0 type=00 rmode=00 opc=010)
static uint32_t encScvtfSW(unsigned rd, unsigned rn) {
  return 0x1E220000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// SCVTF Sd,Xn  (sf=1 type=00 rmode=00 opc=010)
static uint32_t encScvtfSX(unsigned rd, unsigned rn) {
  return 0x9E220000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// SCVTF Dd,Wn  (sf=0 type=01 rmode=00 opc=010)
static uint32_t encScvtfDW(unsigned rd, unsigned rn) {
  return 0x1E620000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// SCVTF Dd,Xn  (sf=1 type=01 rmode=00 opc=010)
static uint32_t encScvtfDX(unsigned rd, unsigned rn) {
  return 0x9E620000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// UCVTF Sd,Wn  (sf=0 type=00 rmode=00 opc=011)
static uint32_t encUcvtfSW(unsigned rd, unsigned rn) {
  return 0x1E230000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// UCVTF Sd,Xn  (sf=1 type=00 rmode=00 opc=011)
static uint32_t encUcvtfSX(unsigned rd, unsigned rn) {
  return 0x9E230000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// UCVTF Dd,Wn  (sf=0 type=01 rmode=00 opc=011)
static uint32_t encUcvtfDW(unsigned rd, unsigned rn) {
  return 0x1E630000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// UCVTF Dd,Xn  (sf=1 type=01 rmode=00 opc=011)
static uint32_t encUcvtfDX(unsigned rd, unsigned rn) {
  return 0x9E630000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// FCVT — change FP precision (ARM ARM C6.2.62).
// Encoding: 0001 1110 0 type[1] 1 0001 opc[2] 10000 Rn Rd
//   FCVT Dd,Sn  (single -> double): type=00 opc=01 → 0x1E22_C000
//   FCVT Sd,Dn  (double -> single): type=01 opc=00 → 0x1E62_4000
static uint32_t encFcvtDS(unsigned rd, unsigned rn) {
  return 0x1E22C000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encFcvtSD(unsigned rd, unsigned rn) {
  return 0x1E624000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// FMOV reinterpret (no value change), int <- FP.
//   FMOV Wd,Sn  (sf=0 type=00 rmode=00 opc=110) → 0x1E26_0000
//   FMOV Xd,Dn  (sf=1 type=01 rmode=00 opc=110) → 0x9E66_0000
// (The reverse direction — FMOV Sd,Wn / FMOV Dd,Xn — already exists
// as `encFmovSFromW` / `encFmovDFromX` since the ConstantFP path needs
// them.)
static uint32_t encFmovWFromS(unsigned rd, unsigned rn) {
  return 0x1E260000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encFmovXFromD(unsigned rd, unsigned rn) {
  return 0x9E660000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encFmovImm2S(unsigned rd) {
  return 0x1E201000u | (rd & 0x1Fu);
}
static uint32_t encFmovZeroS(unsigned rd) {
  return 0x1E2703E0u | (rd & 0x1Fu);
}
static uint32_t encFmovRegS(unsigned rd, unsigned rn) {
  return 0x1E204000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encFmovSFromW(unsigned rd, unsigned rn) {
  return 0x1E270000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// Double-precision FMOV variants (ARM ARM C6.2.103/104).
//   FMOV Dd, Dn   : type=01 opc=00 → 0x1E60_4000
//   FMOV Dd, Xn   : sf=1 type=01 rmode=00 opc=111 → 0x9E67_0000
//   FMOV Xd, Dn   : sf=1 type=01 rmode=00 opc=110 → 0x9E66_0000
//   FMOV Dd, XZR  : same as FMOV Dd, Xn with Xn=31 → +0.0  (no separate
//                   FMOV-imm zero needed, ditto for the S form).
static uint32_t encFmovRegD(unsigned rd, unsigned rn) {
  return 0x1E604000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encFmovDFromX(unsigned rd, unsigned rn) {
  return 0x9E670000u | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// (FMOV Xd from Dn lives above as `encFmovXFromD`, used by both
// bitcast double->i64 and fptoui-via-bitcast lowerings.)
// Single-precision scalar FP arithmetic (ARM ARM C6.2.79..C6.2.82).
// Encoding family: 0001_1110_0010_mmmmm_<op>_nnnnn_ddddd, type=00 → single.
//   FADD: opc=001010 → 0x1E20_2800 base
//   FSUB: opc=001110 → 0x1E20_3800 base
//   FMUL: opc=000010 → 0x1E20_0800 base
//   FDIV: opc=000110 → 0x1E20_1800 base
static uint32_t encFaddS(unsigned rd, unsigned rn, unsigned rm) {
  return 0x1E202800u | ((rm & 0x1Fu) << 16) | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encFsubS(unsigned rd, unsigned rn, unsigned rm) {
  return 0x1E203800u | ((rm & 0x1Fu) << 16) | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encFmulS(unsigned rd, unsigned rn, unsigned rm) {
  return 0x1E200800u | ((rm & 0x1Fu) << 16) | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encFdivS(unsigned rd, unsigned rn, unsigned rm) {
  return 0x1E201800u | ((rm & 0x1Fu) << 16) | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// Double-precision counterparts (type=01 → bit 22 set, vs single's
// type=00). Encoding: 0001_1110_0110_mmmmm_<op>_nnnnn_ddddd.
//   FADD D: 0x1E60_2800   FSUB D: 0x1E60_3800
//   FMUL D: 0x1E60_0800   FDIV D: 0x1E60_1800
static uint32_t encFaddD(unsigned rd, unsigned rn, unsigned rm) {
  return 0x1E602800u | ((rm & 0x1Fu) << 16) | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encFsubD(unsigned rd, unsigned rn, unsigned rm) {
  return 0x1E603800u | ((rm & 0x1Fu) << 16) | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encFmulD(unsigned rd, unsigned rn, unsigned rm) {
  return 0x1E600800u | ((rm & 0x1Fu) << 16) | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encFdivD(unsigned rd, unsigned rn, unsigned rm) {
  return 0x1E601800u | ((rm & 0x1Fu) << 16) | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
// FCMP Sn, Sm (scalar single, ARM ARM C6.2.84). Sets NZCV per IEEE-754:
//   ordered <  → NZCV=1000   ordered ==  → 0110
//   ordered >  → NZCV=0010   unordered   → 0011  (V=1 for NaN)
// Encoding: 0001_1110_0010_mmmmm_001000_nnnnn_00000.
static uint32_t encFcmpS(unsigned rn, unsigned rm) {
  return 0x1E202000u | ((rm & 0x1Fu) << 16) | ((rn & 0x1Fu) << 5);
}
// FCMP Dn, Dm — same NZCV semantics as FCMP S, type=01.
// Encoding: 0001_1110_0110_mmmmm_001000_nnnnn_00000 → 0x1E60_2000.
static uint32_t encFcmpD(unsigned rn, unsigned rm) {
  return 0x1E602000u | ((rm & 0x1Fu) << 16) | ((rn & 0x1Fu) << 5);
}
// SUBS (imm) — used for CMP imm. 32-bit form.
static uint32_t encSubsImm32(unsigned rn, unsigned imm12) {
  return 0x71000000u | ((imm12 & 0xFFFu) << 10) | ((rn & 0x1Fu) << 5) | 31u;
}
// SUBS (reg) — CMP reg. 32-bit form.
static uint32_t encSubsReg32(unsigned rn, unsigned rm) {
  return 0x6B000000u | ((rm & 0x1Fu) << 16) | ((rn & 0x1Fu) << 5) | 31u;
}
// B.cond placeholder (imm19 backpatched later).
static uint32_t encBcondStub(unsigned cond) {
  return 0x54000000u | (cond & 0xFu);
}
// B placeholder.
static uint32_t encBStub() { return 0x14000000u; }

static constexpr uint32_t kRet = 0xD65F03C0u;

// ---------------------- helpers & state ----------------------

struct Writer {
  uint8_t *buf; size_t cap; size_t pos = 0;
  bool emit(uint32_t w) {
    if (pos + 4 > cap) return false;
    // AArch64 instruction stream is unconditionally little-endian (ARM ARM
    // B2.6.2) regardless of the ABI's data-endian. Write explicit LE bytes
    // so this is correct on any host: a BE host writing into a BE process's
    // mmap would otherwise store BE-ordered instruction bytes, which the
    // CPU would then fetch as the wrong 32-bit word. See AARCH64_BE.md.
    buf[pos + 0] = (uint8_t)(w >>  0);
    buf[pos + 1] = (uint8_t)(w >>  8);
    buf[pos + 2] = (uint8_t)(w >> 16);
    buf[pos + 3] = (uint8_t)(w >> 24);
    pos += 4;
    return true;
  }
  void patch32(size_t at, uint32_t w) {
    // Same LE-byte-stream invariant as emit().
    buf[at + 0] = (uint8_t)(w >>  0);
    buf[at + 1] = (uint8_t)(w >>  8);
    buf[at + 2] = (uint8_t)(w >> 16);
    buf[at + 3] = (uint8_t)(w >> 24);
  }
};

// A pointer-typed SSA value either lives in a GPR, is a (sp + offset)
// expression (alloca + constant GEP chain), is a fully-baked constant
// host address (e.g. `inttoptr i64 0xCAFE to ptr`, used by EasyJIT to
// inline a snapshot/array base after specialization), or is an Absolute
// base plus one runtime-variable scaled index (used for `array[idx]`-style
// access into a host buffer whose base is known, or into a forwarded pointer
// argument).
//
// The scaled-index forms lower `getelementptr` patterns with up to two
// runtime indices and power-of-two element scales. Round 8k extended
// the original single-term design (`base + idx0*scale0`) to up to TWO
// dynamic terms (`base + idx0*scale0 + idx1*scale1 + constOff`), which
// covers nested 2D-array GEPs like `base[i][j]`. Each term lowers to a
// single `ADD x17, x17, Wm/Xm, ext #shift` when log2(scale) <= 4, or
// a 3-instruction extend+LSL+ADD sequence when 4 < log2(scale) <= 12,
// using x16 as the per-term temp.
struct PtrLoc {
  enum Kind { InReg, StackRel, Absolute, AbsoluteScaledIndex, InRegScaledIndex } kind = InReg;
  unsigned reg = 0;     // for InReg
  int32_t  spOff = 0;   // for StackRel
  uint64_t addr = 0;    // for Absolute* forms (addr+const_off)

  // For *ScaledIndex only. Up to 2 terms (round 8k).
  struct Term {
    const llvm::Value *value = nullptr;
    uint32_t scaleLog2 = 0;
    bool is64 = false;
    bool isSigned = true;
  };
  Term terms[2] = {};
  unsigned numTerms = 0;
};

struct ICmpFusion {
  const ICmpInst *I = nullptr;
  CmpInst::Predicate pred = CmpInst::ICMP_EQ;
  unsigned lhsReg = 0;
  // Either rhs is imm12 (>=0) or in a reg.
  int rhsImm = -1;
  unsigned rhsReg = 0;
  bool is64 = false;
};

// translate llvm icmp predicate to aarch64 cond code for the TRUE edge.
static unsigned icmpToCond(CmpInst::Predicate p) {
  switch (p) {
  case CmpInst::ICMP_EQ:  return 0x0; // EQ
  case CmpInst::ICMP_NE:  return 0x1; // NE
  case CmpInst::ICMP_UGT: return 0x8; // HI
  case CmpInst::ICMP_UGE: return 0x2; // HS
  case CmpInst::ICMP_ULT: return 0x3; // LO
  case CmpInst::ICMP_ULE: return 0x9; // LS
  case CmpInst::ICMP_SGT: return 0xC; // GT
  case CmpInst::ICMP_SGE: return 0xA; // GE
  case CmpInst::ICMP_SLT: return 0xB; // LT
  case CmpInst::ICMP_SLE: return 0xD; // LE
  default: return 0xE; // AL = unconditional; should not occur
  }
}

static int asImm12(const Value *V) {
  auto *CI = dyn_cast<ConstantInt>(V);
  if (!CI) return -1;
  const auto &AP = CI->getValue();
  if (AP.isNegative() || AP.getActiveBits() > 12) return -1;
  return (int)CI->getZExtValue();
}

// FCmp fusion record: like ICmpFusion but for scalar single-precision
// FP compares. We support an ordered subset of LLVM FCmp predicates and
// reject everything else with an "fcmp predicate" reason. Unordered /
// NaN-sensitive predicates (UEQ, UNE, UGT, UGE, ULT, ULE, ORD, UNO,
// AlwaysTrue/False, ONE) are intentionally not handled: AArch64 has no
// single condition code that captures "ordered AND not equal" without
// CCMP or two branches, and the unordered family would require the
// caller to opt into NaN propagation semantics we have not validated.
struct FCmpFusion {
  const FCmpInst *I = nullptr;
  CmpInst::Predicate pred = CmpInst::FCMP_OEQ;
  // Operands are stored as IR Values (NOT pre-materialized FP regs) so
  // that any FP-constant materialization that happens *between* the
  // FCmp and its consumer (br/select) cannot clobber the S31 scratch
  // slot used by `valueInFpReg`. The consumer materializes lhs/rhs
  // immediately before emitting FCMP, when we know the scratch is
  // fresh. Round 8g hardening — see LightBackend_LIMITATIONS.md.
  const Value *lhs = nullptr;
  const Value *rhs = nullptr;
};

// Translate an ordered LLVM FCmp predicate to an AArch64 condition code
// for the TRUE edge. Returns 0xFF when the predicate is not in the
// supported ordered subset; callers MUST check for that and fail.
//
// AArch64 FCMP NZCV table (ARM ARM C6.2.84):
//   ordered <  : N=1 Z=0 C=0 V=0    ordered == : N=0 Z=1 C=1 V=0
//   ordered >  : N=0 Z=0 C=1 V=0    unordered  : N=0 Z=0 C=1 V=1
//
// The mapping below is the ARM-recommended ordered-only set. In
// particular OLT uses MI (N==1) — NOT LT (N!=V) — because LT also
// fires when NaN sets V=1, which would silently match unordered.
// Symmetrically, OLE uses LS (C==0 || Z==1), and OGT uses GT
// (N==V && !Z), both of which exclude the unordered NZCV pattern.
//
// For each supported predicate, `cond ^ 1` (the negation we use to
// branch over the TRUE arm) yields a condition that catches both
// "ordered with the opposite relation" AND "unordered", which is the
// correct behaviour: when the FCmp is false (including unordered),
// take the FALSE edge.
static unsigned fcmpToCond(CmpInst::Predicate p) {
  switch (p) {
  case CmpInst::FCMP_OEQ: return 0x0; // EQ
  case CmpInst::FCMP_OGT: return 0xC; // GT
  case CmpInst::FCMP_OGE: return 0xA; // GE
  case CmpInst::FCMP_OLT: return 0x4; // MI (NOT LT — LT triggers on NaN)
  case CmpInst::FCMP_OLE: return 0x9; // LS
  default:                return 0xFFu;
  }
}

// Compile-time GEP offset for a constant-index GEP on a sized aggregate.
// Returns false if any index is non-constant or the type is unhandled.
static bool constGepOffset(const GEPOperator *GEP, const DataLayout &DL,
                           int64_t &offOut) {
  APInt off(64, 0);
  if (!GEP->accumulateConstantOffset(DL, off)) return false;
  if (off.getActiveBits() > 31) return false;
  offOut = off.getSExtValue();
  return true;
}

// Recursively resolve a pointer Value into a PtrLoc, walking through
// inttoptr ConstantExprs, constant- and instruction-form GEPs (constant
// indices only), and bitcasts. A GlobalVariable base whose host address
// is registered in the EasyJIT globals table is also resolved, since
// from the emitter's point of view it is just another absolute address
// (round-9 generalisation: matches the ad-hoc external-global path the
// load/store handlers used to do inline). Returns true on success.
//
// This is the engine that lets `inttoptr (i64 0xCAFE to ptr)` and
// `@__easy_snapshot_struct_array` (registered host address) flow into a
// normal load/store as PtrLoc::Absolute, possibly with constant offsets
// applied on top by inline ConstantExpr GEPs (e.g.
// `getelementptr (%struct, ptr inttoptr(...), 0, 1)`).
//
// Endian: the resolved address is just a 64-bit integer baked into the
// instruction stream via MOVZ/MOVK halfwords (positional, endian-neutral).
// The subsequent LDR/STR honours target data endian, but the host C++
// producer wrote target-endian bytes into that same address (same process,
// same SCTLR_EL1.EE) → symmetric → correct on aarch64 and aarch64_be.
static bool resolvePtrLocChain(const Value *P, const DataLayout &DL,
                               const std::unordered_map<const Value *, PtrLoc> &known,
                               const GlobalSymbol *globals, size_t nglobals,
                               PtrLoc &out) {
  auto resolveGV = [&](const GlobalVariable *GV) -> const void * {
    if (!globals || nglobals == 0) return nullptr;
    llvm::StringRef N = GV->getName();
    for (size_t i = 0; i < nglobals; ++i) {
      if (!globals[i].name) continue;
      if (N == globals[i].name) return globals[i].address;
    }
    return nullptr;
  };

  while (P) {
    auto it = known.find(P);
    if (it != known.end()) { out = it->second; return true; }

    if (auto *GV = dyn_cast<GlobalVariable>(P)) {
      if (const void *Host = resolveGV(GV)) {
        out = PtrLoc{};
        out.kind = PtrLoc::Absolute;
        out.addr = (uint64_t)reinterpret_cast<uintptr_t>(Host);
        return true;
      }
      return false;
    }

    if (auto *CE = dyn_cast<ConstantExpr>(P)) {
      if (CE->getOpcode() == Instruction::IntToPtr) {
        if (auto *CI = dyn_cast<ConstantInt>(CE->getOperand(0))) {
          out = PtrLoc{};
          out.kind = PtrLoc::Absolute;
          out.addr = CI->getZExtValue();
          return true;
        }
        return false;
      }
      // BitCast/AddrSpaceCast falls through to the GEPOperator/BitCastOperator
      // handling below, since GEPOperator/BitCastOperator wrap ConstantExprs
      // of the corresponding opcode.
    }

    if (auto *GEP = dyn_cast<GEPOperator>(P)) {
      APInt off(64, 0);
      if (!GEP->accumulateConstantOffset(DL, off)) return false;
      if (off.getActiveBits() > 32) return false;
      PtrLoc base;
      if (!resolvePtrLocChain(GEP->getPointerOperand(), DL, known,
                              globals, nglobals, base))
        return false;
      out = base;
      switch (out.kind) {
        case PtrLoc::StackRel: out.spOff += (int32_t)off.getSExtValue(); return true;
        case PtrLoc::Absolute: out.addr  += (uint64_t)(int64_t)off.getSExtValue(); return true;
        case PtrLoc::AbsoluteScaledIndex:
          // Layering a constant-offset GEP on top of an already-indexed
          // base just shifts the constant part. Keeps idxValue/scale.
          out.addr += (uint64_t)(int64_t)off.getSExtValue();
          return true;
        case PtrLoc::InRegScaledIndex:
          out.spOff += (int32_t)off.getSExtValue();
          return true;
        case PtrLoc::InReg:
          out.spOff += (int32_t)off.getSExtValue();
          return true;
      }
      return false;
    }

    if (auto *BC = dyn_cast<BitCastOperator>(P)) {
      P = BC->getOperand(0);
      continue;
    }

    return false;
  }
  return false;
}

// Inspect a GEP and decide whether it can be expressed as
//
//   base_addr + const_off + idx0 * scale0 [+ idx1 * scale1]
//
// where base_addr is some PtrLoc::Absolute or PtrLoc::InReg (after
// going through resolvePtrLocChain), each non-constant GEP index is
// an i32 or i64 integer Value (or a sext/zext from one), at most TWO
// indices are non-constant, all other indices are constant, and each
// dynamic scale is a power of two with log2 <= 12. Round 8k lifted
// the original "one runtime index, scale <= 8" restriction.
//
// On success fills `out` with kind=AbsoluteScaledIndex/InRegScaledIndex
// and numTerms in {1, 2}, and returns true. On any miss returns false
// (caller falls back / emits Unsupported).
//
// To keep dependencies minimal we walk indices ourselves: at each step
// the "indexed type" tells us the stride (struct member offset for
// struct, element size for array/vector/pointer-element).
static bool resolveDynScaledGep(const GEPOperator *GEP, const DataLayout &DL,
                                const std::unordered_map<const Value *, PtrLoc> &known,
                                const GlobalSymbol *globals, size_t nglobals,
                                PtrLoc &out) {
  // Resolve the base. The base must NOT itself already be a scaled-index
  // form (we only support one chain of runtime indices per pointer
  // location). It must reduce to a flat Absolute address or a forwarded
  // pointer in a GPR.
  PtrLoc base;
  if (!resolvePtrLocChain(GEP->getPointerOperand(), DL, known,
                          globals, nglobals, base))
    return false;
  if (base.kind != PtrLoc::Absolute && base.kind != PtrLoc::InReg) return false;

  Type *Cur = GEP->getSourceElementType();
  // First index applies to the source element type as if it were an
  // array. Subsequent indices walk into structs/arrays/vectors.
  int64_t constOff = 0;
  // Up to two pending dynamic terms (idx Value + element size).
  struct Pending { const Value *v; uint64_t scale; };
  Pending pend[2] = {};
  unsigned nPend = 0;

  auto pushDyn = [&](const Value *v, uint64_t elemSize) -> bool {
    if (elemSize == 0) return false;
    if (elemSize & (elemSize - 1)) return false; // PoT only
    if (nPend >= 2) return false;
    // log2 <= 12 (scale <= 4096) keeps the materializer's LSL imm6 sane.
    uint64_t s = elemSize; unsigned log2 = 0;
    while (s > 1) { s >>= 1; ++log2; }
    if (log2 > 12) return false;
    pend[nPend++] = {v, elemSize};
    return true;
  };

  unsigned NumIdx = GEP->getNumIndices();
  if (NumIdx == 0) return false;

  for (unsigned i = 0; i < NumIdx; ++i) {
    Value *Idx = GEP->getOperand(1 + i);
    if (i == 0) {
      // Stride = sizeof(SourceElementType).
      uint64_t elemSize = DL.getTypeAllocSize(Cur);
      if (auto *CI = dyn_cast<ConstantInt>(Idx)) {
        constOff += (int64_t)CI->getSExtValue() * (int64_t)elemSize;
      } else {
        if (!pushDyn(Idx, elemSize)) return false;
      }
      continue;
    }
    // Subsequent indices walk into Cur.
    if (auto *ST = dyn_cast<StructType>(Cur)) {
      auto *CI = dyn_cast<ConstantInt>(Idx);
      if (!CI) return false; // struct indices must be constant
      const StructLayout *SL = DL.getStructLayout(ST);
      uint64_t fi = CI->getZExtValue();
      if (fi >= ST->getNumElements()) return false;
      constOff += (int64_t)SL->getElementOffset((unsigned)fi);
      Cur = ST->getElementType((unsigned)fi);
      continue;
    }
    if (auto *AT = dyn_cast<ArrayType>(Cur)) {
      uint64_t elemSize = DL.getTypeAllocSize(AT->getElementType());
      if (auto *CI = dyn_cast<ConstantInt>(Idx)) {
        constOff += (int64_t)CI->getSExtValue() * (int64_t)elemSize;
      } else {
        if (!pushDyn(Idx, elemSize)) return false;
      }
      Cur = AT->getElementType();
      continue;
    }
    // Vector / scalable / other: out of scope.
    return false;
  }

  if (nPend == 0) {
    // No runtime index — caller should have used the const-offset path.
    return false;
  }

  // Build the result.
  out = PtrLoc{};
  out.kind       = (base.kind == PtrLoc::Absolute)
      ? PtrLoc::AbsoluteScaledIndex
      : PtrLoc::InRegScaledIndex;
  out.reg        = base.reg;
  out.spOff      = base.spOff + (int32_t)constOff;
  out.addr       = base.addr + (uint64_t)constOff;
  out.numTerms   = nPend;

  for (unsigned t = 0; t < nPend; ++t) {
    // Inspect the dynamic index. We accept i32 or i64. If it's a
    // sext/zext from i32, look through it: the underlying GPR holds the
    // i32 value (the cast in this emitter is a no-op alias), so we
    // should emit SXTW/UXTW with that GPR. If it's already i64, use
    // UXTX/SXTX (no-op for 64-bit) — pick UXTX for the unsigned case
    // since the upper bits are already valid in a true i64 register.
    bool idxIs64 = false;
    bool idxSigned = true;
    const Value *idxV = pend[t].v;
    if (auto *Sx = dyn_cast<SExtInst>(idxV)) {
      Value *Src = Sx->getOperand(0);
      if (Src->getType()->isIntegerTy(32)) {
        idxV = Src; idxIs64 = false; idxSigned = true;
      } else if (Src->getType()->isIntegerTy(64)) {
        idxV = Src; idxIs64 = true; idxSigned = true;
      } else {
        return false;
      }
    } else if (auto *Zx = dyn_cast<ZExtInst>(idxV)) {
      Value *Src = Zx->getOperand(0);
      if (Src->getType()->isIntegerTy(32)) {
        idxV = Src; idxIs64 = false; idxSigned = false;
      } else if (Src->getType()->isIntegerTy(64)) {
        idxV = Src; idxIs64 = true; idxSigned = false;
      } else {
        return false;
      }
    } else {
      Type *T = idxV->getType();
      if (T->isIntegerTy(32))      { idxIs64 = false; idxSigned = true; }
      else if (T->isIntegerTy(64)) { idxIs64 = true;  idxSigned = false; }
      else return false;
    }
    uint64_t s = pend[t].scale; unsigned log2 = 0;
    while (s > 1) { s >>= 1; ++log2; }
    out.terms[t].value     = idxV;
    out.terms[t].scaleLog2 = log2;
    out.terms[t].is64      = idxIs64;
    out.terms[t].isSigned  = idxSigned;
  }
  return true;
}

} // namespace

// ------------------------------ emit ------------------------------

Result light::emit(const Function &Fn, uint8_t *buf, size_t cap,
                   const GlobalSymbol *globals, size_t nglobals) {
  Result r;
  Writer W{buf, cap};

  // Host-address resolver for external GlobalVariables. Linear lookup is
  // fine: the table is ~O(few symbols) in practice (the snapshot struct(s)
  // referenced by the JIT'd function). Names are compared against the
  // GlobalVariable spelling in IR, which matches the one registered by
  // EasyJIT's MapGlobals.
  auto resolveGlobal = [&](const GlobalVariable *GV) -> const void * {
    if (!globals || nglobals == 0) return nullptr;
    llvm::StringRef N = GV->getName();
    for (size_t i = 0; i < nglobals; ++i) {
      if (!globals[i].name) continue;
      if (N == globals[i].name) return globals[i].address;
    }
    return nullptr;
  };
  (void)resolveGlobal; // used below; silence unused in early-exit paths.

  const auto &Triple = Fn.getParent()->getTargetTriple();
  // Round-8: accept both aarch64-* (LE data ABI) and aarch64_be-* (BE data
  // ABI). After the Writer::emit LE-byte-stream fix above, the only triple-
  // dependent behaviour is the data-load/store endian, which is symmetric
  // (JIT'd code runs in the same process that produced the data it reads).
  // ILP32 variants (aarch64_32, arm64_32) are still rejected to keep the
  // correctness claim narrow.
  bool isBE = false;
  if (Triple.rfind("aarch64_be", 0) == 0) {
    isBE = true;
  } else if (Triple.rfind("aarch64_32", 0) == 0 ||
             Triple.rfind("arm64_32", 0)   == 0) {
    r.status = Status::NotAarch64;
    r.reason = "ILP32 aarch64 variants not supported";
    return r;
  } else if (Triple.rfind("aarch64", 0) != 0 &&
             Triple.rfind("arm64", 0)   != 0) {
    r.status = Status::NotAarch64;
    r.reason = "triple is not aarch64*";
    return r;
  }
  (void)isBE; // tracked for debuggability; emitter paths are endian-agnostic.
  if (Fn.isDeclaration() || Fn.empty()) {
    r.status = Status::Unsupported; r.reason = "no body"; return r;
  }
  // Round-8j: stack-passed scalar args are now supported, so the
  // pre-flight `arg_size() > 8` gate has been removed. Per-argument
  // classification + overflow-area handling lives in pass 1 below.
  // Vararg / aggregate-by-value are still rejected there.

  const DataLayout &DL = Fn.getParent()->getDataLayout();

  // Conservative pre-scan: only pay the x19..x28 save/restore cost when
  // the function is likely to exceed the caller-saved GPR scratch pool.
  // The real lowering still owns final validation; underestimates fail
  // cleanly through assignReg, while overestimates only cost a slightly
  // larger frame for that function.
  unsigned gprArgCountForScratch = 0;
  unsigned estimatedGprValues = 0;
  for (const Argument &A : Fn.args()) {
    Type *T = A.getType();
    if (T->isIntegerTy() || T->isPointerTy()) {
      if (gprArgCountForScratch < 8) {
        ++gprArgCountForScratch;
      } else if (T->isPointerTy() || T->isIntegerTy(32) ||
                 T->isIntegerTy(64)) {
        ++estimatedGprValues; // stack-arg preload
      }
    }
  }
  for (const BasicBlock &BB : Fn) {
    for (const Instruction &I : BB) {
      if (auto *PN = dyn_cast<PHINode>(&I)) {
        if (PN->getType()->isIntegerTy()) ++estimatedGprValues;
        continue;
      }
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isIntegerTy() || LI->getType()->isPointerTy())
          ++estimatedGprValues;
        continue;
      }
      if (auto *SI = dyn_cast<SelectInst>(&I)) {
        if (SI->getType()->isIntegerTy()) ++estimatedGprValues;
        continue;
      }
      if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
        if (BO->getType()->isIntegerTy()) ++estimatedGprValues;
        continue;
      }
      if (auto *CI = dyn_cast<CastInst>(&I)) {
        Type *SrcTy = CI->getOperand(0)->getType();
        Type *DstTy = CI->getType();
        if (CI->getOpcode() == Instruction::FPToSI ||
            CI->getOpcode() == Instruction::FPToUI) {
          ++estimatedGprValues;
          continue;
        }
        if (CI->getOpcode() == Instruction::BitCast &&
            ((SrcTy->isFloatTy() && DstTy->isIntegerTy(32)) ||
             (SrcTy->isDoubleTy() && DstTy->isIntegerTy(64)))) {
          ++estimatedGprValues;
          continue;
        }
        if ((CI->getOpcode() == Instruction::SExt ||
             CI->getOpcode() == Instruction::ZExt) &&
            SrcTy->isIntegerTy() && DstTy->isIntegerTy()) {
          unsigned srcBits = SrcTy->getIntegerBitWidth();
          unsigned dstBits = DstTy->getIntegerBitWidth();
          if ((srcBits == 8 || srcBits == 16) &&
              (dstBits == 32 || dstBits == 64))
            ++estimatedGprValues;
        }
      }
    }
  }
  unsigned callerSavedScratch =
      (gprArgCountForScratch < 16) ? (16 - gprArgCountForScratch) : 0;
  const bool useSavedGprScratch = estimatedGprValues > callerSavedScratch;

  // ---------- Pass 0: frame layout (collect allocas + saved scratch regs). ----------
  std::unordered_map<const AllocaInst *, int32_t> allocaOff;
  int32_t frameSize = 0;
  for (const BasicBlock &BB : Fn) {
    for (const Instruction &I : BB) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (!AI->isStaticAlloca()) {
          r.status = Status::Unsupported; r.reason = "dynamic alloca"; return r;
        }
        auto szOpt = AI->getAllocationSizeInBits(DL);
        if (!szOpt) { r.status = Status::Unsupported; r.reason = "unsized alloca"; return r; }
        uint64_t bytes = (*szOpt + 7) / 8;
        uint64_t align = AI->getAlign().value();
        if (align < 8) align = 8;
        frameSize = (int32_t)((frameSize + (int32_t)align - 1) & ~((int32_t)align - 1));
        allocaOff[AI] = frameSize;
        frameSize += (int32_t)bytes;
      }
    }
  }
  // Round 8l: make x19..x28 available as extra GPR scratch registers.
  // They are callee-saved under AAPCS64, so reserve 10 x 8-byte slots
  // and save/restore them in the prologue/epilogue. This is deliberately
  // simpler than a full SSA spill allocator, but removes the common
  // "scratch OOM" cliff for wide scalar expressions while preserving the
  // caller's ABI-visible state.
  if (frameSize) frameSize = (frameSize + 15) & ~15;
  const int32_t calleeSaveBase = frameSize;
  const unsigned firstSavedScratch = 19;
  const unsigned lastSavedScratch = 28;
  const unsigned numSavedScratch = lastSavedScratch - firstSavedScratch + 1;
  if (useSavedGprScratch)
    frameSize += (int32_t)numSavedScratch * 8;
  // 16-byte align the final frame.
  if (frameSize) frameSize = (frameSize + 15) & ~15;
  if (frameSize > 0xFFF) {
    r.status = Status::Unsupported; r.reason = "frame > 4095B"; return r;
  }

  // Prologue: sub sp, sp, #frame.
  if (frameSize > 0) {
    if (!W.emit(encAddSubImm(true, true, 31, 31, (unsigned)frameSize))) {
      r.status = Status::TooLarge; return r;
    }
  }
  if (useSavedGprScratch) {
    for (unsigned sr = firstSavedScratch; sr <= lastSavedScratch; ++sr) {
      unsigned off = (unsigned)calleeSaveBase + (sr - firstSavedScratch) * 8u;
      if (!W.emit(encLdrStrUI(false, 3, sr, 31, off / 8u))) {
        r.status = Status::TooLarge; return r;
      }
    }
  }

  // ---------- Pass 1: pre-assign scratch registers to every SSA value
  //            that produces a non-pointer, non-void result AND is not a
  //            stack-only pointer. Pointers that live on the stack don't
  //            consume a register. Cast/gep/alloca/phi/load/store/icmp
  //            results are handled inline. We use x9..x15 for scratch.
  //
  // Pre-assign phi registers so predecessor copy-insertion can reference
  // them before visiting the phi's block.

  std::unordered_map<const Value *, unsigned> regOf;
  std::unordered_map<const Value *, unsigned> fpRegOf;

  // Params: integer/pointer arguments use x0..x7; float/double arguments
  // use s0..s7 / d0..d7. The register classes have independent AAPCS64
  // allocation. The 9th and later GPR-class / FP-class scalar arguments
  // overflow onto the caller's stack frame in a single shared "incoming
  // argument area" (AAPCS64 §6.4 NSAA), laid out in the original
  // parameter order. We collect overflow args here and preload them
  // into scratch registers below (after assignReg/assignFpReg are
  // defined). For the supported scalar subset every overflow slot is
  // 8-byte aligned and 8 bytes wide:
  //   - i32  → 8-byte slot, low 4 bytes hold the value (LE host).
  //   - i64/pointer → 8-byte slot.
  //   - float  → 8-byte slot, low 4 bytes hold the value (LE host).
  //   - double → 8-byte slot.
  // i8/i16 stack args are intentionally rejected — frontends should
  // widen narrow integers before the AAPCS boundary anyway.
  struct StackArgEnt {
    const Argument *arg;
    int32_t  incomingOff; // bytes from entry SP (caller's NSAA base).
    bool     isFp;
    bool     is64;        // i64/ptr/double; false for i32/float.
    bool     isPointer;
  };
  struct StackReloadLoc {
    int32_t off; // bytes from adjusted SP.
    bool    is64;
  };
  std::vector<StackArgEnt> stackArgs;
  std::unordered_map<const Value *, StackReloadLoc> reloadableStackArg;
  unsigned argCount = 0;
  unsigned fpArgCount = 0;
  int32_t  incomingStackOff = 0;
  {
    for (const Argument &A : Fn.args()) {
      Type *T = A.getType();
      if (T->isIntegerTy() || T->isPointerTy()) {
        if (argCount < 8) {
          regOf[&A] = argCount;
          argCount++;
          continue;
        }
        // Overflow GPR-class arg.
        bool is64 = T->isPointerTy() || T->isIntegerTy(64);
        if (!is64 && !T->isIntegerTy(32)) {
          r.status = Status::Unsupported;
          r.reason = "stack arg shape (i8/i16)";
          return r;
        }
        incomingStackOff = (incomingStackOff + 7) & ~7;
        stackArgs.push_back({&A, incomingStackOff, /*isFp=*/false, is64,
                             T->isPointerTy()});
        incomingStackOff += 8;
      } else if (T->isFloatTy() || T->isDoubleTy()) {
        if (fpArgCount < 8) {
          // float -> S{n}, double -> D{n}; the V register file is shared so
          // we just track the register index and let emit-time logic pick
          // the S- or D-view based on the SSA value's type.
          fpRegOf[&A] = fpArgCount;
          fpArgCount++;
          continue;
        }
        bool isDouble = T->isDoubleTy();
        incomingStackOff = (incomingStackOff + 7) & ~7;
        stackArgs.push_back({&A, incomingStackOff, /*isFp=*/true, isDouble,
                             /*isPointer=*/false});
        incomingStackOff += 8;
      } else {
        r.status = Status::Unsupported; r.reason = "non-int/ptr/float arg"; return r;
      }
    }
  }

  // Scratch pool is x{argCount}..x15 plus saved x19..x28. Skip x16/x17
  // because they are transient materialization/address temps and skip
  // x18 because AAPCS64 reserves it as the platform register on some
  // systems. x19..x28 are safe because the prologue saves them above and
  // every return restores them before handing control back to the caller.
  unsigned nextReg = argCount;
  unsigned nextFpReg = 16;
  std::vector<unsigned> freeRegs;
  std::vector<unsigned> freeFpRegs;
  auto assignReg = [&](const Value *V) -> int {
    if (!freeRegs.empty()) {
      unsigned r = freeRegs.back();
      freeRegs.pop_back();
      regOf[V] = r;
      return (int)r;
    }
    // Skip x16/x17 temps and x18 platform register.
    if (nextReg == 16)
      nextReg = useSavedGprScratch ? 19 : 29;
    if (nextReg == 17 || nextReg == 18) nextReg = 19;
    unsigned lastScratch = useSavedGprScratch ? lastSavedScratch : 15;
    if (nextReg > lastScratch) return -1;
    unsigned r = nextReg++;
    regOf[V] = r;
    return (int)r;
  };
  auto assignFpReg = [&](const Value *V) -> int {
    if (!freeFpRegs.empty()) {
      unsigned r = freeFpRegs.back();
      freeFpRegs.pop_back();
      fpRegOf[V] = r;
      return (int)r;
    }
    // s31 is reserved as a transient FP-immediate scratch register.
    if (nextFpReg > 30) return -1;
    unsigned r = nextFpReg++;
    fpRegOf[V] = r;
    return (int)r;
  };

  // ---------- Stack-arg preload (round 8j). ----------
  // For each AAPCS64 stack-passed scalar argument, allocate a scratch
  // register and emit one LDR from `[sp, frameSize + incomingOff]`. The
  // result is that overflow args become indistinguishable from in-reg
  // args for the rest of pass 1 / pass 2 / pass 3 — they live in
  // regOf / fpRegOf with a normal scratch index. We do this AFTER the
  // prologue (sp already adjusted by `frameSize`) and BEFORE phi
  // pre-assign, so any subsequent scratch allocations follow these.
  auto fitsScaledLocal = [](int32_t off, unsigned size) -> int {
    unsigned scale = 1u << size;
    if (off < 0) return -1;
    if ((uint32_t)off & (scale - 1)) return -1;
    uint32_t s = (uint32_t)off / scale;
    if (s > 0xFFF) return -1;
    return (int)s;
  };
  for (const StackArgEnt &SE : stackArgs) {
    int32_t off = frameSize + SE.incomingOff;
    if (SE.isFp) {
      int rd = assignFpReg(SE.arg);
      if (rd < 0) {
        r.status = Status::Unsupported;
        r.reason = "fp scratch OOM (stack arg)";
        return r;
      }
      unsigned sz = SE.is64 ? 3u : 2u;
      int s = fitsScaledLocal(off, sz);
      if (s < 0) {
        r.status = Status::Unsupported;
        r.reason = "stack arg offset/encoding";
        return r;
      }
      bool ok = SE.is64
                  ? W.emit(encFpLdrStrUID(true, (unsigned)rd, 31, (unsigned)s))
                  : W.emit(encFpLdrStrUIS(true, (unsigned)rd, 31, (unsigned)s));
      if (!ok) { r.status = Status::TooLarge; return r; }
    } else {
      if (!SE.isPointer) {
        reloadableStackArg[SE.arg] = {off, SE.is64};
        continue;
      }
      int rd = assignReg(SE.arg);
      if (rd < 0) {
        r.status = Status::Unsupported;
        r.reason = "scratch OOM (stack arg)";
        return r;
      }
      unsigned sz = SE.is64 ? 3u : 2u;
      int s = fitsScaledLocal(off, sz);
      if (s < 0) {
        r.status = Status::Unsupported;
        r.reason = "stack arg offset/encoding";
        return r;
      }
      if (!W.emit(encLdrStrUI(true, sz, (unsigned)rd, 31, (unsigned)s))) {
        r.status = Status::TooLarge; return r;
      }
    }
  }

  // Pre-assign phi regs.
  for (const BasicBlock &BB : Fn) {
    for (const Instruction &I : BB) {
      if (auto *PN = dyn_cast<PHINode>(&I)) {
        if (PN->getType()->isFloatTy() || PN->getType()->isDoubleTy()) {
          if (assignFpReg(PN) < 0) {
            r.status = Status::Unsupported; r.reason = "out of fp scratch (phi)"; return r;
          }
          continue;
        }
        if (!PN->getType()->isIntegerTy()) {
          r.status = Status::Unsupported; r.reason = "non-int phi"; return r;
        }
        if (assignReg(PN) < 0) {
          r.status = Status::Unsupported; r.reason = "out of scratch (phi)"; return r;
        }
      }
    }
  }

  // ---------- Pass 2: pointer-location analysis (allocas + const-GEPs). ----------
  // Pointer-typed values we can place: allocas → StackRel{offset}; GEPs with
  // all-constant indices on such a base → StackRel{base + off}; other
  // pointer Values are assumed to be InReg via the regOf map (function args).

  std::unordered_map<const Value *, PtrLoc> ptrLoc;
  for (const Argument &A : Fn.args()) {
    if (A.getType()->isPointerTy())
      ptrLoc[&A] = PtrLoc{PtrLoc::InReg, regOf[&A], 0, 0};
  }
  for (const BasicBlock &BB : Fn) {
    for (const Instruction &I : BB) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        ptrLoc[AI] = PtrLoc{PtrLoc::StackRel, 0, allocaOff[AI], 0};
        continue;
      }
      if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        // First try the existing StackRel path (alloca-based GEP). This
        // path also accepts BitCast bases via ptrLoc[BC] propagated below.
        auto it = ptrLoc.find(GEP->getPointerOperand());
        if (it != ptrLoc.end() && it->second.kind == PtrLoc::StackRel) {
          int64_t off;
          if (constGepOffset(cast<GEPOperator>(GEP), DL, off)) {
            ptrLoc[GEP] = PtrLoc{PtrLoc::StackRel, 0,
                                 it->second.spOff + (int32_t)off, 0};
            continue;
          }
        }
        // Otherwise try the general resolver, which handles
        // inttoptr-ConstantExpr bases (PtrLoc::Absolute) and any chain of
        // constant-offset GEPs / bitcasts on top of them.
        PtrLoc loc;
        if (resolvePtrLocChain(GEP, DL, ptrLoc, globals, nglobals, loc)) {
          ptrLoc[GEP] = loc;
          continue;
        }
        // Last resort: dynamic-scaled GEP (one runtime index, PoT scale).
        // This serves the `array[idx]` pattern where the array base is an
        // absolute address (inttoptr or registered GlobalVariable).
        if (resolveDynScaledGep(cast<GEPOperator>(GEP), DL, ptrLoc,
                                globals, nglobals, loc)) {
          ptrLoc[GEP] = loc;
        }
        // Note: still no entry → load/store user will see the missing
        // ptrLoc and emit Status::Unsupported (or fall through to its own
        // resolver below).
        continue;
      }
      if (auto *BC = dyn_cast<BitCastInst>(&I)) {
        auto it = ptrLoc.find(BC->getOperand(0));
        if (it != ptrLoc.end()) ptrLoc[BC] = it->second;
        else {
          PtrLoc loc;
          if (resolvePtrLocChain(BC, DL, ptrLoc, globals, nglobals, loc))
            ptrLoc[BC] = loc;
        }
        continue;
      }
    }
  }

  // Local SSA scratch reuse. The emitter is still not a general register
  // allocator, but many post-specialization expressions are straight-line
  // chains inside one basic block. Reclaiming an instruction result after
  // its last same-block use avoids the old "permanent scratch slot per SSA"
  // cliff without reasoning about cross-block liveness.
  std::unordered_map<const Value *, unsigned> localUseCount;
  std::unordered_set<const Value *> nonLocalValue;
  for (const BasicBlock &BB : Fn) {
    for (const Instruction &I : BB) {
      for (const Use &U : I.operands()) {
        auto *Def = dyn_cast<Instruction>(U.get());
        if (!Def || isa<PHINode>(Def)) continue;
        if (Def->getParent() == &BB) {
          localUseCount[Def]++;
        } else {
          nonLocalValue.insert(Def);
        }
      }
      if (auto *PN = dyn_cast<PHINode>(&I)) {
        for (const Use &U : PN->incoming_values()) {
          if (auto *Def = dyn_cast<Instruction>(U.get()))
            nonLocalValue.insert(Def);
        }
      }
    }
  }
  for (const Value *V : nonLocalValue)
    localUseCount.erase(V);
  std::unordered_map<const Value *, unsigned> ptrTermUseCount;
  auto countPtrLocTerms = [&](const Value *P) {
    auto It = ptrLoc.find(P);
    if (It == ptrLoc.end()) return;
    const PtrLoc &PL = It->second;
    if (PL.kind != PtrLoc::AbsoluteScaledIndex &&
        PL.kind != PtrLoc::InRegScaledIndex)
      return;
    for (unsigned t = 0; t < PL.numTerms; ++t) {
      localUseCount.erase(PL.terms[t].value);
      ptrTermUseCount[PL.terms[t].value]++;
    }
  };
  for (const BasicBlock &BB : Fn) {
    for (const Instruction &I : BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        countPtrLocTerms(LI->getPointerOperand());
        continue;
      }
      if (auto *SI = dyn_cast<StoreInst>(&I)) {
        countPtrLocTerms(SI->getPointerOperand());
        continue;
      }
      if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
        if (II->getIntrinsicID() == Intrinsic::memcpy) {
          countPtrLocTerms(II->getArgOperand(0));
          countPtrLocTerms(II->getArgOperand(1));
        }
      }
    }
  }

  // ---------- Pass 3: BB layout + code emission. ----------
  // Record each BB's starting offset in the code buffer, and collect branch
  // fixups { patchPos, targetBB, isBcond, cond }.

  std::unordered_map<const BasicBlock *, size_t> bbStart;
  struct Fixup {
    size_t pos;
    const BasicBlock *target;
    bool bcond;
    unsigned cond;
  };
  std::vector<Fixup> fixups;

  // For the retval-bearing ret, we need to know frameSize. Already have it.

  // Helper: emit a MOVZ/MOVK chain that places an arbitrary integer
  // constant (negative or non-negative) into Wrd (is64=false) or Xrd
  // (is64=true). Halfword selection uses the positional `hw` field of
  // MOVZ/MOVK (ARM ARM C6.2.193 / C6.2.194) — endian-neutral by
  // construction.
  //
  // Negative-immediate strategy (round 8e):
  //   We materialize the unsigned two's-complement bit pattern of the
  //   constant at the operation width (32 or 64 bits) using MOVZ + up
  //   to 3 MOVKs. This avoids needing a separate MOVN encoder while
  //   still giving correct results for every representable value:
  //     - i32  -12345  → 0xFFFFCFC7 → MOVZ W,#0xCFC7 + MOVK W,#0xFFFF,lsl#16
  //     - i32  -1      → 0xFFFFFFFF → MOVZ W,#0xFFFF + MOVK W,#0xFFFF,lsl#16
  //     - i64  -1      → all-ones   → MOVZ X,#0xFFFF + 3× MOVK ,#0xFFFF
  //   Cost is at worst 2 instructions (i32) or 4 (i64) — same upper
  //   bound as the previous non-negative path. We deliberately do
  //   not emit MOVN: MOVN would shave one instruction off some
  //   negative values, but the simpler bit-pattern path keeps the
  //   helper symmetric for positive and negative constants and avoids
  //   a second encoder.
  //
  // The input APInt is normalized to the operation width with
  // `sextOrTrunc`, so a narrower IR type (e.g. an i8 constant used in
  // an i32 binop) is sign-extended into the right 32-bit pattern.
  // Narrow integer (i1/i8/i16) constants are still only intended to
  // appear after the frontend has widened the surrounding operation —
  // we do NOT promise correctness for an i8/i16 binop directly.
  //
  // Encoding choice:
  //   - 32-bit form: MOVZ W,#hw0 + optional MOVK W,#hw1,lsl#16
  //     (the MOVZ is always emitted — for hw0==0 it produces the
  //     necessary zeroing of the upper halfword in W).
  //   - 64-bit form: MOVZ X,#hw0 + up to three MOVK X,#hwN,lsl#(N*16);
  //     skip MOVK when the halfword is zero, since MOVZ already left
  //     it zero.
  auto emitMovImm = [&](unsigned rd, bool is64, const APInt &APIn) -> bool {
    APInt AP = APIn.sextOrTrunc(is64 ? 64 : 32);
    uint64_t v = AP.getZExtValue();
    if (!is64) {
      uint16_t lo = (uint16_t)(v & 0xFFFFu);
      uint16_t hi = (uint16_t)((v >> 16) & 0xFFFFu);
      if (!W.emit(encMovz16(false, rd, lo))) return false;
      if (hi && !W.emit(encMovkHw32(rd, hi, 1))) return false;
      return true;
    }
    uint16_t hw0 = (uint16_t)(v        & 0xFFFFu);
    uint16_t hw1 = (uint16_t)((v >> 16) & 0xFFFFu);
    uint16_t hw2 = (uint16_t)((v >> 32) & 0xFFFFu);
    uint16_t hw3 = (uint16_t)((v >> 48) & 0xFFFFu);
    if (!W.emit(encMovzHw64(rd, hw0, 0))) return false;
    if (hw1 && !W.emit(encMovkHw64(rd, hw1, 1))) return false;
    if (hw2 && !W.emit(encMovkHw64(rd, hw2, 2))) return false;
    if (hw3 && !W.emit(encMovkHw64(rd, hw3, 3))) return false;
    return true;
  };

  // Materialize an `f32`/`f64` ConstantFP into the destination FP
  // register `rd`. For float, fast paths for 0.0f and 2.0f use
  // FMOV-imm; everything else goes through `MOVZ Wx16,#lo (+ MOVK
  // Wx16,#hi,lsl#16)` followed by `FMOV Sd, W16`. For double, 0.0
  // uses `FMOV Dd, XZR` (1 insn); other values are materialized via
  // a 64-bit MOVZ/MOVK chain on x16 followed by `FMOV Dd, X16`.
  // Defined here (above `epilogueRet`) so both `epilogueRet` (for
  // `ret <ConstantFP>`) and `valueInFpReg` can share it. Returns
  // false on encode/buffer failure or unsupported FP semantics.
  auto materializeFpConstToReg = [&](const ConstantFP *CFP,
                                     unsigned rd) -> bool {
    const APFloat &APF = CFP->getValueAPF();
    if (&APF.getSemantics() == &APFloat::IEEEsingle()) {
      if (APF.isZero()) return W.emit(encFmovZeroS(rd));
      bool losesInfo = false;
      APFloat V2(APF);
      V2.convert(APFloat::IEEEsingle(), APFloat::rmNearestTiesToEven,
                 &losesInfo);
      if (!losesInfo && V2.convertToFloat() == 2.0f)
        return W.emit(encFmovImm2S(rd));
      uint32_t raw = (uint32_t)APF.bitcastToAPInt().getZExtValue();
      if (!W.emit(encMovz16(false, 16, (uint16_t)(raw & 0xFFFFu)))) return false;
      uint16_t hi = (uint16_t)(raw >> 16);
      if (hi && !W.emit(encMovkHw32(16, hi, 1))) return false;
      return W.emit(encFmovSFromW(rd, 16));
    }
    if (&APF.getSemantics() == &APFloat::IEEEdouble()) {
      // 0.0 fast path: FMOV Dd, XZR (xzr is encoded as Xn=31).
      if (APF.isZero()) return W.emit(encFmovDFromX(rd, 31));
      uint64_t raw = APF.bitcastToAPInt().getZExtValue();
      // Reuse the 64-bit MOVZ+MOVK chain on x16, then FMOV Dd, X16.
      // emitMovImm takes an APInt so we hand it the raw bit pattern
      // as an unsigned 64-bit integer.
      if (!emitMovImm(16, /*is64=*/true, APInt(64, raw))) return false;
      return W.emit(encFmovDFromX(rd, 16));
    }
    return false;
  };

  auto epilogueRet = [&](const Value *retVal) -> bool {
    if (retVal) {
      // Float return — place the value in S0. SSA values that already
      // live in `fpRegOf` are moved with FMOV S0, Sn. ConstantFP
      // returns are handled directly here (round 8g): we materialize
      // the constant straight into S0 so callers like `ret float 1.5`
      // work without going through an intermediate SSA producer.
      // Float/double return — place the value in S0/D0. SSA values
      // that already live in `fpRegOf` are moved with FMOV Sn/Dn ->
      // S0/D0. ConstantFP returns are handled directly here (round
      // 8g): we materialize the constant straight into S0/D0 so
      // callers like `ret float 1.5` / `ret double 1.5` work without
      // going through an intermediate SSA producer.
      if (retVal->getType()->isFloatTy() ||
          retVal->getType()->isDoubleTy()) {
        bool isDouble = retVal->getType()->isDoubleTy();
        auto itf = fpRegOf.find(retVal);
        if (itf != fpRegOf.end()) {
          if (itf->second != 0) {
            if (!W.emit(isDouble ? encFmovRegD(0, itf->second)
                                 : encFmovRegS(0, itf->second)))
              return false;
          }
        } else if (auto *CFP = dyn_cast<ConstantFP>(retVal)) {
          if (!materializeFpConstToReg(CFP, 0)) return false;
        } else {
          return false;
        }
      } else {
      auto itr = regOf.find(retVal);
      if (itr != regOf.end()) {
        if (itr->second != 0) {
          bool is64 = retVal->getType()->isIntegerTy(64);
          if (!W.emit(encMovReg(is64, 0, itr->second))) return false;
        }
      } else if (auto *CI = dyn_cast<ConstantInt>(retVal)) {
        // Materialize an integer return constant directly into x0/w0
        // via the shared MOVZ/MOVK helper above. Both non-negative and
        // negative values are supported (negatives use the unsigned
        // two's-complement bit pattern at the operation width).
        bool is64 = CI->getType()->isIntegerTy(64);
        if (!emitMovImm(0, is64, CI->getValue())) return false;
      } else {
        return false;
      }
      }
    }
    if (useSavedGprScratch) {
      for (unsigned sr = firstSavedScratch; sr <= lastSavedScratch; ++sr) {
        unsigned off = (unsigned)calleeSaveBase + (sr - firstSavedScratch) * 8u;
        if (!W.emit(encLdrStrUI(true, 3, sr, 31, off / 8u))) return false;
      }
    }
    if (frameSize > 0) {
      if (!W.emit(encAddSubImm(false, true, 31, 31, (unsigned)frameSize))) return false;
    }
    return W.emit(kRet);
  };

  // Helper: materialize an integer immediate into x16/w16 (the binop
  // scratch slot). Width follows the binop request: i32 → 32-bit
  // MOVZ + optional MOVK; i64 → up to 4-halfword MOVZ+MOVK chain.
  // Negative values are supported via the two's-complement bit
  // pattern (see `emitMovImm` above).
  auto materializeImmAny = [&](const ConstantInt *CI, bool is64,
                               unsigned &outReg) -> bool {
    outReg = 16;
    return emitMovImm(16, is64, CI->getValue());
  };

  auto loadReloadableStackArg = [&](const Value *V, bool is64,
                                    unsigned rd) -> bool {
    (void)is64;
    auto it = reloadableStackArg.find(V);
    if (it == reloadableStackArg.end()) return false;
    unsigned sz = it->second.is64 ? 3u : 2u;
    int s = fitsScaledLocal(it->second.off, sz);
    if (s < 0) return false;
    return W.emit(encLdrStrUI(true, sz, rd, 31, (unsigned)s));
  };

  auto isReloadableStackArg = [&](const Value *V) -> bool {
    return reloadableStackArg.find(V) != reloadableStackArg.end();
  };

  // Helper: get a value into a register (returns true on success). Uses x16
  // as the scratch destination for materialized immediates. Caller supplies
  // the desired is64.
  auto valueInReg = [&](const Value *V, bool is64, unsigned &outReg) -> bool {
    auto it = regOf.find(V);
    if (it != regOf.end()) { outReg = it->second; return true; }
    if (isReloadableStackArg(V)) {
      outReg = 16;
      return loadReloadableStackArg(V, is64, outReg);
    }
    if (auto *CI = dyn_cast<ConstantInt>(V))
      return materializeImmAny(CI, is64, outReg);
    return false;
  };

  auto valueInFpReg = [&](const Value *V, unsigned &outReg) -> bool {
    auto it = fpRegOf.find(V);
    if (it != fpRegOf.end()) { outReg = it->second; return true; }
    if (auto *CFP = dyn_cast<ConstantFP>(V)) {
      // ConstantFP path always lands in the S31 scratch slot. Callers
      // that need to mix two ConstantFPs in one operation (FCMP, FP
      // binop) are responsible for detecting the rn==rm==31 collision
      // and rejecting it with a clear reason.
      outReg = 31;
      return materializeFpConstToReg(CFP, outReg);
    }
    return false;
  };

  auto freeMappedValue = [&](const Value *V) {
    if (auto RI = regOf.find(V); RI != regOf.end()) {
      unsigned rr = RI->second;
      regOf.erase(RI);
      bool stillMapped = false;
      for (const auto &KV : regOf) {
        if (KV.second == rr) {
          stillMapped = true;
          break;
        }
      }
      if (!stillMapped && rr != 16 && rr != 17 && rr != 18)
        freeRegs.push_back(rr);
    }
    if (auto FI = fpRegOf.find(V); FI != fpRegOf.end()) {
      unsigned fr = FI->second;
      fpRegOf.erase(FI);
      bool stillMapped = false;
      for (const auto &KV : fpRegOf) {
        if (KV.second == fr) {
          stillMapped = true;
          break;
        }
      }
      if (!stillMapped && fr != 31)
        freeFpRegs.push_back(fr);
    }
  };

  auto releaseIfDead = [&](const Value *V) {
    auto UC = localUseCount.find(V);
    if (UC == localUseCount.end()) return;
    if (UC->second == 0) return;
    --UC->second;
    if (UC->second != 0) return;
    freeMappedValue(V);
  };

  auto releaseOperands = [&](const Instruction &I) {
    for (const Use &U : I.operands())
      releaseIfDead(U.get());
  };

  // Helper: load uimm12-scaled offset check.
  auto fitsScaled = [](int32_t off, unsigned size) -> int {
    unsigned scale = 1u << size; // size=0→1, size=1→2, size=2→4, size=3→8
    if (off < 0) return -1;
    if ((uint32_t)off & (scale - 1)) return -1;
    uint32_t s = (uint32_t)off / scale;
    if (s > 0xFFF) return -1;
    return (int)s;
  };

  // Helper: materialize a 64-bit absolute address into x17 via MOVZ/MOVK
  // halfword chain. Halfwords are positional (hw field), so this is
  // endian-neutral with respect to data endian. Returns false on emit fail.
  auto materializeAddrToX17 = [&](uint64_t addr) -> bool {
    uint16_t c0 = (uint16_t)(addr >>  0);
    uint16_t c1 = (uint16_t)(addr >> 16);
    uint16_t c2 = (uint16_t)(addr >> 32);
    uint16_t c3 = (uint16_t)(addr >> 48);
    if (!W.emit(encMovzHw64(17, c0, 0))) return false;
    if (c1 && !W.emit(encMovkHw64(17, c1, 1))) return false;
    if (c2 && !W.emit(encMovkHw64(17, c2, 2))) return false;
    if (c3 && !W.emit(encMovkHw64(17, c3, 3))) return false;
    return true;
  };

  // Helper: materialize the address of a PtrLoc::*ScaledIndex into x17.
  // Sequence:
  //   MOVZ/MOVK x17, addr           (Absolute base, includes const_off)
  //   or  MOV x17, base.reg ; ADD x17, x17, #spOff   (InReg base)
  // then for each term (round 8k: up to 2):
  //   shift <= 4: ADD x17, x17, Wm/Xm, ext #shift   (single instruction)
  //   shift  > 4: extend idx into x16; LSL x16, x16, #shift;
  //               ADD x17, x17, x16, UXTX #0        (3 instructions)
  //
  // ext is one of {UXTW=2, SXTW=6, UXTX=3, SXTX=7} depending on
  // idxIs64/idxSigned. For idxIs64=true we use UXTX (option=3); sign of
  // the value is irrelevant for the mod-2^64 add. For idxIs64=false we
  // use SXTW or UXTW based on idxSigned, mirroring the sext/zext
  // semantics of the IR's index-typing cast.
  //
  // x16 is treated as scratch in the >4 path — safe here because no
  // immediate-materialization runs between us and the final LDR/STR.
  std::string addrFailReason;
  auto materializeScaledAddrToX17 = [&](const PtrLoc &P) -> bool {
    addrFailReason.clear();
    if (P.kind == PtrLoc::AbsoluteScaledIndex) {
      if (!materializeAddrToX17(P.addr)) { addrFailReason = "base"; return false; }
    } else if (P.kind == PtrLoc::InRegScaledIndex) {
      if ((P.reg) != 17 && !W.emit(encMovReg(true, 17, P.reg))) { addrFailReason = "base"; return false; }
      if (P.spOff != 0) {
        if (P.spOff < 0 || P.spOff > 0xFFF) { addrFailReason = "base offset"; return false; }
        if (!W.emit(encAddSubImm(false, true, 17, 17, (unsigned)P.spOff))) { addrFailReason = "base offset"; return false; }
      }
    } else {
      addrFailReason = "kind";
      return false;
    }
    if (P.numTerms == 0 || P.numTerms > 2) { addrFailReason = "term count"; return false; }
    for (unsigned t = 0; t < P.numTerms; ++t) {
      const PtrLoc::Term &T = P.terms[t];
      unsigned idxReg;
      if (!valueInReg(T.value, T.is64, idxReg)) {
        addrFailReason = std::string("term") + std::to_string(t) + " value";
        return false;
      }
      unsigned option;
      if (T.is64)             option = 3; // UXTX (64-bit no-op)
      else if (T.isSigned)    option = 6; // SXTW
      else                    option = 2; // UXTW
      if (T.scaleLog2 <= 4) {
        if (!W.emit(encAddExtReg64(17, 17, idxReg, option, T.scaleLog2))) {
          addrFailReason = "term add"; return false;
        }
      } else {
        // 3-instruction: extend idx -> x16, LSL x16, ADD x17,x17,x16,UXTX#0
        if (T.is64) {
          if (idxReg != 16 && !W.emit(encMovReg(true, 16, idxReg))) { addrFailReason = "term move"; return false; }
        } else {
          // SXTW or UXTW into x16. encExtendBits(sign, to64=true, rd=16, rn=idx, fromBits=32)
          if (!W.emit(encExtendBits(T.isSigned, true, 16, idxReg, 32))) { addrFailReason = "term extend"; return false; }
        }
        if (!W.emit(encLslImm(true, 16, 16, T.scaleLog2))) { addrFailReason = "term shift"; return false; }
        if (!W.emit(encAddExtReg64(17, 17, 16, /*UXTX*/3, 0))) { addrFailReason = "term add"; return false; }
      }
    }
    return true;
  };

  auto releasePtrLocTerms = [&](const PtrLoc &P) {
    if (P.kind != PtrLoc::AbsoluteScaledIndex &&
        P.kind != PtrLoc::InRegScaledIndex)
      return;
    for (unsigned t = 0; t < P.numTerms; ++t) {
      const Value *V = P.terms[t].value;
      auto UC = ptrTermUseCount.find(V);
      if (UC == ptrTermUseCount.end() || UC->second == 0) continue;
      --UC->second;
      if (UC->second == 0 && isa<Instruction>(V) && !isa<PHINode>(V))
        freeMappedValue(V);
    }
  };

  auto accessSizeForBits = [](unsigned bits, unsigned &size) -> bool {
    switch (bits) {
      case 8:  size = 0; return true;
      case 16: size = 1; return true;
      case 32: size = 2; return true;
      case 64: size = 3; return true;
      default: return false;
    }
  };

  // Helper: emit a load from PtrLoc into reg rt, size in {0,1,2,3}.
  std::string memFailReason;
  auto emitLoad = [&](PtrLoc base, unsigned size, unsigned rt) -> bool {
    memFailReason.clear();
    if (base.kind == PtrLoc::StackRel) {
      int s = fitsScaled(base.spOff, size);
      if (s < 0) { memFailReason = "stack offset"; return false; }
      if (!W.emit(encLdrStrUI(true, size, rt, 31 /*sp*/, (unsigned)s))) {
        memFailReason = "code buffer"; return false;
      }
      return true;
    }
    if (base.kind == PtrLoc::Absolute) {
      // Materialize host address into x17 then LDR rt, [x17, #0]. We don't
      // try to split addr into base+uimm12 because the imm12 is multiplied
      // by access size (1/4/8) and most snapshot addresses aren't aligned
      // to 4096B; staying with full-width MOVZ/MOVK + offset 0 is simple
      // and always correct.
      if (!materializeAddrToX17(base.addr)) { memFailReason = "absolute addr"; return false; }
      if (!W.emit(encLdrStrUI(true, size, rt, 17, 0))) {
        memFailReason = "code buffer"; return false;
      }
      return true;
    }
    if (base.kind == PtrLoc::AbsoluteScaledIndex ||
        base.kind == PtrLoc::InRegScaledIndex) {
      if (!materializeScaledAddrToX17(base)) {
        memFailReason = std::string("dyn addr ") + addrFailReason;
        return false;
      }
      releasePtrLocTerms(base);
      if (!W.emit(encLdrStrUI(true, size, rt, 17, 0))) {
        memFailReason = "code buffer"; return false;
      }
      return true;
    }
    int s = fitsScaled(base.spOff, size);
    if (s < 0) { memFailReason = "reg offset"; return false; }
    if (!W.emit(encLdrStrUI(true, size, rt, base.reg, (unsigned)s))) {
      memFailReason = "code buffer"; return false;
    }
    return true;
  };
  auto emitStore = [&](PtrLoc base, unsigned size, unsigned rt) -> bool {
    if (base.kind == PtrLoc::StackRel) {
      int s = fitsScaled(base.spOff, size);
      if (s < 0) return false;
      return W.emit(encLdrStrUI(false, size, rt, 31, (unsigned)s));
    }
    if (base.kind == PtrLoc::Absolute) {
      if (!materializeAddrToX17(base.addr)) return false;
      return W.emit(encLdrStrUI(false, size, rt, 17, 0));
    }
    if (base.kind == PtrLoc::AbsoluteScaledIndex ||
        base.kind == PtrLoc::InRegScaledIndex) {
      if (!materializeScaledAddrToX17(base)) return false;
      releasePtrLocTerms(base);
      return W.emit(encLdrStrUI(false, size, rt, 17, 0));
    }
    int s = fitsScaled(base.spOff, size);
    if (s < 0) return false;
    return W.emit(encLdrStrUI(false, size, rt, base.reg, (unsigned)s));
  };

  // Lower memcpy(dst, src, N, false) with N <= 32 into 1–4 LDR/STR pairs.
  auto emitMemcpy = [&](PtrLoc dst, PtrLoc src, uint64_t N) -> bool {
    if (N == 0) return true;
    if (N > 32 || (N % 4) != 0) return false;
    // Use x17 as the transfer register.
    unsigned tmp = 17;
    uint64_t off = 0;
    while (off < N) {
      if (N - off >= 8) {
        PtrLoc s = src, d = dst;
        if (s.kind == PtrLoc::StackRel) s.spOff += off; // byte offset (endian-agnostic)
        if (d.kind == PtrLoc::StackRel) d.spOff += off;
        if (s.kind == PtrLoc::InReg && off != 0) return false;
        if (d.kind == PtrLoc::InReg && off != 0) return false;
        if (!emitLoad(s, 3, tmp)) return false;
        if (!emitStore(d, 3, tmp)) return false;
        off += 8;
      } else {
        PtrLoc s = src, d = dst;
        if (s.kind == PtrLoc::StackRel) s.spOff += off;
        if (d.kind == PtrLoc::StackRel) d.spOff += off;
        if (s.kind == PtrLoc::InReg && off != 0) return false;
        if (d.kind == PtrLoc::InReg && off != 0) return false;
        if (!emitLoad(s, 2, tmp)) return false;
        if (!emitStore(d, 2, tmp)) return false;
        off += 4;
      }
    }
    return true;
  };

  // For icmp/br fusion: remember the last icmp if its only user is the
  // terminating br i1 of the same BB. Reset per BB.
  ICmpFusion pendingCmp;
  // Same idea for fcmp: an fcmp result feeds either a conditional br or
  // a select. Reset per BB. We defer the actual FCMP emit until the
  // consumer because no other instruction we emit between the FCmp and
  // its branch/select can clobber NZCV (FP arith / FMOV / MOV / MOVK
  // all leave the flags alone) — same invariant as the integer path.
  FCmpFusion pendingFCmp;

  for (const BasicBlock &BB : Fn) {
    bbStart[&BB] = W.pos;
    pendingCmp.I = nullptr;
    pendingFCmp.I = nullptr;

    for (const Instruction &I : BB) {
      // Skip instructions whose effect was already modeled in passes 0/2.
      if (isa<AllocaInst>(&I)) continue;
      // Pointer-shaped bitcasts have already been folded into ptrLoc in
      // pass 2 — skip them here. FP/int bitcasts (round 8i: float<->i32,
      // double<->i64) fall through to the CastInst handler below where
      // they emit FMOV W,S / FMOV S,W / FMOV X,D / FMOV D,X.
      if (auto *BCI = dyn_cast<BitCastInst>(&I)) {
        Type *st = BCI->getSrcTy(), *dt = BCI->getDestTy();
        if (st->isPointerTy() && dt->isPointerTy()) continue;
      }
      if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        // Must have been lowered to a PtrLoc in pass 2, otherwise the load/
        // store that uses it will fail. Only accept fully-constant GEPs.
        if (!ptrLoc.count(GEP)) {
          r.status = Status::Unsupported;
          r.reason = "GEP not reducible to stack offset";
          return r;
        }
        continue;
      }

      // Phi nodes are materialized at predecessors' terminators, not here.
      if (isa<PHINode>(&I)) continue;

      // memcpy intrinsic
      if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
        if (II->getIntrinsicID() == Intrinsic::fmuladd) {
          if (!II->getType()->isFloatTy()) {
            r.status = Status::Unsupported; r.reason = "fmuladd non-f32"; return r;
          }
          int rd = assignFpReg(II);
          if (rd < 0) {
            r.status = Status::Unsupported; r.reason = "fp scratch OOM (fmuladd)"; return r;
          }
          unsigned rn, rm, ra;
          if (!valueInFpReg(II->getArgOperand(0), rn) ||
              !valueInFpReg(II->getArgOperand(1), rm) ||
              !valueInFpReg(II->getArgOperand(2), ra)) {
            r.status = Status::Unsupported; r.reason = "fmuladd operands"; return r;
          }
          if (!W.emit(encFmaddS((unsigned)rd, rn, rm, ra))) {
            r.status = Status::TooLarge; return r;
          }
          releaseOperands(I);
          continue;
        }
        if (II->getIntrinsicID() == Intrinsic::memcpy) {
          auto dit = ptrLoc.find(II->getArgOperand(0));
          auto sit = ptrLoc.find(II->getArgOperand(1));
          auto *N = dyn_cast<ConstantInt>(II->getArgOperand(2));
          if (dit == ptrLoc.end() || sit == ptrLoc.end() || !N) {
            r.status = Status::Unsupported; r.reason = "memcpy operands"; return r;
          }
          if (!emitMemcpy(dit->second, sit->second, N->getZExtValue())) {
            r.status = Status::Unsupported; r.reason = "memcpy size/alignment"; return r;
          }
          releaseOperands(I);
          continue;
        }
        // lifetime.* / dbg.* / assume: ignore
        switch (II->getIntrinsicID()) {
        case Intrinsic::lifetime_start:
        case Intrinsic::lifetime_end:
        case Intrinsic::assume:
        case Intrinsic::dbg_declare:
        case Intrinsic::dbg_value:
        case Intrinsic::dbg_label:
          continue;
        default:
          r.status = Status::Unsupported;
          r.reason = std::string("unhandled intrinsic: ") + II->getCalledFunction()->getName().str();
          return r;
        }
      }

      // Load
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isFloatTy() || LI->getType()->isDoubleTy()) {
          bool isDouble = LI->getType()->isDoubleTy();
          // Scale shift for the unsigned-immediate-offset LDR encoding:
          // single = 4 bytes -> shift 2; double = 8 bytes -> shift 3.
          int scaleShift = isDouble ? 3 : 2;
          auto it = ptrLoc.find(LI->getPointerOperand());
          if (it == ptrLoc.end()) {
            PtrLoc loc;
            if (!resolvePtrLocChain(LI->getPointerOperand(), DL, ptrLoc,
                                    globals, nglobals, loc) &&
                !(isa<GEPOperator>(LI->getPointerOperand()) &&
                  resolveDynScaledGep(cast<GEPOperator>(LI->getPointerOperand()),
                                      DL, ptrLoc, globals, nglobals, loc))) {
              r.status = Status::Unsupported; r.reason = "float load ptr"; return r;
            }
            ptrLoc[LI->getPointerOperand()] = loc;
            it = ptrLoc.find(LI->getPointerOperand());
          }
          int rd = assignFpReg(LI);
          if (rd < 0) {
            r.status = Status::Unsupported; r.reason = "fp scratch OOM (load)"; return r;
          }
          PtrLoc base = it->second;
          unsigned baseReg = 0;
          unsigned imm = 0;
          if (base.kind == PtrLoc::StackRel) {
            int s = fitsScaled(base.spOff, scaleShift);
            if (s < 0) { r.status = Status::Unsupported; r.reason = "float load offset"; return r; }
            baseReg = 31; imm = (unsigned)s;
          } else if (base.kind == PtrLoc::Absolute) {
            if (!materializeAddrToX17(base.addr)) { r.status = Status::TooLarge; return r; }
            baseReg = 17; imm = 0;
          } else if (base.kind == PtrLoc::AbsoluteScaledIndex ||
                     base.kind == PtrLoc::InRegScaledIndex) {
            if (!materializeScaledAddrToX17(base)) { r.status = Status::Unsupported; r.reason = "float dyn addr"; return r; }
            releasePtrLocTerms(base);
            baseReg = 17; imm = 0;
          } else {
            int s = fitsScaled(base.spOff, scaleShift);
            if (s < 0) { r.status = Status::Unsupported; r.reason = "float load reg+off"; return r; }
            baseReg = base.reg; imm = 0;
            imm = (unsigned)s;
          }
          if (!W.emit(isDouble
                        ? encFpLdrStrUID(true, (unsigned)rd, baseReg, imm)
                        : encFpLdrStrUIS(true, (unsigned)rd, baseReg, imm))) {
            r.status = Status::TooLarge; return r;
          }
          continue;
        }
        unsigned bits = 0;
        if (LI->getType()->isIntegerTy())
          bits = LI->getType()->getIntegerBitWidth();
        else if (LI->getType()->isPointerTy())
          bits = 64;  // pointer load = i64 load (host ABI)
        unsigned accessSize = 0;
        if (!accessSizeForBits(bits, accessSize)) {
          r.status = Status::Unsupported; r.reason = "load width"; return r;
        }

        // Fast-path: if the pointer is a constant GEP on a GlobalVariable
        // whose initializer is a ConstantStruct/Array, extract the constant
        // element and materialize it as an immediate. This handles the
        // common "global snapshot" case where the pass left a global but
        // the runtime snapshot values are known at compile time.
        if (auto *GEP = dyn_cast<GEPOperator>(LI->getPointerOperand())) {
          if (auto *GV = dyn_cast<GlobalVariable>(GEP->getPointerOperand())) {
            if (GV->hasInitializer()) {
              if (auto *CS = dyn_cast<ConstantStruct>(GV->getInitializer())) {
                // Expect struct geps of the form getelementptr <struct>, ptr @gv, i32 0, i32 idx
                if (GEP->getNumOperands() >= 3) {
                  if (auto *Idx = dyn_cast<ConstantInt>(GEP->getOperand(2))) {
                    uint64_t field = Idx->getZExtValue();
                    if (field < CS->getNumOperands()) {
                      if (auto *CE = dyn_cast<ConstantInt>(CS->getAggregateElement((unsigned)field))) {
                        int rd = assignReg(LI);
                        if (rd < 0) { r.status = Status::Unsupported; r.reason = "scratch OOM (load const global)"; return r; }
                        unsigned outReg;
                        if (!materializeImmAny(CE, bits==64, outReg)) {
                          r.status = Status::Unsupported; r.reason = "global const too large"; return r;
                        }
                        // if assignReg allocated a different reg, move into it
                        if ((unsigned)rd != outReg) {
                          if (!W.emit(encMovReg(bits==64, (unsigned)rd, outReg))) { r.status = Status::TooLarge; return r; }
                        }
                        continue;
                      }
                    }
                  }
                }
              }
            }
          }
        }

        // Another fast-path: direct load from a GlobalVariable (no GEP)
        if (auto *GV = dyn_cast<GlobalVariable>(LI->getPointerOperand())) {
          if (GV->hasInitializer()) {
            if (auto *CS = dyn_cast<ConstantStruct>(GV->getInitializer())) {
              // load from base pointer -> field 0
              if (CS->getNumOperands() >= 1) {
                if (auto *CE = dyn_cast<ConstantInt>(CS->getAggregateElement((unsigned)0))) {
                  int rd = assignReg(LI);
                  if (rd < 0) { r.status = Status::Unsupported; r.reason = "scratch OOM (load const global)"; return r; }
                  unsigned outReg;
                  if (!materializeImmAny(CE, bits==64, outReg)) {
                    r.status = Status::Unsupported; r.reason = "global const too large"; return r;
                  }
                  if ((unsigned)rd != outReg) {
                    if (!W.emit(encMovReg(bits==64, (unsigned)rd, outReg))) { r.status = Status::TooLarge; return r; }
                  }
                  continue;
                }
              }
            }
          }
        }

        // External-global path: pointer resolves (directly or via constant
        // GEP) to a GlobalVariable whose host address was supplied in the
        // `globals` table. We materialize the 64-bit host address into x17
        // via MOVZ + MOVK chain and issue an LDR at [x17, #constOff].
        //
        // Endian: MOVZ/MOVK halfwords are positional (hw field), not byte
        // offsets — endian-neutral. The LDR reads target-endian bytes,
        // but the host C++ producer wrote target-endian bytes into the
        // same global (same process, same endian) -> symmetric -> correct
        // on both aarch64 and aarch64_be.
        {
          const Value *P = LI->getPointerOperand();
          const GlobalVariable *GV = nullptr;
          int64_t off = 0;
          if (auto *GVD = dyn_cast<GlobalVariable>(P)) {
            GV = GVD;
          } else if (auto *GEP = dyn_cast<GEPOperator>(P)) {
            if (auto *GVB = dyn_cast<GlobalVariable>(GEP->getPointerOperand())) {
              APInt apOff(64, 0);
              if (GEP->accumulateConstantOffset(DL, apOff) &&
                  apOff.getActiveBits() <= 31) {
                GV = GVB;
                off = apOff.getSExtValue();
              }
            }
          }
          if (GV) {
            if (const void *Host = resolveGlobal(GV)) {
              int scaled = fitsScaled((int32_t)off, accessSize);
              if (scaled < 0) {
                r.status = Status::Unsupported; r.reason = "global load uimm12 overflow"; return r;
              }
              int rd = assignReg(LI);
              if (rd < 0) { r.status = Status::Unsupported; r.reason = "scratch OOM (global load)"; return r; }
              // Materialize Host into x17 via MOVZ/MOVK chain.
              uint64_t addr = reinterpret_cast<uintptr_t>(Host);
              uint16_t c0 = (uint16_t)(addr >>  0);
              uint16_t c1 = (uint16_t)(addr >> 16);
              uint16_t c2 = (uint16_t)(addr >> 32);
              uint16_t c3 = (uint16_t)(addr >> 48);
              if (!W.emit(encMovzHw64(17, c0, 0))) { r.status=Status::TooLarge; return r; }
              if (c1 && !W.emit(encMovkHw64(17, c1, 1))) { r.status=Status::TooLarge; return r; }
              if (c2 && !W.emit(encMovkHw64(17, c2, 2))) { r.status=Status::TooLarge; return r; }
              if (c3 && !W.emit(encMovkHw64(17, c3, 3))) { r.status=Status::TooLarge; return r; }
              if (!W.emit(encLdrStrUI(true, accessSize, (unsigned)rd, 17,
                                      (unsigned)scaled))) {
                r.status=Status::TooLarge; return r;
              }
              continue;
            }
            // GV found but no host mapping — bail with a clear message.
            r.status = Status::Unsupported;
            r.reason = std::string("unresolved external global: ") + GV->getName().str();
            return r;
          }
        }

        // Fallback: regular ptr-based load (stack-rel or reg-based)
        auto it = ptrLoc.find(LI->getPointerOperand());
        if (it == ptrLoc.end()) {
          // Last-chance resolver: handles inline ConstantExpr GEPs whose
          // base is an inttoptr (i64 <C>) — used by EasyJIT to bake the
          // host address of a snapshot/global into the IR after
          // specialization. (See resolvePtrLocChain doc-comment.)
          PtrLoc loc;
          if (!resolvePtrLocChain(LI->getPointerOperand(), DL, ptrLoc,
                                  globals, nglobals, loc) &&
              !(isa<GEPOperator>(LI->getPointerOperand()) &&
                resolveDynScaledGep(cast<GEPOperator>(LI->getPointerOperand()),
                                    DL, ptrLoc, globals, nglobals, loc))) {
            r.status = Status::Unsupported; r.reason = "load ptr"; return r;
          }
          ptrLoc[LI->getPointerOperand()] = loc;
          it = ptrLoc.find(LI->getPointerOperand());
        }
        int rd = assignReg(LI);
        if (rd < 0) { r.status = Status::Unsupported; r.reason = "scratch OOM (load)"; return r; }
        if (!emitLoad(it->second, accessSize, (unsigned)rd)) {
          r.status = Status::Unsupported;
          r.reason = memFailReason.empty() ? "load offset/encoding"
                                           : std::string("load ") + memFailReason;
          return r;
        }
        continue;
      }
      // Store
      if (auto *SI = dyn_cast<StoreInst>(&I)) {
        const Value *V = SI->getValueOperand();
        if (V->getType()->isFloatTy() || V->getType()->isDoubleTy()) {
          bool isDouble = V->getType()->isDoubleTy();
          int scaleShift = isDouble ? 3 : 2;
          auto it = ptrLoc.find(SI->getPointerOperand());
          if (it == ptrLoc.end()) {
            PtrLoc loc;
            if (!resolvePtrLocChain(SI->getPointerOperand(), DL, ptrLoc,
                                    globals, nglobals, loc) &&
                !(isa<GEPOperator>(SI->getPointerOperand()) &&
                  resolveDynScaledGep(cast<GEPOperator>(SI->getPointerOperand()),
                                      DL, ptrLoc, globals, nglobals, loc))) {
              r.status = Status::Unsupported; r.reason = "float store ptr"; return r;
            }
            ptrLoc[SI->getPointerOperand()] = loc;
            it = ptrLoc.find(SI->getPointerOperand());
          }
          unsigned rs;
          if (!valueInFpReg(V, rs)) {
            r.status = Status::Unsupported; r.reason = "float store value"; return r;
          }
          PtrLoc base = it->second;
          unsigned baseReg = 0;
          unsigned imm = 0;
          if (base.kind == PtrLoc::StackRel) {
            int s = fitsScaled(base.spOff, scaleShift);
            if (s < 0) { r.status = Status::Unsupported; r.reason = "float store offset"; return r; }
            baseReg = 31; imm = (unsigned)s;
          } else if (base.kind == PtrLoc::Absolute) {
            if (!materializeAddrToX17(base.addr)) { r.status = Status::TooLarge; return r; }
            baseReg = 17; imm = 0;
          } else if (base.kind == PtrLoc::AbsoluteScaledIndex ||
                     base.kind == PtrLoc::InRegScaledIndex) {
            if (!materializeScaledAddrToX17(base)) { r.status = Status::Unsupported; r.reason = "float dyn addr"; return r; }
            releasePtrLocTerms(base);
            baseReg = 17; imm = 0;
          } else {
            int s = fitsScaled(base.spOff, scaleShift);
            if (s < 0) { r.status = Status::Unsupported; r.reason = "float store reg+off"; return r; }
            baseReg = base.reg; imm = 0;
            imm = (unsigned)s;
          }
          if (!W.emit(isDouble ? encFpLdrStrUID(false, rs, baseReg, imm)
                               : encFpLdrStrUIS(false, rs, baseReg, imm))) {
            r.status = Status::TooLarge; return r;
          }
          releaseOperands(I);
          continue;
        }
        unsigned bits = 0;
        if (V->getType()->isIntegerTy())
          bits = V->getType()->getIntegerBitWidth();
        else if (V->getType()->isPointerTy())
          bits = 64;  // pointer store = i64 store (host ABI)
        unsigned accessSize = 0;
        if (!accessSizeForBits(bits, accessSize)) {
          r.status = Status::Unsupported; r.reason = "store width"; return r;
        }

        // External-global store path (mirrors the load path above).
        {
          const Value *P = SI->getPointerOperand();
          const GlobalVariable *GV = nullptr;
          int64_t off = 0;
          if (auto *GVD = dyn_cast<GlobalVariable>(P)) {
            GV = GVD;
          } else if (auto *GEP = dyn_cast<GEPOperator>(P)) {
            if (auto *GVB = dyn_cast<GlobalVariable>(GEP->getPointerOperand())) {
              APInt apOff(64, 0);
              if (GEP->accumulateConstantOffset(DL, apOff) &&
                  apOff.getActiveBits() <= 31) {
                GV = GVB;
                off = apOff.getSExtValue();
              }
            }
          }
          if (GV) {
            if (const void *Host = resolveGlobal(GV)) {
              int scaled = fitsScaled((int32_t)off, accessSize);
              if (scaled < 0) {
                r.status = Status::Unsupported; r.reason = "global store uimm12 overflow"; return r;
              }
              unsigned rs;
              if (!valueInReg(V, bits == 64, rs)) {
                r.status = Status::Unsupported; r.reason = "global store value"; return r;
              }
              uint64_t addr = reinterpret_cast<uintptr_t>(Host);
              uint16_t c0 = (uint16_t)(addr >>  0);
              uint16_t c1 = (uint16_t)(addr >> 16);
              uint16_t c2 = (uint16_t)(addr >> 32);
              uint16_t c3 = (uint16_t)(addr >> 48);
              if (!W.emit(encMovzHw64(17, c0, 0))) { r.status=Status::TooLarge; return r; }
              if (c1 && !W.emit(encMovkHw64(17, c1, 1))) { r.status=Status::TooLarge; return r; }
              if (c2 && !W.emit(encMovkHw64(17, c2, 2))) { r.status=Status::TooLarge; return r; }
              if (c3 && !W.emit(encMovkHw64(17, c3, 3))) { r.status=Status::TooLarge; return r; }
              if (!W.emit(encLdrStrUI(false, accessSize, rs, 17,
                                      (unsigned)scaled))) {
                r.status=Status::TooLarge; return r;
              }
              releaseOperands(I);
              continue;
            }
            r.status = Status::Unsupported;
            r.reason = std::string("unresolved external global (store): ") + GV->getName().str();
            return r;
          }
        }

        auto it = ptrLoc.find(SI->getPointerOperand());
        if (it == ptrLoc.end()) {
          // Last-chance resolver, mirrors the load side.
          PtrLoc loc;
          if (!resolvePtrLocChain(SI->getPointerOperand(), DL, ptrLoc,
                                  globals, nglobals, loc) &&
              !(isa<GEPOperator>(SI->getPointerOperand()) &&
                resolveDynScaledGep(cast<GEPOperator>(SI->getPointerOperand()),
                                    DL, ptrLoc, globals, nglobals, loc))) {
            r.status = Status::Unsupported; r.reason = "store ptr"; return r;
          }
          ptrLoc[SI->getPointerOperand()] = loc;
          it = ptrLoc.find(SI->getPointerOperand());
        }
        unsigned rs;
        if (!valueInReg(V, bits == 64, rs)) {
          r.status = Status::Unsupported; r.reason = "store value"; return r;
        }
        if (!emitStore(it->second, accessSize, rs)) {
          r.status = Status::Unsupported; r.reason = "store offset/encoding"; return r;
        }
        releaseOperands(I);
        continue;
      }

      // icmp — record, fuse with following br.
      if (auto *IC = dyn_cast<ICmpInst>(&I)) {
        Value *L = IC->getOperand(0), *Rh = IC->getOperand(1);
        unsigned bits = L->getType()->isIntegerTy()
            ? L->getType()->getIntegerBitWidth() : 0;
        if (bits != 32 && bits != 64) {
          r.status = Status::Unsupported; r.reason = "icmp width"; return r;
        }
        unsigned rn;
        if (!valueInReg(L, bits == 64, rn)) {
          r.status = Status::Unsupported; r.reason = "icmp lhs"; return r;
        }
        pendingCmp.I = IC;
        pendingCmp.pred = IC->getPredicate();
        pendingCmp.lhsReg = rn;
        pendingCmp.is64 = (bits == 64);
        pendingCmp.rhsImm = asImm12(Rh);
        pendingCmp.rhsReg = 0;
        if (pendingCmp.rhsImm < 0) {
          if (!valueInReg(Rh, bits == 64, pendingCmp.rhsReg)) {
            r.status = Status::Unsupported; r.reason = "icmp rhs"; return r;
          }
        }
        continue;
      }

      // fcmp — record, fuse with following br/select. Scalar f32 and
      // f64 are supported; both operands must share the same FP type
      // (no mixed single/double). NaN-sensitive unordered predicates
      // and ONE are rejected with "fcmp predicate" / "fcmp shape".
      //
      // IMPORTANT (round 8g): we do NOT materialize the operands here.
      // If we did, an intervening instruction that materializes another
      // ConstantFP (e.g. `%t = fadd float %x, 2.0` between this fcmp
      // and its consumer) would clobber the S31/D31 scratch slot, and
      // the deferred FCMP would compare against a stale value. Instead
      // we record the IR Values and let the consumer call
      // `valueInFpReg` immediately before emitting FCMP, when the
      // scratch is fresh. The S vs D form of FCMP is selected at the
      // consumer based on `pendingFCmp.lhs->getType()`.
      if (auto *FC = dyn_cast<FCmpInst>(&I)) {
        Type *LT = FC->getOperand(0)->getType();
        Type *RT = FC->getOperand(1)->getType();
        bool bothFloat  = LT->isFloatTy()  && RT->isFloatTy();
        bool bothDouble = LT->isDoubleTy() && RT->isDoubleTy();
        if (!bothFloat && !bothDouble) {
          r.status = Status::Unsupported; r.reason = "fcmp shape"; return r;
        }
        unsigned condCheck = fcmpToCond(FC->getPredicate());
        if (condCheck == 0xFFu) {
          r.status = Status::Unsupported; r.reason = "fcmp predicate"; return r;
        }
        // Pre-flight scratch-collision check: if BOTH operands are raw
        // ConstantFPs (no SSA producer), they would both materialize
        // through S31 at the consumer. Reject early with a clear
        // reason; the frontend would normally constant-fold this.
        const Value *L = FC->getOperand(0);
        const Value *R = FC->getOperand(1);
        bool lhsIsConst = isa<ConstantFP>(L) && !fpRegOf.count(L);
        bool rhsIsConst = isa<ConstantFP>(R) && !fpRegOf.count(R);
        if (lhsIsConst && rhsIsConst) {
          r.status = Status::Unsupported;
          r.reason = "fcmp both ops are scratch consts";
          return r;
        }
        pendingFCmp.I = FC;
        pendingFCmp.pred = FC->getPredicate();
        pendingFCmp.lhs = L;
        pendingFCmp.rhs = R;
        continue;
      }
      
      // select i1, x, y  — lower to small conditional sequence using
      // the pendingCmp (icmp) or pendingFCmp (fcmp) recorded above.
      // We emit a CMP/FCMP, then a B.cond stub, materialize the
      // true-value, branch over the false-value, then materialize the
      // false-value, and patch up branches.
      //
      // Supported result types: i32 / i64 / float. Other types return
      // an "unsupported" status with a clear reason.
      if (auto *SI = dyn_cast<SelectInst>(&I)) {
        const Value *Cond = SI->getCondition();
        bool isIcmp = (pendingCmp.I  && pendingCmp.I  == Cond);
        bool isFcmp = (pendingFCmp.I && pendingFCmp.I == Cond);
        if (!isIcmp && !isFcmp) {
          r.status = Status::Unsupported; r.reason = "select without fused icmp/fcmp"; return r;
        }
        bool isFloatRes  = SI->getType()->isFloatTy();
        bool isDoubleRes = SI->getType()->isDoubleTy();
        bool isFpRes     = isFloatRes || isDoubleRes;
        if (!isFpRes && !SI->getType()->isIntegerTy()) {
          r.status = Status::Unsupported; r.reason = "select result type"; return r;
        }
        bool resIs64 = SI->getType()->isIntegerTy(64);

        // allocate result reg (FP or GPR depending on result type)
        int rd;
        if (isFpRes) {
          rd = assignFpReg(SI);
          if (rd < 0) { r.status = Status::Unsupported; r.reason = "fp scratch OOM (select)"; return r; }
        } else {
          rd = assignReg(SI);
          if (rd < 0) { r.status = Status::Unsupported; r.reason = "scratch OOM (select)"; return r; }
        }

        // emit CMP / FCMP based on which compare drives this select.
        unsigned cond;
        if (isFcmp) {
          // Round 8g: materialize fcmp operands HERE, not at the FCmp
          // node. Any intervening FP-constant materialization may have
          // clobbered S31/D31 since the FCmp; redoing the
          // materialization immediately before FCMP guarantees a fresh
          // scratch.
          unsigned rn, rm;
          if (!valueInFpReg(pendingFCmp.lhs, rn)) {
            r.status = Status::Unsupported; r.reason = "fcmp lhs"; return r;
          }
          if (!valueInFpReg(pendingFCmp.rhs, rm)) {
            r.status = Status::Unsupported; r.reason = "fcmp rhs"; return r;
          }
          if (rn == 31 && rm == 31) {
            r.status = Status::Unsupported;
            r.reason = "fcmp both ops are scratch consts";
            return r;
          }
          // Pick the S- or D-view of FCMP based on the operand type.
          // S/D share the V register file so the register numbers are
          // the same — only the encoding differs.
          bool fcmpIsDouble = pendingFCmp.lhs->getType()->isDoubleTy();
          if (!W.emit(fcmpIsDouble ? encFcmpD(rn, rm) : encFcmpS(rn, rm))) {
            r.status = Status::TooLarge; return r;
          }
          cond = fcmpToCond(pendingFCmp.pred);
        } else {
          if (pendingCmp.rhsImm >= 0) {
            if (!W.emit(encSubsImm32(pendingCmp.lhsReg, (unsigned)pendingCmp.rhsImm))) { r.status=Status::TooLarge; return r; }
            if (pendingCmp.is64) { r.status = Status::Unsupported; r.reason = "i64 cmp imm in select"; return r; }
          } else {
            if (pendingCmp.is64) { r.status = Status::Unsupported; r.reason = "i64 cmp reg in select"; return r; }
            if (!W.emit(encSubsReg32(pendingCmp.lhsReg, pendingCmp.rhsReg))) { r.status=Status::TooLarge; return r; }
          }
          cond = icmpToCond(pendingCmp.pred);
        }
        unsigned invCond = cond ^ 1u;
        size_t bcondPos = W.pos;
        if (!W.emit(encBcondStub(invCond))) { r.status=Status::TooLarge; return r; }

        // TRUE case: materialize true value into rd
        if (isFpRes) {
          unsigned trueReg;
          if (!valueInFpReg(SI->getTrueValue(), trueReg)) {
            r.status = Status::Unsupported; r.reason = "select true val (fp)"; return r;
          }
          if ((unsigned)rd != trueReg) {
            if (!W.emit(isDoubleRes ? encFmovRegD((unsigned)rd, trueReg)
                                    : encFmovRegS((unsigned)rd, trueReg))) {
              r.status=Status::TooLarge; return r;
            }
          }
        } else {
          unsigned trueReg;
          if (!valueInReg(SI->getTrueValue(), resIs64, trueReg)) {
            r.status = Status::Unsupported; r.reason = "select true val"; return r;
          }
          if ((unsigned)rd != trueReg) {
            if (!W.emit(encMovReg(resIs64, (unsigned)rd, trueReg))) { r.status=Status::TooLarge; return r; }
          }
        }

        // jump over false-case
        size_t bTruePos = W.pos;
        if (!W.emit(encBStub())) { r.status=Status::TooLarge; return r; }

        // patch bcond to jump here (false-case start)
        {
          int32_t diff = (int32_t)W.pos - (int32_t)bcondPos;
          int32_t imm19 = diff / 4;
          if (imm19 < -(1<<18) || imm19 >= (1<<18)) { r.status=Status::TooLarge; return r; }
          uint32_t w = 0x54000000u | (((uint32_t)imm19 & 0x7FFFFu) << 5) | (invCond & 0xFu);
          W.patch32(bcondPos, w);
        }

        // FALSE case
        if (isFpRes) {
          unsigned falseReg;
          if (!valueInFpReg(SI->getFalseValue(), falseReg)) {
            r.status = Status::Unsupported; r.reason = "select false val (fp)"; return r;
          }
          if ((unsigned)rd != falseReg) {
            if (!W.emit(isDoubleRes ? encFmovRegD((unsigned)rd, falseReg)
                                    : encFmovRegS((unsigned)rd, falseReg))) {
              r.status=Status::TooLarge; return r;
            }
          }
        } else {
          unsigned falseReg;
          if (!valueInReg(SI->getFalseValue(), resIs64, falseReg)) {
            r.status = Status::Unsupported; r.reason = "select false val"; return r;
          }
          if ((unsigned)rd != falseReg) {
            if (!W.emit(encMovReg(resIs64, (unsigned)rd, falseReg))) { r.status=Status::TooLarge; return r; }
          }
        }

        // patch bTrue to jump to continuation (TRUE target)
        {
          int32_t diff = (int32_t)W.pos - (int32_t)bTruePos;
          int32_t imm = diff / 4;
          if (imm < -(1<<25) || imm >= (1<<25)) { r.status=Status::TooLarge; return r; }
          uint32_t w = 0x14000000u | ((uint32_t)imm & 0x3FFFFFFu);
          W.patch32(bTruePos, w);
        }

        // select lowered; clear pendingCmp / pendingFCmp
        if (isIcmp && pendingCmp.I) {
          releaseIfDead(pendingCmp.I->getOperand(0));
          releaseIfDead(pendingCmp.I->getOperand(1));
        }
        if (isFcmp && pendingFCmp.I) {
          releaseIfDead(pendingFCmp.lhs);
          releaseIfDead(pendingFCmp.rhs);
        }
        releaseOperands(I);
        pendingCmp.I = nullptr;
        pendingFCmp.I = nullptr;
        continue;
      }

      // Binary ops
      if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
        // FP binary ops (fadd/fsub/fmul/fdiv) — scalar f32 and f64 are
        // supported. Both operands are routed through valueInFpReg,
        // which materializes ConstantFP values into S31/D31 via
        // x16+FMOV. Because the constant path always lands in the
        // shared scratch slot, having two ConstantFP operands in the
        // same binop would clobber the first; we explicitly reject
        // that. In practice the frontend would have constant-folded
        // such a binop already.
        if (BO->getType()->isFloatTy() || BO->getType()->isDoubleTy()) {
          bool isDouble = BO->getType()->isDoubleTy();
          auto opc = BO->getOpcode();
          if (opc != Instruction::FAdd && opc != Instruction::FSub &&
              opc != Instruction::FMul && opc != Instruction::FDiv) {
            r.status = Status::Unsupported; r.reason = "fp binop kind"; return r;
          }
          const Value *L = BO->getOperand(0);
          const Value *R = BO->getOperand(1);
          bool lhsIsConst = isa<ConstantFP>(L) && !fpRegOf.count(L);
          bool rhsIsConst = isa<ConstantFP>(R) && !fpRegOf.count(R);
          if (lhsIsConst && rhsIsConst) {
            r.status = Status::Unsupported;
            r.reason = "fp binop both ops are scratch consts";
            return r;
          }
          unsigned rn, rm;
          if (!valueInFpReg(L, rn)) {
            r.status = Status::Unsupported; r.reason = "fp binop lhs"; return r;
          }
          if (!valueInFpReg(R, rm)) {
            r.status = Status::Unsupported; r.reason = "fp binop rhs"; return r;
          }
          int rd = assignFpReg(&I);
          if (rd < 0) {
            r.status = Status::Unsupported; r.reason = "fp scratch OOM (binop)"; return r;
          }
          bool ok = false;
          switch (opc) {
          case Instruction::FAdd: ok = W.emit(isDouble ? encFaddD((unsigned)rd, rn, rm) : encFaddS((unsigned)rd, rn, rm)); break;
          case Instruction::FSub: ok = W.emit(isDouble ? encFsubD((unsigned)rd, rn, rm) : encFsubS((unsigned)rd, rn, rm)); break;
          case Instruction::FMul: ok = W.emit(isDouble ? encFmulD((unsigned)rd, rn, rm) : encFmulS((unsigned)rd, rn, rm)); break;
          case Instruction::FDiv: ok = W.emit(isDouble ? encFdivD((unsigned)rd, rn, rm) : encFdivS((unsigned)rd, rn, rm)); break;
          default: break;
          }
          if (!ok) { r.status = Status::TooLarge; return r; }
          releaseOperands(I);
          continue;
        }
        if (!BO->getType()->isIntegerTy()) { r.status=Status::Unsupported; r.reason="binop non-int"; return r; }
        unsigned bits = BO->getType()->getIntegerBitWidth();
        if (bits != 32 && bits != 64) { r.status=Status::Unsupported; r.reason="binop width"; return r; }
        bool is64 = (bits == 64);
        int rd = assignReg(&I);
        if (rd < 0) { r.status=Status::Unsupported; r.reason="scratch OOM (binop)"; return r; }
        unsigned rn;
        if (isReloadableStackArg(BO->getOperand(0))) {
          if (!loadReloadableStackArg(BO->getOperand(0), is64, (unsigned)rd)) {
            r.status=Status::Unsupported; r.reason="binop op0"; return r;
          }
          rn = (unsigned)rd;
        } else if (!valueInReg(BO->getOperand(0), is64, rn)) {
          r.status=Status::Unsupported; r.reason="binop op0"; return r;
        }
        int imm = asImm12(BO->getOperand(1));
        auto opc = BO->getOpcode();
        if (opc == Instruction::Add && imm >= 0) {
          if (!W.emit(encAddSubImm(false, is64, (unsigned)rd, rn, (unsigned)imm))) { r.status=Status::TooLarge; return r; }
          releaseOperands(I);
          continue;
        }
        if (opc == Instruction::Sub && imm >= 0) {
          if (!W.emit(encAddSubImm(true, is64, (unsigned)rd, rn, (unsigned)imm))) { r.status=Status::TooLarge; return r; }
          releaseOperands(I);
          continue;
        }
        if (opc == Instruction::Shl || opc == Instruction::LShr || opc == Instruction::AShr) {
          auto *CI = dyn_cast<ConstantInt>(BO->getOperand(1));
          if (!CI) { r.status=Status::Unsupported; r.reason="variable shift"; return r; }
          unsigned sh = (unsigned)CI->getZExtValue();
          if (sh >= (is64 ? 64u : 32u)) { r.status=Status::Unsupported; r.reason="shift oversize"; return r; }
          bool ok = (opc == Instruction::Shl)  ? W.emit(encLslImm(is64, (unsigned)rd, rn, sh)) :
                    (opc == Instruction::LShr) ? W.emit(encLsrImm(is64, (unsigned)rd, rn, sh)) :
                                                 W.emit(encAsrImm(is64, (unsigned)rd, rn, sh));
          if (!ok) { r.status=Status::TooLarge; return r; }
          releaseOperands(I);
          continue;
        }
        // reg-reg fallback (or imm-materialized into x16)
        unsigned rm;
        if (!valueInReg(BO->getOperand(1), is64, rm)) {
          r.status=Status::Unsupported; r.reason="binop op1"; return r;
        }
        bool ok = false;
        switch (opc) {
        case Instruction::Add: ok = W.emit(encAddSubReg(false, is64, (unsigned)rd, rn, rm)); break;
        case Instruction::Sub: ok = W.emit(encAddSubReg(true , is64, (unsigned)rd, rn, rm)); break;
        case Instruction::Mul: ok = W.emit(encMul(is64, (unsigned)rd, rn, rm)); break;
        case Instruction::And: ok = W.emit(encLogicReg(0, is64, (unsigned)rd, rn, rm)); break;
        case Instruction::Or:  ok = W.emit(encLogicReg(1, is64, (unsigned)rd, rn, rm)); break;
        case Instruction::Xor: ok = W.emit(encLogicReg(2, is64, (unsigned)rd, rn, rm)); break;
        default: r.status=Status::Unsupported; r.reason="binop kind"; return r;
        }
        if (!ok) { r.status=Status::TooLarge; return r; }
        releaseOperands(I);
        continue;
      }

      // Cast (trunc/zext/sext/FP-int conv/FP-precision/bitcast).
      // trunc is a register alias; zext from sub-word values is already
      // satisfied by LDRB/LDRH or 32-bit ops clearing high bits; sext
      // from i8/i16 needs an explicit SBFM. FP-int conversions and
      // bitcasts (round 8i) lower to AArch64 FCVTZS/FCVTZU/SCVTF/UCVTF
      // /FCVT/FMOV-reinterpret encoders defined near the top of this
      // file.
      if (auto *CI = dyn_cast<CastInst>(&I)) {
        // -------- FPToSI / FPToUI (FP -> GPR with truncation) --------
        if (CI->getOpcode() == Instruction::FPToSI ||
            CI->getOpcode() == Instruction::FPToUI) {
          bool isUnsigned = (CI->getOpcode() == Instruction::FPToUI);
          const char *kind = isUnsigned ? "fptoui" : "fptosi";
          Type *srcTy = CI->getOperand(0)->getType();
          Type *dstTy = CI->getType();
          bool srcIsFloat  = srcTy->isFloatTy();
          bool srcIsDouble = srcTy->isDoubleTy();
          bool dstIs32 = dstTy->isIntegerTy(32);
          bool dstIs64 = dstTy->isIntegerTy(64);
          if ((!srcIsFloat && !srcIsDouble) || (!dstIs32 && !dstIs64)) {
            r.status = Status::Unsupported;
            r.reason = std::string(kind) + " shape"; return r;
          }
          auto it = fpRegOf.find(CI->getOperand(0));
          if (it == fpRegOf.end()) {
            r.status = Status::Unsupported;
            r.reason = std::string(kind) + " src"; return r;
          }
          int rd = assignReg(&I);
          if (rd < 0) {
            r.status = Status::Unsupported;
            r.reason = std::string("scratch OOM (") + kind + ")"; return r;
          }
          uint32_t opc = 0;
          if (isUnsigned) {
            opc = srcIsDouble
                    ? (dstIs64 ? encFcvtzuXD((unsigned)rd, it->second)
                               : encFcvtzuWD((unsigned)rd, it->second))
                    : (dstIs64 ? encFcvtzuXS((unsigned)rd, it->second)
                               : encFcvtzuWS((unsigned)rd, it->second));
          } else {
            opc = srcIsDouble
                    ? (dstIs64 ? encFcvtzsXD((unsigned)rd, it->second)
                               : encFcvtzsWD((unsigned)rd, it->second))
                    : (dstIs64 ? encFcvtzsXS((unsigned)rd, it->second)
                               : encFcvtzsWS((unsigned)rd, it->second));
          }
          if (!W.emit(opc)) { r.status = Status::TooLarge; return r; }
          releaseOperands(I);
          continue;
        }
        // -------- SIToFP / UIToFP (GPR -> FP) --------
        if (CI->getOpcode() == Instruction::SIToFP ||
            CI->getOpcode() == Instruction::UIToFP) {
          bool isUnsigned = (CI->getOpcode() == Instruction::UIToFP);
          const char *kind = isUnsigned ? "uitofp" : "sitofp";
          Type *srcTy = CI->getOperand(0)->getType();
          Type *dstTy = CI->getType();
          bool srcIs32 = srcTy->isIntegerTy(32);
          bool srcIs64 = srcTy->isIntegerTy(64);
          bool dstIsFloat  = dstTy->isFloatTy();
          bool dstIsDouble = dstTy->isDoubleTy();
          if ((!srcIs32 && !srcIs64) || (!dstIsFloat && !dstIsDouble)) {
            r.status = Status::Unsupported;
            r.reason = std::string(kind) + " shape"; return r;
          }
          unsigned srcReg;
          if (!valueInReg(CI->getOperand(0), srcIs64, srcReg)) {
            r.status = Status::Unsupported;
            r.reason = std::string(kind) + " src"; return r;
          }
          int rd = assignFpReg(&I);
          if (rd < 0) {
            r.status = Status::Unsupported;
            r.reason = std::string("fp scratch OOM (") + kind + ")"; return r;
          }
          uint32_t opc = 0;
          if (isUnsigned) {
            opc = dstIsDouble
                    ? (srcIs64 ? encUcvtfDX((unsigned)rd, srcReg)
                               : encUcvtfDW((unsigned)rd, srcReg))
                    : (srcIs64 ? encUcvtfSX((unsigned)rd, srcReg)
                               : encUcvtfSW((unsigned)rd, srcReg));
          } else {
            opc = dstIsDouble
                    ? (srcIs64 ? encScvtfDX((unsigned)rd, srcReg)
                               : encScvtfDW((unsigned)rd, srcReg))
                    : (srcIs64 ? encScvtfSX((unsigned)rd, srcReg)
                               : encScvtfSW((unsigned)rd, srcReg));
          }
          if (!W.emit(opc)) { r.status = Status::TooLarge; return r; }
          releaseOperands(I);
          continue;
        }
        // -------- FPExt (float -> double) --------
        if (CI->getOpcode() == Instruction::FPExt) {
          if (!CI->getOperand(0)->getType()->isFloatTy() ||
              !CI->getType()->isDoubleTy()) {
            r.status = Status::Unsupported; r.reason = "fpext shape"; return r;
          }
          auto it = fpRegOf.find(CI->getOperand(0));
          if (it == fpRegOf.end()) {
            r.status = Status::Unsupported; r.reason = "fpext src"; return r;
          }
          int rd = assignFpReg(&I);
          if (rd < 0) {
            r.status = Status::Unsupported; r.reason = "fp scratch OOM (fpext)"; return r;
          }
          if (!W.emit(encFcvtDS((unsigned)rd, it->second))) {
            r.status = Status::TooLarge; return r;
          }
          releaseOperands(I);
          continue;
        }
        // -------- FPTrunc (double -> float) --------
        if (CI->getOpcode() == Instruction::FPTrunc) {
          if (!CI->getOperand(0)->getType()->isDoubleTy() ||
              !CI->getType()->isFloatTy()) {
            r.status = Status::Unsupported; r.reason = "fptrunc shape"; return r;
          }
          auto it = fpRegOf.find(CI->getOperand(0));
          if (it == fpRegOf.end()) {
            r.status = Status::Unsupported; r.reason = "fptrunc src"; return r;
          }
          int rd = assignFpReg(&I);
          if (rd < 0) {
            r.status = Status::Unsupported; r.reason = "fp scratch OOM (fptrunc)"; return r;
          }
          if (!W.emit(encFcvtSD((unsigned)rd, it->second))) {
            r.status = Status::TooLarge; return r;
          }
          releaseOperands(I);
          continue;
        }
        // -------- BitCast (FP <-> int reinterpret only) --------
        // Pointer bitcasts have already been folded into the pointer-
        // location chain in pass 2 (see the `BitCastInst` handler that
        // forwards `ptrLoc[BC->getOperand(0)]` to `ptrLoc[BC]`); they
        // never reach this site as a CastInst that needs lowering.
        // Pointer bitcasts that DO reach us would only occur if the
        // bitcast result is used somewhere we don't model as a ptr
        // operand — in that case the user (load/store) will already
        // have failed with a more specific reason.
        if (CI->getOpcode() == Instruction::BitCast) {
          Type *srcTy = CI->getOperand(0)->getType();
          Type *dstTy = CI->getType();
          // Pointer<->pointer bitcasts: forward ptr-loc transparently
          // (already done in pass 2). The IR value still needs to map
          // to its source's GPR/FP register (or to nothing if it's
          // pointer-only). Skip silently for pointer-shaped bitcasts
          // so chained bitcasts on alloca/global pointers continue to
          // work without consuming a scratch reg.
          if (srcTy->isPointerTy() && dstTy->isPointerTy()) continue;
          // float <-> i32
          bool floatToI32  = srcTy->isFloatTy()       && dstTy->isIntegerTy(32);
          bool i32ToFloat  = srcTy->isIntegerTy(32)   && dstTy->isFloatTy();
          // double <-> i64
          bool doubleToI64 = srcTy->isDoubleTy()      && dstTy->isIntegerTy(64);
          bool i64ToDouble = srcTy->isIntegerTy(64)   && dstTy->isDoubleTy();
          if (floatToI32 || doubleToI64) {
            // FP -> GPR. FMOV Wd,Sn / FMOV Xd,Dn.
            auto it = fpRegOf.find(CI->getOperand(0));
            if (it == fpRegOf.end()) {
              r.status = Status::Unsupported; r.reason = "bitcast src"; return r;
            }
            int rd = assignReg(&I);
            if (rd < 0) {
              r.status = Status::Unsupported; r.reason = "scratch OOM (bitcast)"; return r;
            }
            if (!W.emit(doubleToI64
                          ? encFmovXFromD((unsigned)rd, it->second)
                          : encFmovWFromS((unsigned)rd, it->second))) {
              r.status = Status::TooLarge; return r;
            }
            releaseOperands(I);
            continue;
          }
          if (i32ToFloat || i64ToDouble) {
            // GPR -> FP. FMOV Sd,Wn / FMOV Dd,Xn.
            unsigned srcReg;
            if (!valueInReg(CI->getOperand(0), i64ToDouble, srcReg)) {
              r.status = Status::Unsupported; r.reason = "bitcast src"; return r;
            }
            int rd = assignFpReg(&I);
            if (rd < 0) {
              r.status = Status::Unsupported; r.reason = "fp scratch OOM (bitcast)"; return r;
            }
            if (!W.emit(i64ToDouble
                          ? encFmovDFromX((unsigned)rd, srcReg)
                          : encFmovSFromW((unsigned)rd, srcReg))) {
              r.status = Status::TooLarge; return r;
            }
            releaseOperands(I);
            continue;
          }
          // Same-class same-width int<->int bitcasts (rare but valid):
          // forward the source register without emitting code.
          if (srcTy->isIntegerTy() && dstTy->isIntegerTy() &&
              srcTy->getIntegerBitWidth() == dstTy->getIntegerBitWidth()) {
            auto it = regOf.find(CI->getOperand(0));
            if (it == regOf.end()) {
              r.status = Status::Unsupported; r.reason = "bitcast src"; return r;
            }
            regOf[&I] = it->second;
            continue;
          }
          r.status = Status::Unsupported; r.reason = "bitcast shape"; return r;
        }
        if (CI->getOpcode() == Instruction::Trunc ||
            CI->getOpcode() == Instruction::ZExt  ||
            CI->getOpcode() == Instruction::SExt) {
          Type *SrcTy = CI->getOperand(0)->getType();
          Type *DstTy = CI->getType();
          if (!SrcTy->isIntegerTy() || !DstTy->isIntegerTy()) {
            r.status = Status::Unsupported; r.reason = "cast non-int"; return r;
          }
          unsigned srcBits = SrcTy->getIntegerBitWidth();
          unsigned dstBits = DstTy->getIntegerBitWidth();
          bool srcIs64 = srcBits == 64;
          unsigned srcReg = 0;
          bool srcReload = isReloadableStackArg(CI->getOperand(0));
          if (srcReload) {
            int rd = assignReg(&I);
            if (rd < 0) {
              r.status = Status::Unsupported; r.reason = "scratch OOM (cast reload)"; return r;
            }
            if (!loadReloadableStackArg(CI->getOperand(0), srcIs64, (unsigned)rd)) {
              r.status = Status::Unsupported; r.reason = "cast src not in reg"; return r;
            }
            srcReg = (unsigned)rd;
          } else {
            auto it = regOf.find(CI->getOperand(0));
            if (it == regOf.end()) {
              r.status = Status::Unsupported; r.reason = "cast src not in reg"; return r;
            }
            srcReg = it->second;
          }
          if (CI->getOpcode() == Instruction::SExt &&
              (srcBits == 8 || srcBits == 16) &&
              (dstBits == 32 || dstBits == 64)) {
            int rd = srcReload ? (int)srcReg : assignReg(&I);
            if (rd < 0) {
              r.status = Status::Unsupported; r.reason = "scratch OOM (sext)"; return r;
            }
            if (!W.emit(encExtendBits(true, dstBits == 64, (unsigned)rd, srcReg, srcBits))) {
              r.status = Status::TooLarge; return r;
            }
            releaseOperands(I);
            continue;
          }
          if (CI->getOpcode() == Instruction::SExt &&
              srcBits == 32 && dstBits == 64) {
            int rd = srcReload ? (int)srcReg : assignReg(&I);
            if (rd < 0) {
              r.status = Status::Unsupported; r.reason = "scratch OOM (sext)"; return r;
            }
            if (!W.emit(encExtendBits(true, true, (unsigned)rd, srcReg, 32))) {
              r.status = Status::TooLarge; return r;
            }
            releaseOperands(I);
            continue;
          }
          if (CI->getOpcode() == Instruction::ZExt &&
              (srcBits == 8 || srcBits == 16) &&
              (dstBits == 32 || dstBits == 64)) {
            int rd = srcReload ? (int)srcReg : assignReg(&I);
            if (rd < 0) {
              r.status = Status::Unsupported; r.reason = "scratch OOM (zext)"; return r;
            }
            if (!W.emit(encExtendBits(false, dstBits == 64, (unsigned)rd, srcReg, srcBits))) {
              r.status = Status::TooLarge; return r;
            }
            releaseOperands(I);
            continue;
          }
          regOf[&I] = srcReg;
          releaseOperands(I);
          continue;
        }
        r.status = Status::Unsupported; r.reason = "cast kind"; return r;
      }

      // Terminators.
      if (auto *RI = dyn_cast<ReturnInst>(&I)) {
        // Before return, if this block has phi-target successors... no, we
        // came from 'br label %.exit' already and did phi copy then.
        if (!epilogueRet(RI->getReturnValue())) {
          r.status = Status::Unsupported; r.reason = "ret lowering"; return r;
        }
        continue;
      }
      if (auto *BR = dyn_cast<BranchInst>(&I)) {
        // Emit phi copies at each successor: for every phi in succ, move its
        // incoming value for BB into the phi's register.
        auto emitPhiCopies = [&](const BasicBlock *succ) -> bool {
          for (const Instruction &J : *succ) {
            auto *PN = dyn_cast<PHINode>(&J);
            if (!PN) break;
            Value *inc = PN->getIncomingValueForBlock(&BB);
            if (PN->getType()->isFloatTy() || PN->getType()->isDoubleTy()) {
              bool isDouble = PN->getType()->isDoubleTy();
              auto itR = fpRegOf.find(PN);
              if (itR == fpRegOf.end()) return false;
              unsigned phiReg = itR->second;
              unsigned srcReg;
              if (!valueInFpReg(inc, srcReg)) return false;
              if (srcReg != phiReg) {
                if (!W.emit(isDouble ? encFmovRegD(phiReg, srcReg)
                                     : encFmovRegS(phiReg, srcReg)))
                  return false;
              }
              continue;
            }
            bool is64 = PN->getType()->isIntegerTy(64);
            auto itR = regOf.find(PN);
            if (itR == regOf.end()) return false;
            unsigned phiReg = itR->second;
            unsigned srcReg;
            if (!valueInReg(inc, is64, srcReg)) return false;
            if (srcReg != phiReg) {
              if (!W.emit(encMovReg(is64, phiReg, srcReg))) return false;
            }
          }
          return true;
        };

        if (BR->isUnconditional()) {
          if (!emitPhiCopies(BR->getSuccessor(0))) {
            r.status = Status::Unsupported; r.reason = "phi copy"; return r;
          }
          // B stub + fixup.
          size_t p = W.pos;
          if (!W.emit(encBStub())) { r.status=Status::TooLarge; return r; }
          fixups.push_back({p, BR->getSuccessor(0), false, 0});
          continue;
        }
        // Conditional: must be fused with the pending icmp or fcmp.
        bool brIsIcmp = (pendingCmp.I  && BR->getCondition() == pendingCmp.I);
        bool brIsFcmp = (pendingFCmp.I && BR->getCondition() == pendingFCmp.I);
        if (!brIsIcmp && !brIsFcmp) {
          r.status = Status::Unsupported;
          r.reason = "cond br without fused icmp/fcmp";
          return r;
        }
        // emit CMP / FCMP.
        if (brIsFcmp) {
          // Round 8g: materialize fcmp operands HERE so we use a fresh
          // S31 scratch even if intervening FP-constant materialization
          // happened between the FCmp and this branch.
          unsigned rn, rm;
          if (!valueInFpReg(pendingFCmp.lhs, rn)) {
            r.status = Status::Unsupported; r.reason = "fcmp lhs"; return r;
          }
          if (!valueInFpReg(pendingFCmp.rhs, rm)) {
            r.status = Status::Unsupported; r.reason = "fcmp rhs"; return r;
          }
          if (rn == 31 && rm == 31) {
            r.status = Status::Unsupported;
            r.reason = "fcmp both ops are scratch consts";
            return r;
          }
          // S vs D dispatch based on the operand type recorded at the
          // FCmp site. Both operands have already been validated to
          // share the same FP type at FCmp lowering time.
          bool fcmpIsDouble = pendingFCmp.lhs->getType()->isDoubleTy();
          if (!W.emit(fcmpIsDouble ? encFcmpD(rn, rm) : encFcmpS(rn, rm))) {
            r.status=Status::TooLarge; return r;
          }
        } else if (pendingCmp.rhsImm >= 0) {
          if (!W.emit(encSubsImm32(pendingCmp.lhsReg, (unsigned)pendingCmp.rhsImm))) {
            r.status=Status::TooLarge; return r;
          }
          // NB: we only emit a 32-bit CMP (wN) even if the icmp was on i64,
          // because the target IR never does that; if is64 is true, bail.
          if (pendingCmp.is64) {
            r.status = Status::Unsupported; r.reason = "i64 cmp imm"; return r;
          }
        } else {
          if (pendingCmp.is64) {
            r.status = Status::Unsupported; r.reason = "i64 cmp reg"; return r;
          }
          if (!W.emit(encSubsReg32(pendingCmp.lhsReg, pendingCmp.rhsReg))) {
            r.status=Status::TooLarge; return r;
          }
        }

        const BasicBlock *Tsucc = BR->getSuccessor(0);
        const BasicBlock *Fsucc = BR->getSuccessor(1);

        // Phi copies for TRUE path, then B.cond to TRUE; then phi copies for
        // FALSE path, then B to FALSE.
        //
        // If TRUE and FALSE share phi destinations, we may need distinct
        // copies. Simpler correct layout: emit a small trampoline.
        //
        //     <phi copies for TRUE>
        //     B.cond  -> afterFalse      (taken → TRUE)
        //     <phi copies for FALSE>
        //     B       -> FALSE
        //   afterFalse:
        //     B       -> TRUE
        //
        // But because we can't emit labels mid-stream easily, we restructure:
        //
        //     B.cond (invert) -> FALSE_LABEL
        //     <phi copies for TRUE>
        //     B       -> TRUE
        //   FALSE_LABEL:
        //     <phi copies for FALSE>
        //     B       -> FALSE
        //
        // To implement this we emit a B.cond whose target is a synthetic
        // 'mid' point in THIS buffer, and backpatch when we know mid's pos.
        unsigned cond = brIsFcmp ? fcmpToCond(pendingFCmp.pred)
                                 : icmpToCond(pendingCmp.pred);
        unsigned invCond = cond ^ 1u; // invert low bit flips EQ<->NE, etc.
        size_t bcondPos = W.pos;
        if (!W.emit(encBcondStub(invCond))) { r.status=Status::TooLarge; return r; }
        // TRUE edge sequence.
        if (!emitPhiCopies(Tsucc)) { r.status=Status::Unsupported; r.reason="phi copy T"; return r; }
        size_t bTruePos = W.pos;
        if (!W.emit(encBStub())) { r.status=Status::TooLarge; return r; }
        fixups.push_back({bTruePos, Tsucc, false, 0});
        // Backpatch bcondPos to jump here (FALSE edge start).
        {
          int32_t diff = (int32_t)W.pos - (int32_t)bcondPos;
          int32_t imm19 = diff / 4;
          if (imm19 < -(1<<18) || imm19 >= (1<<18)) { r.status=Status::TooLarge; return r; }
          uint32_t w = 0x54000000u | (((uint32_t)imm19 & 0x7FFFFu) << 5) | (invCond & 0xFu);
          W.patch32(bcondPos, w);
        }
        // FALSE edge sequence.
        if (!emitPhiCopies(Fsucc)) { r.status=Status::Unsupported; r.reason="phi copy F"; return r; }
        size_t bFalsePos = W.pos;
        if (!W.emit(encBStub())) { r.status=Status::TooLarge; return r; }
        fixups.push_back({bFalsePos, Fsucc, false, 0});
        if (brIsIcmp && pendingCmp.I) {
          releaseIfDead(pendingCmp.I->getOperand(0));
          releaseIfDead(pendingCmp.I->getOperand(1));
        }
        if (brIsFcmp && pendingFCmp.I) {
          releaseIfDead(pendingFCmp.lhs);
          releaseIfDead(pendingFCmp.rhs);
        }
        releaseOperands(I);
        pendingCmp.I = nullptr;
        pendingFCmp.I = nullptr;
        continue;
      }

      r.status = Status::Unsupported;
      r.reason = std::string("opcode: ") + I.getOpcodeName();
      return r;
    }
  }

  // ---------- Backpatch branch fixups. ----------
  for (const Fixup &F : fixups) {
    auto it = bbStart.find(F.target);
    if (it == bbStart.end()) {
      r.status = Status::Unsupported; r.reason = "branch to unknown BB"; return r;
    }
    int32_t diff = (int32_t)it->second - (int32_t)F.pos;
    int32_t imm = diff / 4;
    if (F.bcond) {
      if (imm < -(1<<18) || imm >= (1<<18)) { r.status=Status::TooLarge; return r; }
      uint32_t w = 0x54000000u | (((uint32_t)imm & 0x7FFFFu) << 5) | (F.cond & 0xFu);
      W.patch32(F.pos, w);
    } else {
      if (imm < -(1<<25) || imm >= (1<<25)) { r.status=Status::TooLarge; return r; }
      uint32_t w = 0x14000000u | ((uint32_t)imm & 0x3FFFFFFu);
      W.patch32(F.pos, w);
    }
  }

  r.codeBytes = W.pos;
  r.status = Status::Ok;
  return r;
}

// ------------------------------ compile ------------------------------

void *light::compile(const Function &Fn, Result &out,
                     const GlobalSymbol *globals, size_t nglobals) {
  const size_t pageSize = (size_t)sysconf(_SC_PAGESIZE);
  const size_t codeSize = pageSize * 4;
  void *page = ::mmap(nullptr, codeSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) { out.status = Status::TooLarge; out.reason = "mmap"; return nullptr; }
  out = emit(Fn, (uint8_t *)page, codeSize, globals, nglobals);
  if (out.status != Status::Ok) { ::munmap(page, codeSize); return nullptr; }
  __builtin___clear_cache((char *)page, (char *)page + out.codeBytes);
  if (::mprotect(page, codeSize, PROT_READ | PROT_EXEC) != 0) {
    ::munmap(page, codeSize);
    out.status = Status::TooLarge; out.reason = "mprotect"; return nullptr;
  }
  return page;
}
