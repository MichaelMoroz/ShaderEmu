; RUN: llc -mtriple=gpuvm < %s | FileCheck %s
; Compare-and-branch in the control word: registers, imm6 (and a > k as
; a >= k + 1), the unsigned forms against a sign-extended imm6, bit tests,
; and compares that do not branch (slt, seq ... with any immediate).

declare void @f()

define void @beq(i32 %a, i32 %b) {
; CHECK-LABEL: beq:
; CHECK: bne a0, a1, .LBB0_2
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: call f
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: .LBB0_2:
; CHECK: ret
  %c = icmp eq i32 %a, %b
  br i1 %c, label %t, label %e
t:
  call void @f()
  br label %e
e:
  ret void
}

define void @bgt(i32 %a, i32 %b) {
; CHECK-LABEL: bgt:
; CHECK: bge a1, a0, .LBB1_2
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: call f
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: .LBB1_2:
; CHECK: ret
  %c = icmp sgt i32 %a, %b
  br i1 %c, label %t, label %e
t:
  call void @f()
  br label %e
e:
  ret void
}

define void @blti(i32 %a) {
; CHECK-LABEL: blti:
; CHECK: bgei a0, -32, .LBB2_2
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: call f
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: .LBB2_2:
; CHECK: ret
  %c = icmp slt i32 %a, -32
  br i1 %c, label %t, label %e
t:
  call void @f()
  br label %e
e:
  ret void
}

define void @bgti(i32 %a) {
; CHECK-LABEL: bgti:
; CHECK: blti a0, 31, .LBB3_2
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: call f
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: .LBB3_2:
; CHECK: ret
  %c = icmp sgt i32 %a, 30
  br i1 %c, label %t, label %e
t:
  call void @f()
  br label %e
e:
  ret void
}

define void @bltui(i32 %a) {
; CHECK-LABEL: bltui:
; CHECK: bgeui a0, 17, .LBB4_2
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: call f
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: .LBB4_2:
; CHECK: ret
  %c = icmp ult i32 %a, 17
  br i1 %c, label %t, label %e
t:
  call void @f()
  br label %e
e:
  ret void
}

define void @bltu_big(i32 %a) {
; CHECK-LABEL: bltu_big:
; CHECK: li t0, 99
; CHECK: bltu t0, a0, .LBB5_2
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: call f
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: .LBB5_2:
; CHECK: ret
  %c = icmp ult i32 %a, 100
  br i1 %c, label %t, label %e
t:
  call void @f()
  br label %e
e:
  ret void
}

define void @bbs(i32 %a) {
; CHECK-LABEL: bbs:
; CHECK: bbc a0, 12, .LBB6_2
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: call f
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: .LBB6_2:
; CHECK: ret
  %m = and i32 %a, 4096
  %c = icmp ne i32 %m, 0
  br i1 %c, label %t, label %e
t:
  call void @f()
  br label %e
e:
  ret void
}

define void @bbc(i32 %a) {
; CHECK-LABEL: bbc:
; CHECK: bbs a0, 1, .LBB7_2
; CHECK: addi sp, sp, -16
; CHECK: sw ra, 12(sp)
; CHECK: call f
; CHECK: lw ra, 12(sp)
; CHECK: addi sp, sp, 16
; CHECK: .LBB7_2:
; CHECK: ret
  %m = and i32 %a, 2
  %c = icmp eq i32 %m, 0
  br i1 %c, label %t, label %e
t:
  call void @f()
  br label %e
e:
  ret void
}

define i32 @setcc(i32 %a, i32 %b) {
; CHECK-LABEL: setcc:
; CHECK: slt t0, a0, a1
; CHECK: sgeui t1, a0, 100001
; CHECK: add t0, t0, t1
; CHECK: seqi t2, a1, 7
; CHECK: slti t3, a0, -2
; CHECK: add t1, t2, t3
; CHECK: slli t1, t1, 2
; CHECK: or a0, t0, t1
; CHECK: ret
  %c1 = icmp slt i32 %a, %b
  %c2 = icmp ugt i32 %a, 100000
  %c3 = icmp eq i32 %b, 7
  %c4 = icmp sle i32 %a, -3
  %x1 = zext i1 %c1 to i32
  %x2 = zext i1 %c2 to i32
  %x3 = zext i1 %c3 to i32
  %x4 = zext i1 %c4 to i32
  %s1 = add i32 %x1, %x2
  %s2 = add i32 %x3, %x4
  %s = shl i32 %s2, 2
  %r = add i32 %s1, %s
  ret i32 %r
}

define i32 @select(i32 %a, i32 %b, i32 %c) {
; CHECK-LABEL: select:
; CHECK: sel a0, a0, a1, a2
; CHECK: ret
  %t = icmp ne i32 %c, 0
  %r = select i1 %t, i32 %a, i32 %b
  ret i32 %r
}

define i32 @loop(ptr %p, i32 %n) {
; CHECK-LABEL: loop:
; CHECK: li t0, 0
; CHECK: .LBB10_1:
; CHECK: lw t1, 0(a0)
; CHECK: add t0, t0, t1
; CHECK: addi a0, a0, 4
; CHECK: addbnz a1, -1, .LBB10_1
; CHECK: mv a0, t0
; CHECK: ret
entry:
  br label %l
l:
  %i = phi i32 [ 0, %entry ], [ %i1, %l ]
  %s = phi i32 [ 0, %entry ], [ %s1, %l ]
  %a = getelementptr i32, ptr %p, i32 %i
  %v = load i32, ptr %a
  %s1 = add i32 %s, %v
  %i1 = add i32 %i, 1
  %c = icmp ne i32 %i1, %n
  br i1 %c, label %l, label %e
e:
  ret i32 %s1
}
