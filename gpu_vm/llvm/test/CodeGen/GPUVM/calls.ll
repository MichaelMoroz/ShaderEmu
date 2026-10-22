; RUN: llc -mtriple=gpuvm < %s | FileCheck %s
; The calling convention: a0-a7, then 4-byte stack slots; a value of two
; parts (i64, double) whole in registers or whole on the stack; variadic
; arguments on the stack; results in a0, a1, larger ones by sret; ra and
; s0-s23 kept; tail calls.

declare i32 @callee(i32, i32, i32, i32, i32, i32, i32, i32, i32, i32)
declare i64 @callee64(i32, i64, i32, i64, i32, i64, i64)
declare i32 @vararg(i32, ...)

define i32 @many() {
; CHECK-LABEL: many:
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: li t0, 10
; CHECK: sw t0, 4(sp)
; CHECK: li t0, 9
; CHECK: sw t0, 0(sp)
; CHECK: li a0, 1
; CHECK: li a1, 2
; CHECK: li a2, 3
; CHECK: li a3, 4
; CHECK: li a4, 5
; CHECK: li a5, 6
; CHECK: li a6, 7
; CHECK: li a7, 8
; CHECK: call callee
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: ret
  %r = call i32 @callee(i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7,
                        i32 8, i32 9, i32 10)
  ret i32 %r
}

define i64 @pairs(i64 %x) {
; CHECK-LABEL: pairs:
; CHECK: addi sp, sp, -32
; CHECK: sw ra, 28(sp)
; CHECK: li t0, 6
; CHECK: sw t0, 8(sp)
; CHECK: mv a2, a1
; CHECK: sw zero, 12(sp)
; CHECK: mv a1, a0
; CHECK: sw zero, 4(sp)
; CHECK: li t0, 5
; CHECK: sw t0, 0(sp)
; CHECK: li a0, 1
; CHECK: li a3, 2
; CHECK: li a4, 3
; CHECK: li a6, 4
; CHECK: li a5, 0
; CHECK: call callee64
; CHECK: lw ra, 28(sp)
; CHECK: addi sp, sp, 32
; CHECK: ret
  %r = call i64 @callee64(i32 1, i64 %x, i32 2, i64 3, i32 4, i64 5, i64 6)
  ret i64 %r
}

define i32 @incoming(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e, i32 %f,
; CHECK-LABEL: incoming:
; CHECK: lw t0, 4(sp)
; CHECK: add a0, a0, t0
; CHECK: ret
                     i32 %g, i32 %h, i32 %i, i32 %j) {
  %r = add i32 %a, %j
  ret i32 %r
}

define i32 @varargs(i32 %x) {
; CHECK-LABEL: varargs:
; CHECK: addi sp, sp, -32
; CHECK: sw ra, 28(sp)
; CHECK: li t0, 1074266112
; CHECK: sw t0, 16(sp)
; CHECK: li t0, 2
; CHECK: sw t0, 4(sp)
; CHECK: li t0, 1
; CHECK: sw zero, 12(sp)
; CHECK: sw zero, 8(sp)
; CHECK: sw t0, 0(sp)
; CHECK: call vararg
; CHECK: lw ra, 28(sp)
; CHECK: addi sp, sp, 32
; CHECK: ret
  %r = call i32 (i32, ...) @vararg(i32 %x, i32 1, i64 2, double 3.0)
  ret i32 %r
}

define i32 @va_callee(i32 %n, ...) {
; CHECK-LABEL: va_callee:
; CHECK: addi sp, sp, -16
; CHECK: lw t0, 16(sp)
; CHECK: addi t1, sp, 16
; CHECK: ori t1, t1, 8
; CHECK: sw t1, 12(sp)
; CHECK: lw t1, 20(sp)
; CHECK: add a0, t0, t1
; CHECK: addi sp, sp, 16
; CHECK: ret
  %ap = alloca ptr
  call void @llvm.va_start.p0(ptr %ap)
  %a = va_arg ptr %ap, i32
  %b = va_arg ptr %ap, i32
  call void @llvm.va_end.p0(ptr %ap)
  %r = add i32 %a, %b
  ret i32 %r
}

%big = type { i32, i32, i32, i32 }

define %big @sret(i32 %a) {
; CHECK-LABEL: sret:
; CHECK: sw a1, 12(a0)
; CHECK: sw a1, 0(a0)
; CHECK: ret
  %x = insertvalue %big undef, i32 %a, 0
  %y = insertvalue %big %x, i32 %a, 3
  ret %big %y
}

define float @floats(float %a, float %b) {
; CHECK-LABEL: floats:
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: mv t0, a0
; CHECK: mv a0, a1
; CHECK: mv a1, t0
; CHECK: call fcallee
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: ret
  %r = call float @fcallee(float %b, float %a)
  ret float %r
}
declare float @fcallee(float, float)

define void @indirect(ptr %f) {
; CHECK-LABEL: indirect:
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: callr a0
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: ret
  call void %f()
  ret void
}

; callee-saved registers around a call
define i32 @keep(i32 %a, i32 %b) {
; CHECK-LABEL: keep:
; CHECK: addi sp, sp, -32
; CHECK: sw ra, 28(sp)
; CHECK: sw s1, 24(sp)
; CHECK: sw s2, 20(sp)
; CHECK: mv s1, a1
; CHECK: sw zero, 4(sp)
; CHECK: mv s2, a0
; CHECK: sw zero, 0(sp)
; CHECK: li a0, 0
; CHECK: li a1, 0
; CHECK: li a2, 0
; CHECK: li a3, 0
; CHECK: li a4, 0
; CHECK: li a5, 0
; CHECK: li a6, 0
; CHECK: li a7, 0
; CHECK: call callee
; CHECK: lw ra, 28(sp)
; CHECK: add t0, s2, s1
; CHECK: lw s1, 24(sp)
; CHECK: lw s2, 20(sp)
; CHECK: add a0, t0, a0
; CHECK: addi sp, sp, 32
; CHECK: ret
  %r = call i32 @callee(i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0,
                        i32 0, i32 0, i32 0)
  %s = add i32 %a, %b
  %t = add i32 %s, %r
  ret i32 %t
}

declare void @llvm.va_start.p0(ptr)
declare void @llvm.va_end.p0(ptr)

; tail calls: j to the callee after the epilogue, jr through a register the
; epilogue keeps; not with arguments on the stack
define i32 @tail(i32 %a) {
; CHECK-LABEL: tail:
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: addi a0, a0, 1
; CHECK: sw zero, 4(sp)
; CHECK: li a1, 0
; CHECK: sw zero, 0(sp)
; CHECK: li a2, 0
; CHECK: li a3, 0
; CHECK: li a4, 0
; CHECK: li a5, 0
; CHECK: li a6, 0
; CHECK: li a7, 0
; CHECK: call callee
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: ret
  %b = add i32 %a, 1
  %r = tail call i32 @callee(i32 %b, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0)
  ret i32 %r
}

declare i32 @callee2(i32, i32)

define i32 @tail_regs(i32 %a, i32 %b) {
; CHECK-LABEL: tail_regs:
; CHECK: mv t0, a0
; CHECK: mv a0, a1
; CHECK: mv a1, t0
; CHECK: j callee2
  %r = tail call i32 @callee2(i32 %b, i32 %a)
  ret i32 %r
}

define i32 @tail_indirect(ptr %f, i32 %a) {
; CHECK-LABEL: tail_indirect:
; CHECK: mv t0, a0
; CHECK: mv a0, a1
; CHECK: jr t0
  %r = tail call i32 %f(i32 %a, i32 %a)
  ret i32 %r
}
