# EasyJIT Light Backend Limitations

This file tracks the intentional limits of the AArch64 light backend used by
`EASYJIT_LIGHT_BACKEND_ONLY=ON`.

##It is also pulled in automatically by the top-level `check` target when
the light backend is enabled. The test exercises LDR/LDRH/STR, the
12-bit-immediate ADD fast path, a >16-bit positive constant ADD, **and**
an `i32` negative constant ADD (`-12345`, bit pattern `0xFFFFCFC7`).
The latter two both force the binop RHS through `materializeImmAny` →
`emitMovImm`, producing a real `MOVZ Wd,#lo` + `MOVK Wd,#hi,lsl #16`
halfword pair each. The test asserts ≥2 MOVZ + ≥2 MOVK opcode words
in the LE stream and explicitly asserts that no MOVN appears (the
round-8e design choice is bit-pattern materialization, not MOVN), so
any future change that silently bypasses the wide- or negative-
immediate path will be caught here. (MOVZ/MOVK halfwords are also
exercised independently by the absolute-address path used for snapshot
bases.)Behavior

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
- Fixed-size stack frame, constant-offset alloca GEP.
- One dynamic scaled GEP index over an absolute pointer or forwarded pointer.
- `i8/i16/i32/i64` loads and stores.
- `float` loads and stores.
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
- Scalar single-precision ordered comparisons (round 8f), fused into the
  immediately-following conditional branch or `select`. Supported
  predicates: `FCMP_OEQ` (EQ), `FCMP_OGT` (GT), `FCMP_OGE` (GE),
  `FCMP_OLT` (MI — *not* AArch64 LT, which fires on NaN since V==1
  inverts N), `FCMP_OLE` (LS). Unordered predicates and `FCMP_ONE`
  are intentionally rejected — see Known Unsupported Areas.
- `select` with float result type, driven by either an icmp or an
  ordered fcmp (round 8f).
- `ret float` of an SSA float value already materialized in an
  FP register (round 8f). The result is moved to `s0` via
  `FMOV s0, sN` if not already there.
- `fptosi float -> i32`.
- `sext/zext/trunc` over integer widths used by current tests.

## Known Unsupported Areas

- `double` / `f64` arithmetic and conversion.
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
- `ret` of a `ConstantFP` directly (no SSA producer). Frontends almost
  always materialize the constant first; supporting this would require
  reordering helpers in `light_aarch64.cpp` (lambda-ordering issue).
- An FP binop or FCmp where *both* operands are unmaterialized
  `ConstantFP` (`s31` scratch clobber).
- Stack-passed arguments beyond the first eight integer/pointer or float
  argument registers.
- Register spilling under high pressure.
- Multiple independent dynamic indexes in one GEP chain.
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

