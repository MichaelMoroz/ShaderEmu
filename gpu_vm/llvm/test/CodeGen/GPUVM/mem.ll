; RUN: llc -mtriple=gpuvm < %s | FileCheck %s
; Loads and stores of every width, base + imm32 (a symbol too), indexed
; base + (index << s), and the memory intrinsics: texels by lq/sq when both
; sides are 16-aligned, the commit's operation (sq of a descriptor to
; 0xf0000000) from 4 KB up and for llvm.gpuvm.copy/fill, libc otherwise.

@g = global [100 x i32] zeroinitializer
@h = global [100 x i16] zeroinitializer

define i32 @load_global() {
; CHECK-LABEL: load_global:
; CHECK: lw a0, g+20(zero)
; CHECK: ret
  %p = getelementptr [100 x i32], ptr @g, i32 0, i32 5
  %v = load i32, ptr %p
  ret i32 %v
}

define i32 @load_indexed(ptr %p, i32 %i) {
; CHECK-LABEL: load_indexed:
; CHECK: lwx a0, a0, a1, 2
; CHECK: ret
  %a = getelementptr i32, ptr %p, i32 %i
  %v = load i32, ptr %a
  ret i32 %v
}

define void @store_indexed(ptr %p, i32 %i, i16 %v) {
; CHECK-LABEL: store_indexed:
; CHECK: shx a2, a0, a1, 1
; CHECK: ret
  %a = getelementptr i16, ptr %p, i32 %i
  store i16 %v, ptr %a
  ret void
}

define i32 @load_global_indexed(i32 %i) {
; CHECK-LABEL: load_global_indexed:
; CHECK: slli t0, a0, 1
; CHECK: lh a0, h(t0)
; CHECK: ret
  %p = getelementptr [100 x i16], ptr @h, i32 0, i32 %i
  %v = load i16, ptr %p
  %e = sext i16 %v to i32
  ret i32 %e
}

define i32 @widths(ptr %p) {
; CHECK-LABEL: widths:
; CHECK: lb t0, 0(a0)
; CHECK: lbu t1, 1(a0)
; CHECK: lhu t2, 4000(a0)
; CHECK: add t0, t0, t1
; CHECK: add a0, t0, t2
; CHECK: ret
  %a = load i8, ptr %p
  %b = getelementptr i8, ptr %p, i32 1
  %c = load i8, ptr %b
  %d = getelementptr i8, ptr %p, i32 4000
  %e = load i16, ptr %d
  %ae = sext i8 %a to i32
  %ce = zext i8 %c to i32
  %ee = zext i16 %e to i32
  %s = add i32 %ae, %ce
  %t = add i32 %s, %ee
  ret i32 %t
}

define void @memcpy_texels(ptr align 16 %d, ptr align 16 %s) {
; CHECK-LABEL: memcpy_texels:
; CHECK: lw t0, 32(a1)
; CHECK: sw t0, 32(a0)
; CHECK: lq a2, 16(a1)
; CHECK: sq a2, 16(a0)
; CHECK: lq a1, 0(a1)
; CHECK: sq a1, 0(a0)
; CHECK: ret
  call void @llvm.memcpy.p0.p0.i32(ptr align 16 %d, ptr align 16 %s, i32 36, i1 false)
  ret void
}

define void @memcpy_words(ptr align 4 %d, ptr align 4 %s) {
; CHECK-LABEL: memcpy_words:
; CHECK: lw t0, 8(a1)
; CHECK: sw t0, 8(a0)
; CHECK: lw t0, 4(a1)
; CHECK: sw t0, 4(a0)
; CHECK: lw t0, 0(a1)
; CHECK: sw t0, 0(a0)
; CHECK: ret
  call void @llvm.memcpy.p0.p0.i32(ptr align 4 %d, ptr align 4 %s, i32 12, i1 false)
  ret void
}

define void @memcpy_op(ptr %d, ptr %s) {
; CHECK-LABEL: memcpy_op:
; CHECK: mv a3, a1
; CHECK: mv a2, a0
; CHECK: li a4, 4096
; CHECK: li a1, 1
; CHECK: sq a1, -268435456(zero)
; CHECK: ret
  call void @llvm.memcpy.p0.p0.i32(ptr %d, ptr %s, i32 4096, i1 false)
  ret void
}

define void @memmove_op(ptr %d, ptr %s) {
; CHECK-LABEL: memmove_op:
; CHECK: mv a3, a1
; CHECK: mv a2, a0
; CHECK: li a4, 10000
; CHECK: li a1, 1
; CHECK: sq a1, -268435456(zero)
; CHECK: ret
  call void @llvm.memmove.p0.p0.i32(ptr %d, ptr %s, i32 10000, i1 false)
  ret void
}

define void @memset_op(ptr %d, i8 %v) {
; CHECK-LABEL: memset_op:
; CHECK: mv a3, a0
; CHECK: andi t0, a1, 255
; CHECK: muli a4, t0, 16843009
; CHECK: li a5, 5000
; CHECK: li a2, 2
; CHECK: sq a2, -268435456(zero)
; CHECK: ret
  call void @llvm.memset.p0.i32(ptr %d, i8 %v, i32 5000, i1 false)
  ret void
}

define void @memset_op_const(ptr %d) {
; CHECK-LABEL: memset_op_const:
; CHECK: mv a1, a0
; CHECK: li a3, 8192
; CHECK: li a2, 117901063
; CHECK: li a0, 2
; CHECK: sq a0, -268435456(zero)
; CHECK: ret
  call void @llvm.memset.p0.i32(ptr %d, i8 7, i32 8192, i1 false)
  ret void
}

define void @memcpy_libc(ptr %d, ptr %s, i32 %n) {
; CHECK-LABEL: memcpy_libc:
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: call memcpy
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: ret
  call void @llvm.memcpy.p0.p0.i32(ptr %d, ptr %s, i32 %n, i1 false)
  ret void
}

define void @builtins(ptr %d, ptr %s, i32 %n) {
; CHECK-LABEL: builtins:
; CHECK: mv a5, a2
; CHECK: mv a4, a1
; CHECK: mv a3, a0
; CHECK: li a2, 1
; CHECK: sq a2, -268435456(zero)
; CHECK: li a5, 64
; CHECK: li a4, 305419896
; CHECK: li a2, 2
; CHECK: sq a2, -268435456(zero)
; CHECK: ret
  call void @llvm.gpuvm.copy(ptr %d, ptr %s, i32 %n)
  call void @llvm.gpuvm.fill(ptr %d, i32 305419896, i32 64)
  ret void
}

declare void @llvm.memcpy.p0.p0.i32(ptr, ptr, i32, i1)
declare void @llvm.memmove.p0.p0.i32(ptr, ptr, i32, i1)
declare void @llvm.memset.p0.i32(ptr, i8, i32, i1)
declare void @llvm.gpuvm.copy(ptr, ptr, i32)
declare void @llvm.gpuvm.fill(ptr, i32, i32)
