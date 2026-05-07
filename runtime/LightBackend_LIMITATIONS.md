# EasyJIT Light Backend Limitations

This file tracks the intentional limits of the AArch64 light backend used by
`EASYJIT_LIGHT_BACKEND_ONLY=ON`. It is also pulled in automatically by the
top-level `check` target when the light backend is enabled.

## Fallback Behavior

Light-only builds do not contain ORC/LLVM CodeGen fallback. If the light backend
rejects a function, the runtime can only fall back to the original function
pointer when the specialized call signature is identical to the original
function signature.

Automatic original-pointer fallback is allowed only when:

- every original argument is forwarded unchanged;
- forwarded argument indexes are exactly `0, 1, 2, ...`;
- the context argument count matches the original function argument count.

Fallback is intentionally rejected when EasyJIT removes or binds arguments, for
example `easy::jit(add, _1, 1)`. In that case the returned wrapper has a
different signature from the original function pointer, so calling the original
pointer would be an ABI bug. The runtime throws `LightBackendCompileError`
instead.

When fallback is used and `EASYJIT_LIGHT_VERBOSE` is non-zero, the runtime prints
the light-backend rejection reason to stderr.

## Currently Supported Triples

The light backend's triple gate (`light::emit`) accepts:

- `aarch64-*`            — AArch64 LP64 little-endian.
- `aarch64_be-*`         — AArch64 LP64 big-endian.
- `arm64-*`              — Apple-style alias for AArch64 LP64 LE.

It rejects with `Status::NotAarch64`:

- `aarch64_32-*`, `arm64_32-*` — ILP32 AArch64 variants.
- Anything else (`x86_64-*`, `riscv64-*`, ...).

The status name `Status::NotAarch64LE` is preserved as a deprecated alias
of `Status::NotAarch64` for source-level back-compat with rounds 7 and
earlier; new code should match `NotAarch64`.

### Endian model (round 8)

- AArch64 instruction encoding is unconditionally little-endian (ARM ARM
  B2.6.2). `Writer::emit` writes explicit LE bytes for every 32-bit
  instruction word, so emission is correct on any host.
- `LDR`/`STR` honour target data endian (SCTLR_EL1.EE). The light
  backend always JIT-runs the code it produces in the same process, so
  target data endian == host endian; producer and consumer of any data
  buffer agree by construction.
- `MOVZ`/`MOVK` halfwords address `hw` positions, not bytes, so
  immediate materialization is endian-neutral.
- Sub-word loads use `LDRB`/`LDRH`; sign extension is encoded explicitly
  via `SBFM`, so signed sub-word fields work on both endians.
- `MaterializePrivateGlobals` (in `runtime/LightBackend.cpp`) reifies
  PrivateLinkage `ConstantData*` initializers into heap buffers using
  host byte order (`memcpy` of the underlying value). The `LDR` reading
  those bytes uses target data endian, which equals host endian, so
  the bytes line up on both `aarch64-*` and `aarch64_be-*`.

The triple-acceptance and instruction-byte-stream parity claims are
checked in-tree by `light_endian_parity_test` (built whenever
`EASYJIT_ENABLE_LIGHT_BACKEND=ON`). End-to-end execution on a real
`aarch64_be` machine is **not** part of the regression set; that
final-mile claim is deferred to a target-machine validation pass.

## Currently Supported IR Shape

- AArch64/aarch64_be target only.
- Integer and pointer arguments in `x0..x7`.
- Float arguments in `s0..s7`.
- Double-precision arguments in `d0..d7` (round 8h). The same V
  register file backs both `s` and `d` views, so the AAPCS allocation
  pool and `fpRegOf` register numbering are shared between `float`
  and `double` arguments.
- Stack-passed scalar arguments beyond the first eight GPR-class /
  FP-class slots (round 8j, refined after the pressure gauntlet). The supported subset is
  `i32`, `i64`, pointer, `float`, `double`. Each overflow argument
  occupies an 8-byte AAPCS64 NSAA slot regardless of natural width;
  GPR-overflow and FP-overflow share a single overflow area in
  original parameter order (per AAPCS64 §6.4 NSAA — they do **not**
  start at independent zero offsets). Pointer and FP overflow args are
  still preloaded immediately after the entry prologue, via a single
  `LDR X/S/D, [sp, #frameSize+incomingOff]`, because pointer tracking
  and V-register use need stable locations. Non-pointer GPR overflow
  args are lazy-reloadable: the lowering records their incoming slot and
  reloads them with `LDR W/X` only at the integer use site, which avoids
  permanently consuming one scratch register per stack argument. The
  reloaded values feed the existing binop / load / store / GEP / fcmp /
  select / ret / cast paths just like in-register arguments. Pointer
  stack args correctly drive pointer-tracking (`ptrLoc[&A] =
  InReg{regOf[&A]}`). When too many preloaded stack args exhaust the
  scratch pool the lowering rejects with `"scratch OOM (stack arg)"` /
  `"fp scratch OOM (stack arg)"`; if the offset cannot be encoded in the
  LDR uimm12 the rejection is `"stack arg offset/encoding"`.
- Extended GPR scratch pool (round 8l): the emitter saves `x19..x28`
  into the local frame in the prologue, makes those registers available
  after the caller-saved `x8..x15` pool is exhausted, and restores them
  before every return. This is a pressure-relief mechanism rather than a
  full SSA spill/reload allocator: GPR-heavy scalar expressions that need
  up to 18 scratch registers now compile, while shapes requiring more
  simultaneously assigned GPR values still reject cleanly.
- Local SSA scratch reuse (round 8m, refined for dynamic-GEP address
  terms): within one basic block, temporary
  instruction results are returned to a small free list after their last
  same-block use. This lets straight-line post-specialization expression
  chains reuse GPR and FP scratch registers instead of permanently
  consuming one slot per SSA name. Values that cross blocks, feed PHIs, or
  act as dynamic-GEP index terms stay pinned until their hidden address
  uses have been materialised. Dynamic-GEP terms are counted separately
  from ordinary SSA uses and released after the final `LDR`/`STR`
  address materialisation; PHIs and arguments remain pinned.
  Round 8m extends this to the binop emit path itself: integer and FP
  binops now drop their dead operand mappings *before* the destination
  register is allocated, so a freshly-released operand register can be
  reused as the destination in the same instruction. This is safe
  because every supported AArch64 ALU/FP encoding here reads its source
  operands before writing the destination. The reloadable-stack-arg
  path keeps the original ordering (allocate `rd` first, then load the
  arg into it) so that loading op0 cannot clobber op1's value.
- Minimal GPR spill/reload (round 10): when `assignReg` runs out of
  scratch registers, the emitter now picks one currently-mapped local
  integer/pointer SSA temp as a spill victim, emits an
  `STR W/X, [sp, #spillOff]` into a frame-resident slot, evicts the
  victim from `regOf`, and hands the freed register to the requester.
  When the spilled value is next consumed, `valueInReg` allocates a
  fresh register (which may itself trigger another spill) and emits
  `LDR W/X, [sp, #spillOff]` to materialize the value, then recycles
  the slot. The spill area is reserved between the saved `x19..x28`
  block and the final 16-byte alignment, capped at 32 slots (256 B);
  reservation only happens when `useSavedGprScratch` is on AND the
  estimated GPR live set already exceeds the combined caller-saved
  plus saved-scratch pool. The mechanism is intentionally conservative:
    * Only `i32`, `i64`, and pointer-width SSA temps (Instructions) are
      spilled. PHIs, function `Argument`s, dynamic-GEP hidden index
      terms while still in use, the `ptrLoc` owner, the
      reloadable-stack-arg lazy-load reg, and any Value that is live
      across basic blocks are off-limits as victims.
    * FP / V-register temps are never spilled.
    * The recursion guard `reloading` prevents the value being reloaded
      from being picked as its own victim, and `pinnedFromSpill`
      protects all values that the in-flight instruction has already
      pulled into a local (`rn`/`rm`/...) so that a reload that piggy-
      backs on the same allocation cannot clobber an active operand
      register before the instruction emits. The pin set is cleared at
      the top of every instruction body.
    * If no eligible victim exists or the slot offset cannot be encoded
      in the LDR/STR uimm12 scaled immediate, the lowering falls back
      to the original `"scratch OOM (binop)"` (or equivalent)
      `Status::Unsupported` rejection — same as pre-round-10.
  This is the missing rung that lets the explicit
  `be_backend_probe_o0 --case gauntlet` case compile under
  `EASYJIT_LIGHT=force`. It is **not** a full register allocator: there
  is no live-range analysis, no cross-block spill, no rematerialization,
  no FP/vector spill, no PHI spill, and no support for spilling values
  that participate in `ptrLoc` address calculations or stack-arg lazy
  reload locations.
- Fixed-size executable code allocation: each light-compiled function
  currently gets four host pages of RX code storage (typically 16 KiB),
  mapped RW during emission and flipped RX before return. This is still
  intentionally simple: functions whose emitted code would exceed the
  fixed cap reject instead of growing the mapping.
- Fixed-size stack frame, constant-offset alloca GEP.
- Dynamic scaled GEP with up to **two** dynamic terms (round 8k):

      base + constOff + idx0 * pow2Scale0 [+ idx1 * pow2Scale1]

  Each dynamic index is an `i32` or `i64` Value (sext/zext from
  `i32` is looked through, recording the SXTW vs UXTW choice).
  Each scale must be a power of two with `log2(scale)` in
  `0..12`. The base may be a `PtrLoc::Absolute` (inttoptr / known
  global) or a `PtrLoc::InReg` (function-arg pointer, including
  one preloaded from a stack arg). The address is materialised in
  `x17` as base, then per term:
    * `log2(scale) <= 4`: single
      `ADD x17, x17, Wm/Xm, {SXTW|UXTW|UXTX} #log2(scale)`.
    * `log2(scale) in 5..12`: 3-instruction sequence —
      `SXTW/UXTW/MOV x16, idx`, `LSL x16, x16, #log2(scale)`,
      `ADD x17, x17, x16, UXTX #0`. `x16` is treated as
      address-materialisation scratch and is safe because no
      immediate-materialisation runs between us and the final
      `LDR/STR`.
  This covers the common `base[i][j]` pattern over nested
  arrays (`int (*)[N]`, `double (*)[N]`) and multi-index struct
  GEPs that have a constant struct-field prefix and two array
  indices (`s->arr[i][j]`).
- `i8/i16/i32/i64` loads and stores.
- `float` loads and stores.
- `double` loads and stores (round 8h) — `LDR Dt`/`STR Dt` with the
  size=11, scale=8 unsigned-immediate-offset encoding (`fitsScaled`
  shift = 3, so byte offset must be 8-byte aligned and within
  `0..32760`).
- Integer arithmetic: `add/sub/mul/and/or/xor/shl/lshr/ashr`.
- Arbitrary `i32` / `i64` integer constants — including negative
  values — used as binop RHS or as the value of an integer `ret` are
  materialized through a generic MOVZ + (optional) MOVK halfword
  chain (`emitMovImm` / `materializeImmAny` in
  `light_codegen/light_aarch64.cpp`):
  one MOVZ for `0..0xFFFF`; MOVZ + MOVK,lsl#16 for `0x10000..0xFFFFFFFF`;
  up to four MOVZ/MOVK halfwords for full 64-bit constants. Negative
  values are handled by materializing the unsigned two's-complement
  bit pattern at the operation width (32 or 64 bits): for example
  `-12345` (i32) becomes `MOVZ W,#0xCFC7 + MOVK W,#0xFFFF,lsl#16`,
  and `-1` (i64) becomes `MOVZ X,#0xFFFF + 3× MOVK ,#0xFFFF`. We do
  NOT emit `MOVN`; the bit-pattern path keeps the helper symmetric
  for positive and negative values and avoids a second encoder. The
  12-bit immediate ADD/SUB fast path still wins when the constant
  fits and is non-negative. Note: this guarantee is for `i32` and
  `i64` constants used by an integer binop or `ret` of matching
  width; narrow-integer (`i1`/`i8`/`i16`) negative constants used
  directly in a same-width binop are not promised — the frontend is
  expected to widen the operation first.
- Integer comparisons when fused into a following conditional branch.
- Integer and float phi copies.
- `llvm.memcpy` with constant size up to 32 bytes.
- `llvm.fmuladd.f32`.
- Standalone scalar single-precision arithmetic: `fadd`, `fsub`, `fmul`,
  `fdiv` on `float` (round 8f). `ConstantFP` operands are materialized
  through `valueInFpReg` (FMOV-imm fast path for representable
  immediates such as `0.0f` / `2.0f`; otherwise via integer MOVZ/MOVK
  + `FMOV s,w` into the scratch register `s31`). Both operands of a
  single FP binop being unmaterialized `ConstantFP` is rejected
  ("fp binop both ops are scratch consts") because they would both
  land in `s31`.
- Standalone scalar double-precision arithmetic: `fadd`, `fsub`,
  `fmul`, `fdiv` on `double` (round 8h). The encodings reuse the
  single-precision op slots with bit 22 (the `type` field) toggled to
  `01`. `ConstantFP` doubles are materialized via a 64-bit MOVZ +
  optional MOVK halfword chain on `x16` followed by `FMOV Dn, X16`;
  the zero fast path uses `FMOV Dn, XZR`. The scratch slot is `d31`,
  which is the same V register as `s31`; the existing "both ops are
  scratch consts" pre-flight check applies unchanged.
- `fneg` on `float` and `double` (round 8m). Lowered to scalar
  `FNEG Sd, Sn` / `FNEG Dd, Dn` (encoding family
  `0001_1110_0X1_00001_010000_nnnnn_ddddd`, type=00 for single,
  type=01 for double). Required because clang since LLVM 13 emits
  `fneg` directly for both `-x` and `0.0 - x`, so a select branch
  with a negated alternative now stays inside the light backend
  instead of falling back. The dead-operand release runs before the
  destination register is allocated, mirroring the round-8m binop
  fix below.
- Scalar single-precision ordered comparisons (round 8f, hardened in
  round 8g), fused into the immediately-following conditional branch
  or `select`. Supported predicates: `FCMP_OEQ` (EQ), `FCMP_OGT` (GT),
  `FCMP_OGE` (GE), `FCMP_OLT` (MI — *not* AArch64 LT, which fires on
  NaN since V==1 inverts N), `FCMP_OLE` (LS). Unordered predicates
  and `FCMP_ONE` are intentionally rejected — see Known Unsupported
  Areas. Round 8g hardening: fcmp operands are *not* materialized at
  the FCmp node; the consumer (br/select) calls `valueInFpReg` for
  both operands immediately before emitting `FCMP`. This guarantees
  the `s31` scratch is fresh, even if any FP-constant materialization
  occurs between the FCmp and its consumer (e.g. an intervening
  `%t = fadd float %x, 2.0` no longer corrupts the FCMP RHS).
- Scalar double-precision ordered comparisons (round 8h): same
  predicate set as `float`, fused into a following branch or
  `select`. Both operands must share the same FP type — mixed
  `float`/`double` operands in one `fcmp` are rejected with
  `"fcmp shape"`. The S vs D form of `FCMP` is selected at the
  consumer based on the recorded operand type. The same round-8g
  deferred-operand discipline applies; the `d31` view of the shared
  scratch is refreshed immediately before `FCMP`.
- `select` with float OR double result type (round 8h), driven by
  either an icmp or an ordered fcmp. The TRUE/FALSE FMOV between
  V-registers picks the S- or D-view based on the select result type.
- `ret float` / `ret double` of either an SSA FP value already in an
  FP register (FMOV S0, Sn / FMOV D0, Dn) **or** a `ConstantFP`
  directly (round 8g for float, round 8h for double). Constants are
  materialized straight into `s0` / `d0` via the shared
  `materializeFpConstToReg` helper.
- `fptosi float -> i32` (FCVTZS Wd, Sn) and `fptosi double -> i32`
  (FCVTZS Wd, Dn, round 8h). Conversion uses the round-toward-zero
  saturating semantics required by the C standard.
- Round 8i: full scalar FP↔int / FP↔FP conversion family. All
  conversions use the canonical AArch64 conversion encoders that
  share the same FP-int conversion bit slot, so the light backend
  emits exactly one instruction per IR conversion node:
  - `fptosi {float,double} -> {i32,i64}`: `FCVTZS Wd,Sn` /
    `FCVTZS Wd,Dn` / `FCVTZS Xd,Sn` / `FCVTZS Xd,Dn`.
  - `fptoui {float,double} -> {i32,i64}`: same shape with `FCVTZU`.
  - `sitofp {i32,i64} -> {float,double}`: `SCVTF Sd,Wn` /
    `SCVTF Sd,Xn` / `SCVTF Dd,Wn` / `SCVTF Dd,Xn`.
  - `uitofp {i32,i64} -> {float,double}`: same shape with `UCVTF`.
  - `fpext float -> double`: `FCVT Dd,Sn` (precision change,
    type=00→01, opc=01).
  - `fptrunc double -> float`: `FCVT Sd,Dn` (precision change,
    type=01→00, opc=00).
  - `bitcast` reinterpret: `float <-> i32` via `FMOV Wd,Sn` /
    `FMOV Sd,Wn`; `double <-> i64` via `FMOV Xd,Dn` / `FMOV Dd,Xn`.
    Same-width same-class integer bitcasts (rare, e.g. `i32 <-> i32`
    in transformed IR) forward the source register without emitting
    code. Pointer↔pointer bitcasts are still folded into the
    `ptrLoc` pointer-tracking chain in pass 2 and never reach the
    code-emission path.
- `sext/zext/trunc` over integer widths used by current tests.

## Known Unsupported Areas

- `double` / `f64` conversions other than `fptosi double -> i32`:
  `fpext float -> double`, `fptrunc double -> float`, `fptoui` of any
  width, `sitofp`/`uitofp` from any integer width, `bitcast double
  <-> i64` are all supported as of round 8i — see Currently Supported
  IR Shape. Out-of-scope items in this family are the FP-class
  intrinsics (e.g. `llvm.is.fpclass`), saturating conversion
  intrinsics, and the half/bfloat/long-double FP types.
- Vector IR.
- Unordered FP comparison predicates (`FCMP_UEQ`, `FCMP_UNE`,
  `FCMP_UGT`, `FCMP_UGE`, `FCMP_ULT`, `FCMP_ULE`, `FCMP_UNO`,
  `FCMP_ORD`, `FCMP_TRUE`, `FCMP_FALSE`). AArch64 ordered conditions
  treat `unordered` as a fall-through to the FALSE edge by design;
  unordered semantics would require a second branch or `CCMP` and are
  out of scope for the light backend.
- `FCMP_ONE` ("ordered AND not equal"). AArch64 has no single-condition
  encoding; emitting it correctly would require either two branches or
  a `CCMP` chain. Frontends should canonicalize to `!FCMP_UEQ` ahead of
  the light backend or accept rejection.
- `ret` of a `ConstantFP` directly is supported for both `float`
  (round 8g) and `double` (round 8h). Other FP semantics
  (long-double, `bfloat`, `half`) remain out of scope.
- An FP binop or FCmp where *both* operands are unmaterialized
  `ConstantFP` (`s31` scratch clobber). The FCmp pre-flight check
  catches this at the FCmp node (reason `"fcmp both ops are scratch
  consts"`); the binop check stays at the binop site (reason `"fp
  binop both ops are scratch consts"`).
- Stack-passed scalar arguments beyond the first eight GPR-class /
  FP-class slots are now supported for `i32`/`i64`/pointer/`float`/
  `double` (round 8j plus the lazy GPR reload refinement, see
  preamble). Still unsupported in the
  stack-arg path: aggregate-by-value, homogeneous floating-point
  aggregates (HFA), varargs, `i8`/`i16` overflow args (rejected with
  reason `"stack arg shape (i8/i16)"`), scratch register exhaustion
  for pointer/FP preloads after the saved-`x19..x28` extension is also
  exhausted (rejected with `"scratch OOM (stack arg)"` /
  `"fp scratch OOM (stack arg)"`), later use-site scratch exhaustion
  when too many lazy-reloaded GPR stack args are live at once, and
  offsets that don't fit in the scaled `LDR` uimm12 encoding
  (rejected with `"stack arg offset/encoding"`).
- Full SSA spilling/reloading under very high register pressure. Rounds
  8l/8m plus the lazy stack-arg reload refinement expand the practical
  envelope with saved GPR scratch registers, same-block scratch reuse,
  and demand-loaded non-pointer GPR overflow args, but the backend still
  does not spill arbitrary live SSA values to stack slots and reload
  them on demand. FP scratch remains capped at `d16..d30` plus the
  reserved constant scratch `d31`.
- Dynamic GEP shapes beyond the round-8k two-term form: more than
  two dynamic indices in the GEP chain, dynamic index scales that
  are not powers of two, scales with `log2 > 12`, and arithmetic
  expressions in the index slot that are not a sext/zext of a
  scratch-resident i32/i64 (e.g. `add (mul i, C), j` is not
  recognised — frontends should encode such patterns through nested
  array types or struct GEPs so the resolver sees them as separate
  dynamic indices).
- Complex C++ frontend shapes such as exceptions, RTTI-heavy code, virtual
  dispatch that is not devirtualized, and non-trivial object lifetime code.
- Serialization fallback: deserialized bitcode has no original function pointer
  to call, so unsupported deserialized modules still throw.
- Live execution on a real `aarch64_be` machine is not part of the
  in-tree regression set. The triple gate, encoder LE byte stream, and
  emit-time output parity are checked by `light_endian_parity_test`,
  but the runtime side has never been exercised on a BE CPU; if the
  light backend is ported to BE silicon this should be the first
  targeted validation pass.

## Validation Notes

The light-only C API regression set currently covers:

- `add_int`
- `array_snapshot`
- `cache_example`
- `global_snapshot`
- `global_partial_snapshot`
- `mixed_bindings`
- `partial_struct_binding`
- `pointer_field_snapshot`
- `struct_snapshot`
- `wireless_beamform`

In addition, `light_endian_parity_test` (built under
`EASYJIT_ENABLE_LIGHT_BACKEND=ON`) covers the triple gate and the
emit-time LE / BE / arm64 instruction-byte-stream parity claim.

Run it directly:

    cmake --build <build-dir> --target check-light-endian

It is also pulled in automatically by the top-level `check` target when
the light backend is enabled. The test exercises LDR/LDRH/STR, the
12-bit-immediate ADD fast path, **and** a >16-bit constant ADD that
forces the binop RHS through `materializeImmAny` →  `emitMovImm`,
producing a real `MOVZ Wd,#lo` + `MOVK Wd,#hi,lsl #16` halfword chain
in the byte stream. The test asserts presence of at least one MOVZ and
one MOVK opcode word in the LE stream as a coverage guard, so any
future change that silently bypasses the wide-immediate path will be
caught here. (MOVZ/MOVK halfwords are also exercised independently by
the absolute-address path used for snapshot bases.)

A second standalone target `light_fp_ops_test` covers the round-8f
f32 capability extension (fadd/fsub/fmul/fdiv, ordered fcmp +
branch/select on float result, `ret` of an SSA float or a
`ConstantFP`) AND the round-8g hardening of fcmp deferred-operand
lifetime (a dedicated reproducer asserts the FCMP RHS does not get
clobbered by an intervening FP-constant materialization). The test
includes opcode-mask coverage guards (`FADD-S` / `FSUB-S` / `FMUL-S`
/ `FDIV-S` / `FCMP-S`) that run on any host, plus full
`mmap`+execute correctness on AArch64 hosts. Round 8h extends it to
the full scalar `double` family (D-form binops, `FCMP-D`, `FMOV D,X`
constant materialization, `FCVTZS W,D`); round 8i extends it again
to cover every scalar FP↔int / FP↔FP conversion (`FCVTZS X,S`,
`FCVTZS X,D`, `FCVTZU W,S` / `W,D` / `X,S` / `X,D`, `SCVTF S,W` /
`S,X` / `D,W` / `D,X`, `UCVTF` in all four widths, `FCVT D,S`,
`FCVT S,D`, `FMOV W,S`, `FMOV S,W`, `FMOV X,D`, `FMOV D,X`) with
matching opcode-mask coverage guards and exact bit-pattern execution
checks for all four bitcast directions.

A third standalone target `light_stack_args_test` covers the round-8j
AAPCS64 stack-passed scalar argument support. It builds and emits
seven IR shapes: 9× `i64`, 9× `i32`, eight `i32` plus a 9th `i32*`
that is loaded from, 9× `float`, 9× `double`, a mixed-overflow shape
with eight `i64` + eight `double` followed by alternating
`i64`/`double` overflow (exercising the shared GPR/FP NSAA in
original parameter order), and an `alloca` + 9th `i64` shape that
exercises the interaction between `frameSize` and the
`[sp, frameSize+incomingOff]` preload addressing. On AArch64 hosts
all seven are executed for end-to-end correctness; on other hosts
only the emit path is exercised. Run it directly:

    cmake --build <build-dir> --target check-light-stack-args

It is also pulled in automatically by the top-level `check` target
when the light backend is enabled.

Run it directly:

    cmake --build <build-dir> --target check-light-fp

Like `check-light-endian`, it is pulled in automatically by the
top-level `check` target when the light backend is enabled.

A fourth standalone target `light_dynamic_gep_test` covers the
round-8k two-term dynamic GEP support. It builds and emits six IR
shapes: `int (*)[4]` 2-D load (both shifts ≤ 4 → two single
ADD-extended-register instructions), `int (*)[8]` 2-D load (outer
shift = 5 → 3-instruction extend+LSL+ADD path), `int (*)[4]` 2-D
store + reload round trip, `double (*)[4]` 2-D load (outer shift = 5,
inner shift = 3), a struct field + 2-D array combo
(`struct S { int pad; int arr[4][4]; }; s->arr[i][j]`) that
exercises the constant-offset-plus-two-dynamic-terms folding, and a
9-arg form whose two dynamic indices arrive as round-8j stack-passed
arguments. The test also includes a coverage guard asserting the
small-stride 2-D shape emits ≥ 2 ADD-extended-register
instructions, so a future refactor that silently drops the second
term is caught here. On AArch64 hosts all six are executed for
end-to-end correctness; on other hosts only the emit path is
exercised. Run it directly:

    cmake --build <build-dir> --target check-light-dynamic-gep

It is also pulled in automatically by the top-level `check` target
when the light backend is enabled.

A fifth standalone target `light_gpr_pressure_test` covers the
round-8l saved-scratch extension. It emits a single-block i64 function
with 17 GPR-producing SSA instructions, which exceeded the old
caller-saved-only scratch pool but fits once saved `x19..x28` are
available. On AArch64 hosts it mmap+executes the emitted code and
therefore also validates the save/restore path for the callee-saved
registers. Run it directly:

    cmake --build <build-dir> --target check-light-gpr-pressure

It is also pulled in automatically by the top-level `check` target
when the light backend is enabled.

The C API probe `tests/c_api/be_backend_probe.c` is the board-friendly
runtime smoke used for LE and BE machines. Its default `--case all`
currently covers memory, stack arguments, scalar FP, snapshots, GPR
pressure, combo, stress, and a multi-BB / PHI / select / branch case
(`--case branch`, round 8m). The branch case exercises an `if/else`
with phi-merged i32 result, an `fcmp`-driven FP select with `fneg` on
the negative arm, snapshot-folded sub-word constants, and a writeback
through `*out`. A heavier explicit case is available
as `--case gauntlet`; it mixes pointer-heavy work, FP work, overflow
stack args, dynamic GEPs, and many scalar operations in one kernel. The
gauntlet is intended for optimized builds or board-side spot checks, so
it is not part of default `all`:

    EASYJIT_LIGHT=force ./be_backend_probe --case gauntlet --iters 20 --verbose

As of round 10 the gauntlet compiles and executes correctly under
`EASYJIT_LIGHT=force`. The integer scratch pool (8 caller-saved
`x{argCount}..x15` plus 10 callee-saved `x19..x28`) is no longer the
hard ceiling: when the ~26 simultaneously-live i32/i64 SSA values in
the gauntlet exceed the 18-slot pool, the round-10 minimal GPR
spill/reload mechanism described above kicks in and parks the excess
locals in the dedicated frame area. The mechanism is still narrow —
FP / vector / cross-block / PHI / argument values are not spilled —
so harder kernels can still reject with `scratch OOM (...)` or `frame
> 4095B` if they exceed the spill-slot cap (32 × 8 bytes) or push the
total frame past the uimm12-scaled offset budget.

A sixth optional target `check-light-be-smoke` builds a freestanding
static `aarch64_be` ELF from `light_codegen/test_be_smoke.S` and runs
it under `qemu-aarch64_be` or `qemu-aarch64_be-static` when one is
available. This smoke test does not require an `aarch64_be` libc or
sysroot: the BE ELF uses Linux `exit` directly, and the test routines
inside it are encoded with explicit `Writer::emit`-style little-endian
instruction bytes. It validates the most endian-sensitive runtime
paths under QEMU user mode: `LDR W`, `LDRH`, `STR W`, `LDR S` +
`FMOV W,S`, `LDR D` + `FMOV X,D`, `MOVZ/MOVK` + `FMOV` for f32/f64
bit patterns, and an 8-byte `LDR X` / `STR X` memcpy-like copy. Run it
directly:

    cmake --build <build-dir> --target check-light-be-smoke

If no suitable QEMU binary, clang, or `ld.lld` is found, the target
prints a skip message and succeeds so normal local `check` remains
portable. Passing this smoke is stronger than emit-time parity, but it
is still QEMU user-mode coverage rather than execution on real
`aarch64_be` silicon.
