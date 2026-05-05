// test_be_smoke.S - qemu-user aarch64_be smoke for the light emitter's
// endian-sensitive instruction/data paths.
//
// This file is intentionally freestanding: it links with -nostdlib and uses
// the Linux exit syscall directly, so it does not need an aarch64_be libc or
// sysroot. The small code_* routines below are encoded as explicit bytes in
// the same little-endian instruction stream format produced by Writer::emit.
// The surrounding BE ELF verifies those instruction bytes execute correctly
// under qemu-aarch64_be and observe big-endian data semantics.

.section .text
.global _start
_start:
  // i32 LDR from BE data: expect 0x11223344.
  adr x0, word_data
  bl code_load_i32
  adr x2, expect_word
  ldr w1, [x2]
  cmp w0, w1
  b.ne fail_1

  // i16 LDRH from BE data: expect 0x1234.
  adr x0, half_data
  bl code_load_u16
  movz w1, #0x1234
  cmp w0, w1
  b.ne fail_2

  // STR W to BE data, then normal BE load verifies the numeric value.
  adr x0, store_word
  movz w1, #0x7788
  movk w1, #0x5566, lsl #16
  bl code_store_i32
  adr x2, store_word
  ldr w3, [x2]
  cmp w3, w1
  b.ne fail_3

  // LDR S + FMOV W,S from BE float memory: expect 1.5f bits.
  adr x0, f32_data
  bl code_load_f32_bits
  adr x2, expect_f32
  ldr w1, [x2]
  cmp w0, w1
  b.ne fail_4

  // LDR D + FMOV X,D from BE double memory: expect 1.5 bits.
  adr x0, f64_data
  bl code_load_f64_bits
  adr x2, expect_f64
  ldr x1, [x2]
  cmp x0, x1
  b.ne fail_5

  // MOVZ/MOVK + FMOV S,W constant path returns 1.5f bits.
  bl code_const_f32_bits
  adr x2, expect_f32
  ldr w1, [x2]
  cmp w0, w1
  b.ne fail_6

  // MOVZ/MOVK + FMOV D,X constant path returns 1.5 double bits.
  bl code_const_f64_bits
  adr x2, expect_f64
  ldr x1, [x2]
  cmp x0, x1
  b.ne fail_7

  // memcpy-like LDR X / STR X preserves 8 BE bytes.
  adr x0, copy_src
  adr x1, copy_dst
  bl code_copy8
  adr x2, copy_src
  ldr x3, [x2]
  adr x2, copy_dst
  ldr x4, [x2]
  cmp x3, x4
  b.ne fail_8

  mov x0, #0
  b exit

fail_1: mov x0, #1; b exit
fail_2: mov x0, #2; b exit
fail_3: mov x0, #3; b exit
fail_4: mov x0, #4; b exit
fail_5: mov x0, #5; b exit
fail_6: mov x0, #6; b exit
fail_7: mov x0, #7; b exit
fail_8: mov x0, #8; b exit

exit:
  mov x8, #93
  svc #0

.align 2
code_load_i32:
  .byte 0x00,0x00,0x40,0xb9  // ldr w0, [x0]
  .byte 0xc0,0x03,0x5f,0xd6  // ret

.align 2
code_load_u16:
  .byte 0x00,0x00,0x40,0x79  // ldrh w0, [x0]
  .byte 0xc0,0x03,0x5f,0xd6

.align 2
code_store_i32:
  .byte 0x01,0x00,0x00,0xb9  // str w1, [x0]
  .byte 0xc0,0x03,0x5f,0xd6

.align 2
code_load_f32_bits:
  .byte 0x00,0x00,0x40,0xbd  // ldr s0, [x0]
  .byte 0x00,0x00,0x26,0x1e  // fmov w0, s0
  .byte 0xc0,0x03,0x5f,0xd6

.align 2
code_load_f64_bits:
  .byte 0x00,0x00,0x40,0xfd  // ldr d0, [x0]
  .byte 0x00,0x00,0x66,0x9e  // fmov x0, d0
  .byte 0xc0,0x03,0x5f,0xd6

.align 2
code_const_f32_bits:
  .byte 0x10,0x00,0x80,0x52  // movz w16, #0
  .byte 0x10,0xf8,0xa7,0x72  // movk w16, #0x3fc0, lsl #16
  .byte 0x00,0x02,0x27,0x1e  // fmov s0, w16
  .byte 0x00,0x00,0x26,0x1e  // fmov w0, s0
  .byte 0xc0,0x03,0x5f,0xd6

.align 2
code_const_f64_bits:
  .byte 0x10,0x00,0x80,0xd2  // movz x16, #0
  .byte 0x10,0xff,0xe7,0xf2  // movk x16, #0x3ff8, lsl #48
  .byte 0x00,0x02,0x67,0x9e  // fmov d0, x16
  .byte 0x00,0x00,0x66,0x9e  // fmov x0, d0
  .byte 0xc0,0x03,0x5f,0xd6

.align 2
code_copy8:
  .byte 0x02,0x00,0x40,0xf9  // ldr x2, [x0]
  .byte 0x22,0x00,0x00,0xf9  // str x2, [x1]
  .byte 0xc0,0x03,0x5f,0xd6

.section .data
.align 3
word_data:   .word 0x11223344
expect_word: .word 0x11223344
half_data:   .hword 0x1234
.align 2
store_word:  .word 0x00000000
f32_data:    .word 0x3fc00000
expect_f32:  .word 0x3fc00000
.align 3
f64_data:    .quad 0x3ff8000000000000
expect_f64:  .quad 0x3ff8000000000000
copy_src:    .quad 0x0102030405060708
copy_dst:    .quad 0x0000000000000000
