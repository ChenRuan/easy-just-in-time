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

