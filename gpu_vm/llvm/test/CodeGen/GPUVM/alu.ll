; RUN: llc -mtriple=gpuvm < %s | FileCheck %s
; Integer ops: R and I forms (any 32-bit immediate is one op), the extended
; ops, and the patterns over them (andn, bfe, bfes, shadd, abs, min/max).

define i32 @add(i32 %a, i32 %b) {
; CHECK-LABEL: add:
; CHECK: add a0, a0, a1
; CHECK: ret
  %r = add i32 %a, %b
  ret i32 %r
}

define i32 @addi_short(i32 %a) {
; CHECK-LABEL: addi_short:
; CHECK: addi a0, a0, 2047
; CHECK: ret
  %r = add i32 %a, 2047
  ret i32 %r
}

define i32 @addi_long(i32 %a) {
; CHECK-LABEL: addi_long:
; CHECK: addi a0, a0, 123456789
; CHECK: ret
  %r = add i32 %a, 123456789
  ret i32 %r
}

define i32 @sub_imm_reg(i32 %a) {
; CHECK-LABEL: sub_imm_reg:
; CHECK: li t0, 100
; CHECK: sub a0, t0, a0
; CHECK: ret
  %r = sub i32 100, %a
  ret i32 %r
}

define i32 @neg(i32 %a) {
; CHECK-LABEL: neg:
; CHECK: neg a0, a0
; CHECK: ret
  %r = sub i32 0, %a
  ret i32 %r
}

define i32 @andn(i32 %a, i32 %b) {
; CHECK-LABEL: andn:
; CHECK: andn a0, a0, a1
; CHECK: ret
  %n = xor i32 %b, -1
  %r = and i32 %a, %n
  ret i32 %r
}

define i32 @orn(i32 %a, i32 %b) {
; CHECK-LABEL: orn:
; CHECK: orn a0, a0, a1
; CHECK: ret
  %n = xor i32 %b, -1
  %r = or i32 %a, %n
  ret i32 %r
}

define i32 @xnor(i32 %a, i32 %b) {
; CHECK-LABEL: xnor:
; CHECK: xnor a0, a0, a1
; CHECK: ret
  %x = xor i32 %a, %b
  %r = xor i32 %x, -1
  ret i32 %r
}

define i32 @zext16(i32 %a) {
; CHECK-LABEL: zext16:
; CHECK: bfe a0, a0, 0, 16
; CHECK: ret
  %r = and i32 %a, 65535
  ret i32 %r
}

define i32 @field(i32 %a) {
; CHECK-LABEL: field:
; CHECK: bfe a0, a0, 7, 13
; CHECK: ret
  %s = lshr i32 %a, 7
  %r = and i32 %s, 8191
  ret i32 %r
}

define i32 @sext8(i32 %a) {
; CHECK-LABEL: sext8:
; CHECK: bfes a0, a0, 0, 8
; CHECK: ret
  %t = trunc i32 %a to i8
  %r = sext i8 %t to i32
  ret i32 %r
}

define i32 @sext16(i16 %a) {
; CHECK-LABEL: sext16:
; CHECK: bfes a0, a0, 0, 16
; CHECK: ret
  %r = sext i16 %a to i32
  ret i32 %r
}

define i32 @shadd(i32 %a, i32 %b) {
; CHECK-LABEL: shadd:
; CHECK: shadd a0, a0, a1, 3
; CHECK: ret
  %s = shl i32 %b, 3
  %r = add i32 %a, %s
  ret i32 %r
}

define i32 @shifts(i32 %a, i32 %b) {
; CHECK-LABEL: shifts:
; CHECK: sll t0, a0, a1
; CHECK: srli t0, t0, 3
; CHECK: srl a0, t0, a1
; CHECK: ret
  %x = shl i32 %a, %b
  %y = lshr i32 %x, 3
  %z = ashr i32 %y, %b
  ret i32 %z
}

define i32 @rot(i32 %a, i32 %b) {
; CHECK-LABEL: rot:
; CHECK: rol t0, a0, a1
; CHECK: rori a0, t0, 5
; CHECK: ret
  %x = call i32 @llvm.fshl.i32(i32 %a, i32 %a, i32 %b)
  %y = call i32 @llvm.fshr.i32(i32 %x, i32 %x, i32 5)
  ret i32 %y
}

define i32 @minmax(i32 %a, i32 %b, i32 %c) {
; CHECK-LABEL: minmax:
; CHECK: min t0, a0, a1
; CHECK: maxu t0, t0, a2
; CHECK: maxi a0, t0, -5
; CHECK: ret
  %x = call i32 @llvm.smin.i32(i32 %a, i32 %b)
  %y = call i32 @llvm.umax.i32(i32 %x, i32 %c)
  %z = call i32 @llvm.smax.i32(i32 %y, i32 -5)
  ret i32 %z
}

define i32 @iabs(i32 %a) {
; CHECK-LABEL: iabs:
; CHECK: neg t0, a0
; CHECK: max a0, a0, t0
; CHECK: ret
  %r = call i32 @llvm.abs.i32(i32 %a, i1 false)
  ret i32 %r
}

define i32 @muldiv(i32 %a, i32 %b) {
; CHECK-LABEL: muldiv:
; CHECK: mul t0, a0, a1
; CHECK: div t0, t0, a1
; CHECK: divu t0, t0, a0
; CHECK: rem t0, t0, a1
; CHECK: remu a0, t0, a0
; CHECK: ret
  %m = mul i32 %a, %b
  %d = sdiv i32 %m, %b
  %u = udiv i32 %d, %a
  %r = srem i32 %u, %b
  %q = urem i32 %r, %a
  ret i32 %q
}

define i32 @mulh(i32 %a, i32 %b) {
; CHECK-LABEL: mulh:
; CHECK: mulh a0, a0, a1
; CHECK: ret
  %x = sext i32 %a to i64
  %y = sext i32 %b to i64
  %m = mul i64 %x, %y
  %h = lshr i64 %m, 32
  %r = trunc i64 %h to i32
  ret i32 %r
}

define i32 @bitops(i32 %a) {
; CHECK-LABEL: bitops:
; CHECK: clz t0, a0
; CHECK: ctz t0, t0
; CHECK: popc t0, t0
; CHECK: slli t0, t0, 24
; CHECK: brev a0, t0
; CHECK: ret
  %c = call i32 @llvm.ctlz.i32(i32 %a, i1 false)
  %t = call i32 @llvm.cttz.i32(i32 %c, i1 false)
  %p = call i32 @llvm.ctpop.i32(i32 %t)
  %s = call i32 @llvm.bswap.i32(i32 %p)
  %r = call i32 @llvm.bitreverse.i32(i32 %s)
  ret i32 %r
}

define i32 @constant() {
; CHECK-LABEL: constant:
; CHECK: li a0, -559038737
; CHECK: ret
  ret i32 -559038737
}

declare i32 @llvm.fshl.i32(i32, i32, i32)
declare i32 @llvm.fshr.i32(i32, i32, i32)
declare i32 @llvm.smin.i32(i32, i32)
declare i32 @llvm.smax.i32(i32, i32)
declare i32 @llvm.umax.i32(i32, i32)
declare i32 @llvm.abs.i32(i32, i1)
declare i32 @llvm.ctlz.i32(i32, i1)
declare i32 @llvm.cttz.i32(i32, i1)
declare i32 @llvm.ctpop.i32(i32)
declare i32 @llvm.bswap.i32(i32)
declare i32 @llvm.bitreverse.i32(i32)
