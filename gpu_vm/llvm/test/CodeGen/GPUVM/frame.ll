; RUN: llc -mtriple=gpuvm < %s | FileCheck %s
; Frames: sp moves once (any size), the callee-saved registers under the
; caller's sp, fp for allocas, a realigned sp, bp with both; TLS (tp +
; %tprel); i64 sums by addcy/carry; atomics.

declare void @use(ptr)

define void @small() {
; CHECK-LABEL: small:
; CHECK: addi sp, sp, -32
; CHECK: sw ra, 28(sp)
; CHECK: addi a0, sp, 12
; CHECK: call use
; CHECK: lw ra, 28(sp)
; CHECK: addi sp, sp, 32
; CHECK: ret
  %a = alloca [4 x i32]
  call void @use(ptr %a)
  ret void
}

define void @large() {
; CHECK-LABEL: large:
; CHECK: addi sp, sp, -400016
; CHECK: sw ra, 400012(sp)
; CHECK: addi a0, sp, 12
; CHECK: call use
; CHECK: lw ra, 400012(sp)
; CHECK: addi sp, sp, 400016
; CHECK: ret
  %a = alloca [100000 x i32]
  call void @use(ptr %a)
  ret void
}

define void @dynamic(i32 %n) {
; CHECK-LABEL: dynamic:
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: li t0, 15
; CHECK: sw s0, 8(sp)
; CHECK: addi s0, sp, 16
; CHECK: shadd t0, t0, a0, 2
; CHECK: andi t0, t0, -16
; CHECK: sub a0, sp, t0
; CHECK: mv sp, a0
; CHECK: call use
; CHECK: addi sp, s0, -16
; CHECK: lw ra, 12(sp)
; CHECK: lw s0, 8(sp)
; CHECK: addi sp, sp, 16
; CHECK: ret
  %a = alloca i32, i32 %n
  call void @use(ptr %a)
  ret void
}

define void @realigned() {
; CHECK-LABEL: realigned:
; CHECK: addi sp, sp, -64
; CHECK: sw ra, 60(sp)
; CHECK: sw s0, 56(sp)
; CHECK: addi s0, sp, 64
; CHECK: andi sp, sp, -64
; CHECK: mv a0, sp
; CHECK: call use
; CHECK: addi sp, s0, -64
; CHECK: lw ra, 60(sp)
; CHECK: lw s0, 56(sp)
; CHECK: addi sp, sp, 64
; CHECK: ret
  %a = alloca i32, align 64
  call void @use(ptr %a)
  ret void
}

define void @realigned_dynamic(i32 %n) {
; CHECK-LABEL: realigned_dynamic:
; CHECK: addi sp, sp, -64
; CHECK: sw ra, 60(sp)
; CHECK: sw s0, 56(sp)
; CHECK: sw s1, 52(sp)
; CHECK: sw s2, 48(sp)
; CHECK: addi s0, sp, 64
; CHECK: andi sp, sp, -64
; CHECK: li t0, 15
; CHECK: mv s1, sp
; CHECK: shadd t0, t0, a0, 2
; CHECK: andi t0, t0, -16
; CHECK: sub s2, sp, t0
; CHECK: mv sp, s2
; CHECK: mv a0, s1
; CHECK: call use
; CHECK: mv a0, s2
; CHECK: call use
; CHECK: addi sp, s0, -64
; CHECK: lw ra, 60(sp)
; CHECK: lw s0, 56(sp)
; CHECK: lw s1, 52(sp)
; CHECK: lw s2, 48(sp)
; CHECK: addi sp, sp, 64
; CHECK: ret
  %a = alloca i32, align 64
  %b = alloca i32, i32 %n
  call void @use(ptr %a)
  call void @use(ptr %b)
  ret void
}

@t = thread_local global i32 0

define i32 @tls(i32 %v) {
; CHECK-LABEL: tls:
; CHECK: lw t0, %tprel(t)(tp)
; CHECK: sw a0, %tprel(t)(tp)
; CHECK: mv a0, t0
; CHECK: ret
  %x = load i32, ptr @t
  store i32 %v, ptr @t
  ret i32 %x
}

define ptr @tls_addr() {
; CHECK-LABEL: tls_addr:
; CHECK: addi a0, tp, %tprel(t)
; CHECK: ret
  ret ptr @t
}

define i64 @add64(i64 %a, i64 %b) {
; CHECK-LABEL: add64:
; CHECK: carry t0, a0, a2, zero
; CHECK: add a0, a0, a2
; CHECK: addcy a1, a1, a3, t0
; CHECK: ret
  %r = add i64 %a, %b
  ret i64 %r
}

define i64 @sub64(i64 %a, i64 %b) {
; CHECK-LABEL: sub64:
; CHECK: borrow t0, a0, a2, zero
; CHECK: sub a0, a0, a2
; CHECK: subby a1, a1, a3, t0
; CHECK: ret
  %r = sub i64 %a, %b
  ret i64 %r
}

define i64 @mul64(i64 %a, i64 %b) {
; CHECK-LABEL: mul64:
; CHECK: mul t0, a0, a3
; CHECK: mulhu t1, a0, a2
; CHECK: add t0, t1, t0
; CHECK: mul t1, a1, a2
; CHECK: add a1, t0, t1
; CHECK: mul a0, a0, a2
; CHECK: ret
  %r = mul i64 %a, %b
  ret i64 %r
}

define i64 @div64(i64 %a, i64 %b) {
; CHECK-LABEL: div64:
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: call __divdi3
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: ret
  %r = sdiv i64 %a, %b
  ret i64 %r
}

define i32 @atomics(ptr %p, i32 %v, i32 %e) {
; CHECK-LABEL: atomics:
; CHECK: amoadd t0, a1, (a0)
; CHECK: amoswap t0, t0, (a0)
; CHECK: amomaxu t0, t0, (a0)
; CHECK: amocas a2, t0, (a0)
; CHECK: mv a0, a2
; CHECK: fence
; CHECK: ret
  %a = atomicrmw add ptr %p, i32 %v seq_cst
  %b = atomicrmw xchg ptr %p, i32 %a seq_cst
  %c = atomicrmw umax ptr %p, i32 %b seq_cst
  %d = cmpxchg ptr %p, i32 %e, i32 %c seq_cst seq_cst
  %r = extractvalue { i32, i1 } %d, 0
  fence seq_cst
  ret i32 %r
}

define i32 @inline_asm(i32 %a) {
; CHECK-LABEL: inline_asm:
; CHECK: addi t0, a0, 3
; CHECK: mfsr t1, cycle
; CHECK: add a0, t0, t1
; CHECK: ret
  %r = call i32 asm sideeffect "addi $0, $1, 3", "=r,r"(i32 %a)
  %s = call i32 asm sideeffect "mfsr $0, cycle", "=r"()
  %t = add i32 %r, %s
  ret i32 %t
}
