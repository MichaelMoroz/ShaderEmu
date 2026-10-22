; RUN: llc -mtriple=gpuvm < %s | FileCheck %s
; float in the integer registers on the card's units (I forms take the
; constant's bits), fmad for an fmul and an fadd (each rounded, as fmad is),
; compares (the unordered ones by inversion), conversions; double by
; compiler-rt; fma and the transcendental functions by libm.

define float @arith(float %a, float %b) {
; CHECK-LABEL: arith:
; CHECK: fadd t0, a0, a1
; CHECK: li t1, 1075838976
; CHECK: fmsb t0, t0, t1, a0
; CHECK: fdiv a0, t0, a1
; CHECK: ret
  %x = fadd float %a, %b
  %y = fmul float %x, 2.5
  %z = fsub float %y, %a
  %w = fdiv float %z, %b
  ret float %w
}

define float @mad(float %a, float %b, float %c) {
; CHECK-LABEL: mad:
; CHECK: fmad a0, a0, a1, a2
; CHECK: ret
  %m = fmul float %a, %b
  %s = fadd float %m, %c
  ret float %s
}

define float @msb(float %a, float %b, float %c) {
; CHECK-LABEL: msb:
; CHECK: fmsb a0, a0, a1, a2
; CHECK: ret
  %m = fmul float %a, %b
  %s = fsub float %m, %c
  ret float %s
}

define float @fma(float %a, float %b, float %c) {
; CHECK-LABEL: fma:
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: call fmaf
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: ret
  %r = call float @llvm.fma.f32(float %a, float %b, float %c)
  ret float %r
}

define float @unary(float %a, float %b) {
; CHECK-LABEL: unary:
; CHECK: fabs t0, a0
; CHECK: fsqrt t0, t0
; CHECK: ffloor t0, t0
; CHECK: fsgnj t0, t0, a1
; CHECK: fmin a0, t0, a1
; CHECK: ret
  %n = fneg float %a
  %x = call float @llvm.fabs.f32(float %n)
  %y = call float @llvm.sqrt.f32(float %x)
  %z = call float @llvm.floor.f32(float %y)
  %w = call float @llvm.copysign.f32(float %z, float %b)
  %v = call float @llvm.minnum.f32(float %w, float %b)
  ret float %v
}

define i32 @cmp_olt(float %a, float %b) {
; CHECK-LABEL: cmp_olt:
; CHECK: flt a0, a0, a1
; CHECK: ret
  %c = fcmp olt float %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}

define i32 @cmp_une(float %a, float %b) {
; CHECK-LABEL: cmp_une:
; CHECK: feq t0, a0, a1
; CHECK: xori a0, t0, 1
; CHECK: ret
  %c = fcmp une float %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}

define i32 @cmp_uno(float %a, float %b) {
; CHECK-LABEL: cmp_uno:
; CHECK: funord a0, a0, a1
; CHECK: ret
  %c = fcmp uno float %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}

define i32 @conv(float %a, i32 %b) {
; CHECK-LABEL: conv:
; CHECK: itof t2, a1
; CHECK: utof t3, a1
; CHECK: fadd t2, t2, t3
; CHECK: ftoi t0, a0
; CHECK: ftou t1, a0
; CHECK: add t0, t0, t1
; CHECK: ftoi t2, t2
; CHECK: add a0, t0, t2
; CHECK: ret
  %i = fptosi float %a to i32
  %u = fptoui float %a to i32
  %f = sitofp i32 %b to float
  %g = uitofp i32 %b to float
  %s = fadd float %f, %g
  %j = fptosi float %s to i32
  %t = add i32 %i, %u
  %r = add i32 %t, %j
  ret i32 %r
}

define double @dbl(double %a, double %b) {
; CHECK-LABEL: dbl:
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: sw s1, 8(sp)
; CHECK: sw s2, 4(sp)
; CHECK: mv s1, a3
; CHECK: mv s2, a2
; CHECK: call __adddf3
; CHECK: mv a2, s2
; CHECK: mv a3, s1
; CHECK: call __muldf3
; CHECK: lw ra, 12(sp)
; CHECK: lw s1, 8(sp)
; CHECK: lw s2, 4(sp)
; CHECK: addi sp, sp, 16
; CHECK: ret
  %x = fadd double %a, %b
  %y = fmul double %x, %b
  ret double %y
}

define float @trunc(double %a) {
; CHECK-LABEL: trunc:
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: call __truncdfsf2
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: ret
  %r = fptrunc double %a to float
  ret float %r
}

define float @constants() {
; CHECK-LABEL: constants:
; CHECK: li a0, 1343554297
; CHECK: ret
  ret float 1.0e10
}

declare float @llvm.fma.f32(float, float, float)
declare float @llvm.fabs.f32(float)
declare float @llvm.sqrt.f32(float)
declare float @llvm.floor.f32(float)
declare float @llvm.copysign.f32(float, float)
declare float @llvm.minnum.f32(float, float)
