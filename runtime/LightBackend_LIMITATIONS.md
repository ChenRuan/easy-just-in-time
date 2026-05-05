# EasyJIT Light Backend Limitations

This file tracks the intentional limits of the AArch64 light backend used by
`EASYJIT_LIGHT_BACKEND_ONLY=ON`.

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
- Fixed-size stack frame, constant-offset alloca GEP.
- One dynamic scaled GEP index over an absolute pointer or forwarded pointer.
- `i8/i16/i32/i64` loads and stores.
- `float` loads and stores.
- Integer arithmetic: `add/sub/mul/and/or/xor/shl/lshr/ashr`.
- Integer comparisons when fused into a following conditional branch.
- Integer and float phi copies.
- `llvm.memcpy` with constant size up to 32 bytes.
- `llvm.fmuladd.f32`.
- `fptosi float -> i32`.
- `sext/zext/trunc` over integer widths used by current tests.

## Known Unsupported Areas

- `double` / `f64` arithmetic and conversion.
- Vector IR.
- Standalone floating-point `fadd`, `fsub`, `fmul`, `fdiv` when optimization
  does not lower the pattern to `llvm.fmuladd.f32`.
- Floating-point comparisons and selects.
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
the light backend is enabled. The test exercises LDR/LDRH/STR and the
12-bit-immediate ADD path; it does not currently cover the >16-bit
MOVZ+MOVK integer-constant materialization (the binop RHS materializer
caps at 16 bits today — see `materializeImm16` in `light_aarch64.cpp`),
though MOVZ/MOVK halfwords are still exercised by the absolute-address
path used for snapshot bases.

