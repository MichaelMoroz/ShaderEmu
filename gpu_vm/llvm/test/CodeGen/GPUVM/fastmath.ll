; RUN: llc -mtriple=gpuvm < %s | FileCheck %s
; The card's special functions and multiply-adds (gpu_vm/llvm/README.md, "float"): fmad, fmsb and fnmsb for an fmul
; into an fadd or fsub whatever the contraction flags (the ops round the product, as separate ops do); under afn
; (-ffast-math, -fapprox-func) sin cos tan exp exp2 exp10 log log2 log10 pow sincos on fsin fcos fexp2 flog2, 1/x
; and 1/sqrt x on frcp and frsqrt (or arcp); without afn libm (a sin and a cos of one value: sincosf); round, lround
; and lrint inline; three divisions by one value under arcp by one frcp; f32 to i64 inline.

define float @mad_no_contract(float %a, float %b, float %c) {
; CHECK-LABEL: mad_no_contract:
; CHECK: fmad a0, a0, a1, a2
; CHECK: ret
  %m = fmul float %a, %b
  %s = fadd float %m, %c
  ret float %s
}

define float @nmsb(float %a, float %b, float %c) {
; CHECK-LABEL: nmsb:
; CHECK: fnmsb a0, a0, a1, a2
; CHECK: ret
  %m = fmul float %a, %b
  %s = fsub float %c, %m
  ret float %s
}

define float @fmuladd(float %a, float %b, float %c) {
; CHECK-LABEL: fmuladd:
; CHECK: fmad a0, a0, a1, a2
; CHECK: ret
  %r = call float @llvm.fmuladd.f32(float %a, float %b, float %c)
  ret float %r
}

define float @dot3(float %a0, float %a1, float %a2, float %b0, float %b1, float %b2) {
; CHECK-LABEL: dot3:
; CHECK: fmul t0, a1, a4
; CHECK: fmad t0, a0, a3, t0
; CHECK: fmad a0, a2, a5, t0
; CHECK: ret
  %m0 = fmul float %a0, %b0
  %m1 = fmul float %a1, %b1
  %m2 = fmul float %a2, %b2
  %s1 = fadd float %m0, %m1
  %s2 = fadd float %s1, %m2
  ret float %s2
}

define float @sin_afn(float %x) {
; CHECK-LABEL: sin_afn:
; CHECK: fsin a0, a0
; CHECK: ret
  %r = call afn float @llvm.sin.f32(float %x)
  ret float %r
}

define float @sin_libm(float %x) {
; CHECK-LABEL: sin_libm:
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: call sinf
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: ret
  %r = call float @llvm.sin.f32(float %x)
  ret float %r
}

define float @sincos_libm(float %x) {
; CHECK-LABEL: sincos_libm:
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: addi a1, sp, 8
; CHECK: addi a2, sp, 4
; CHECK: call sincosf
; CHECK: lw t0, 4(sp)
; CHECK: lw t1, 8(sp)
; CHECK: lw ra, 12(sp)
; CHECK: fadd a0, t1, t0
; CHECK: addi sp, sp, 16
; CHECK: ret
  %s = call float @llvm.sin.f32(float %x)
  %c = call float @llvm.cos.f32(float %x)
  %r = fadd float %s, %c
  ret float %r
}

define float @sincos_afn(float %x) {
; CHECK-LABEL: sincos_afn:
; CHECK: fcos t0, a0
; CHECK: fsin t1, a0
; CHECK: fadd a0, t1, t0
; CHECK: ret
  %sc = call afn { float, float } @llvm.sincos.f32(float %x)
  %s = extractvalue { float, float } %sc, 0
  %c = extractvalue { float, float } %sc, 1
  %r = fadd float %s, %c
  ret float %r
}

define float @tan_afn(float %x) {
; CHECK-LABEL: tan_afn:
; CHECK: fcos t0, a0
; CHECK: fsin t1, a0
; CHECK: fdiv a0, t1, t0
; CHECK: ret
  %r = call afn float @llvm.tan.f32(float %x)
  ret float %r
}

define float @exp_afn(float %x) {
; CHECK-LABEL: exp_afn:
; CHECK: fmuli t0, a0, 1.44269502
; CHECK: fexp2 a0, t0
; CHECK: ret
  %r = call afn float @llvm.exp.f32(float %x)
  ret float %r
}

define float @log10_afn(float %x) {
; CHECK-LABEL: log10_afn:
; CHECK: flog2 t0, a0
; CHECK: fmuli a0, t0, 0.30103001
; CHECK: ret
  %r = call afn float @llvm.log10.f32(float %x)
  ret float %r
}

define float @pow_afn(float %x, float %y) {
; CHECK-LABEL: pow_afn:
; CHECK: ftrunc t0, a1
; CHECK: feq t1, t0, a1
; CHECK: ftoi t0, t0
; CHECK: and t0, t0, t1
; CHECK: fabs t1, a0
; CHECK: slli t0, t0, 31
; CHECK: flog2 t1, t1
; CHECK: fmul t1, a1, t1
; CHECK: fexp2 t1, t1
; CHECK: and t0, a0, t0
; CHECK: xor a0, t1, t0
; CHECK: ret
  %r = call afn float @llvm.pow.f32(float %x, float %y)
  ret float %r
}

define float @pow_afn_pos(float %x, float %y) {
; CHECK-LABEL: pow_afn_pos:
; CHECK: fabs t0, a0
; CHECK: flog2 t0, t0
; CHECK: fmul t0, a1, t0
; CHECK: fexp2 a0, t0
; CHECK: ret
  %a = call float @llvm.fabs.f32(float %x)
  %r = call afn float @llvm.pow.f32(float %a, float %y)
  ret float %r
}

define float @pow_libm(float %x, float %y) {
; CHECK-LABEL: pow_libm:
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: call powf
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: ret
  %r = call float @llvm.pow.f32(float %x, float %y)
  ret float %r
}

define float @rsqrt_afn(float %x) {
; CHECK-LABEL: rsqrt_afn:
; CHECK: frsqrt a0, a0
; CHECK: ret
  %s = call afn float @llvm.sqrt.f32(float %x)
  %r = fdiv afn float 1.0, %s
  ret float %r
}

define float @rcp_arcp(float %x) {
; CHECK-LABEL: rcp_arcp:
; CHECK: frcp a0, a0
; CHECK: ret
  %r = fdiv arcp float 1.0, %x
  ret float %r
}

define float @round(float %x) {
; CHECK-LABEL: round:
; CHECK: li t0, 1056964607
; CHECK: fsgnj t0, t0, a0
; CHECK: fadd t0, a0, t0
; CHECK: ftrunc a0, t0
; CHECK: ret
  %r = call float @llvm.round.f32(float %x)
  ret float %r
}

define i32 @lround(float %x) {
; CHECK-LABEL: lround:
; CHECK: li t0, 1056964607
; CHECK: fsgnj t0, t0, a0
; CHECK: fadd t0, a0, t0
; CHECK: ftrunc t0, t0
; CHECK: ftoi a0, t0
; CHECK: ret
  %r = call i32 @llvm.lround.i32.f32(float %x)
  ret i32 %r
}

define i32 @lrint(float %x) {
; CHECK-LABEL: lrint:
; CHECK: fround t0, a0
; CHECK: ftoi a0, t0
; CHECK: ret
  %r = call i32 @llvm.lrint.i32.f32(float %x)
  ret i32 %r
}

define void @div3_arcp(float %a, float %b, float %c, float %d, ptr %p) {
; CHECK-LABEL: div3_arcp:
; CHECK: frcp t0, a3
; CHECK: fmul t1, a0, t0
; CHECK: fmul t2, a1, t0
; CHECK: fmul t0, a2, t0
; CHECK: sw t1, 0(a4)
; CHECK: sw t2, 4(a4)
; CHECK: sw t0, 8(a4)
; CHECK: ret
  %x = fdiv arcp float %a, %d
  %y = fdiv arcp float %b, %d
  %z = fdiv arcp float %c, %d
  store float %x, ptr %p
  %p1 = getelementptr float, ptr %p, i32 1
  store float %y, ptr %p1
  %p2 = getelementptr float, ptr %p, i32 2
  store float %z, ptr %p2
  ret void
}

define void @div2_arcp(float %a, float %b, float %d, ptr %p) {
; CHECK-LABEL: div2_arcp:
; CHECK: fdiv t0, a0, a2
; CHECK: fdiv t1, a1, a2
; CHECK: sw t0, 0(a3)
; CHECK: sw t1, 4(a3)
; CHECK: ret
  %x = fdiv arcp float %a, %d
  %y = fdiv arcp float %b, %d
  store float %x, ptr %p
  %p1 = getelementptr float, ptr %p, i32 1
  store float %y, ptr %p1
  ret void
}

define i64 @f2i64(float %x) {
; CHECK-LABEL: f2i64:
; CHECK: ftrunc t0, a0
; CHECK: fmuli t1, t0, 2.32830644e-10
; CHECK: ffloor t1, t1
; CHECK: li t2, -813694976
; CHECK: fmad t2, t1, t2, t0
; CHECK: li t4, 1325400064
; CHECK: fabs t3, t0
; CHECK: ftou t2, t2
; CHECK: flt t3, t3, t4
; CHECK: ftoi t0, t0
; CHECK: sel a0, t0, t2, t3
; CHECK: srai t0, t0, 31
; CHECK: ftoi t1, t1
; CHECK: sel a1, t0, t1, t3
; CHECK: ret
  %r = fptosi float %x to i64
  ret i64 %r
}

define i64 @f2u64(float %x) {
; CHECK-LABEL: f2u64:
; CHECK: ftrunc t0, a0
; CHECK: fmuli t1, t0, 2.32830644e-10
; CHECK: ffloor t1, t1
; CHECK: li t2, -813694976
; CHECK: ftou a1, t1
; CHECK: fmad t0, t1, t2, t0
; CHECK: ftou a0, t0
; CHECK: ret
  %r = fptoui float %x to i64
  ret i64 %r
}

define i64 @d2i64(double %x) {
; CHECK-LABEL: d2i64:
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: call __fixdfdi
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: ret
  %r = fptosi double %x to i64
  ret i64 %r
}

define double @fpext(float %x) {
; CHECK-LABEL: fpext:
; CHECK: andi t1, a0, 2147483647
; CHECK: slli t0, a0, 29
; CHECK: sgeui t3, t1, 2139095040
; CHECK: sltui t2, t1, 8388608
; CHECK: srli t1, t1, 3
; CHECK: sel t0, zero, t0, t2
; CHECK: li t4, 939524096
; CHECK: li t5, 1879048192
; CHECK: sel t3, t5, t4, t3
; CHECK: add t1, t1, t3
; CHECK: sel t1, zero, t1, t2
; CHECK: andi t2, a0, -2147483648
; CHECK: or a1, t1, t2
; CHECK: mv a0, t0
; CHECK: ret
  %r = fpext float %x to double
  ret double %r
}

define float @fptrunc(double %x) {
; CHECK-LABEL: fptrunc:
; CHECK: srli t0, a0, 29
; CHECK: slli t1, a1, 3
; CHECK: or t0, t1, t0
; CHECK: addi t0, t0, 1073741824
; CHECK: andi t1, t0, 1
; CHECK: andi t2, a0, 536870911
; CHECK: add t1, t2, t1
; CHECK: andi t2, a1, 2147483647
; CHECK: sgeui t1, t1, 268435457
; CHECK: add t0, t0, t1
; CHECK: sltu t1, zero, a0
; CHECK: or t1, t2, t1
; CHECK: sgeui t1, t1, 2146435073
; CHECK: li t3, 2139095040
; CHECK: li t4, 2143289344
; CHECK: sel t1, t4, t3, t1
; CHECK: sgeui t3, t2, 1206910976
; CHECK: sel t0, t1, t0, t3
; CHECK: sltui t1, t2, 940572672
; CHECK: sel t0, zero, t0, t1
; CHECK: andi t1, a1, -2147483648
; CHECK: or a0, t0, t1
; CHECK: ret
  %r = fptrunc double %x to float
  ret float %r
}
