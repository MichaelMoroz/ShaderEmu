; RUN: llc -mtriple=gpuvm -O2 -relocation-model=pic < %s | FileCheck %s
; programs/mc/mcw.c's mcw_most: mmap2's length is a1, an inline asm's register input. GlobalFill moved the li of
; 4096 up into the open's block and renamed it to t5 down the chain, the asm's a1 with it: mmap2 got the string's
; address as its length, failed, and every program asked for 0 workers.

; CHECK-LABEL: mcw_most:
; CHECK: li a7, 56
; CHECK: syscall
; CHECK: li a1, 4096
; CHECK: li a7, 222
; CHECK: syscall

@mcw_most.most = internal unnamed_addr global i32 -1, align 4
@.str = private unnamed_addr constant [9 x i8] c"/dev/gpu\00", align 1

define dso_local range(i32 -1, 256) i32 @mcw_most() local_unnamed_addr {
entry:
  %0 = load i32, ptr @mcw_most.most, align 4
  %cmp = icmp sgt i32 %0, -1
  br i1 %cmp, label %cleanup, label %if.end

if.end:                                           ; preds = %entry
  store i32 0, ptr @mcw_most.most, align 4
  %1 = tail call i32 asm sideeffect "syscall", "={r5},{r12},{r6},{r7},{r8},{r9},{r10},{r5},~{memory}"(i32 56, i32 ptrtoint (ptr @.str to i32), i32 2, i32 0, i32 0, i32 0, i32 -100)
  %cmp1 = icmp slt i32 %1, 0
  br i1 %cmp1, label %cleanup, label %if.end3

if.end3:                                          ; preds = %if.end
  %2 = tail call i32 asm sideeffect "syscall", "={r5},{r12},{r6},{r7},{r8},{r9},{r10},{r5},~{memory}"(i32 222, i32 4096, i32 3, i32 1, i32 range(i32 0, -2147483648) %1, i32 4096, i32 0)
  %3 = inttoptr i32 %2 to ptr
  %4 = add i32 %2, 4095
  %cmp5.not = icmp ult i32 %4, 4096
  br i1 %cmp5.not, label %if.end20, label %if.then6

if.then6:                                         ; preds = %if.end3
  %arrayidx = getelementptr inbounds nuw i8, ptr %3, i32 972
  %5 = load volatile i32, ptr %arrayidx, align 4
  %tobool.not = icmp eq i32 %5, 0
  br i1 %tobool.not, label %cond.false, label %cond.true

cond.true:                                        ; preds = %if.then6
  %6 = load volatile i32, ptr %arrayidx, align 4
  br label %cond.end

cond.false:                                       ; preds = %if.then6
  %arrayidx8 = getelementptr inbounds nuw i8, ptr %3, i32 960
  %7 = load volatile i32, ptr %arrayidx8, align 4
  br label %cond.end

cond.end:                                         ; preds = %cond.false, %cond.true
  %cond = phi i32 [ %6, %cond.true ], [ %7, %cond.false ]
  %cmp9 = icmp ugt i32 %cond, 256
  %cond16 = tail call i32 @llvm.usub.sat.i32(i32 %cond, i32 1)
  %cond18 = select i1 %cmp9, i32 255, i32 %cond16
  store i32 %cond18, ptr @mcw_most.most, align 4
  %8 = tail call i32 asm sideeffect "syscall", "={r5},{r12},{r6},{r7},{r8},{r9},{r10},{r5},~{memory}"(i32 215, i32 4096, i32 0, i32 0, i32 0, i32 0, i32 %2)
  br label %if.end20

if.end20:                                         ; preds = %cond.end, %if.end3
  %9 = tail call i32 asm sideeffect "syscall", "={r5},{r12},{r6},{r7},{r8},{r9},{r10},{r5},~{memory}"(i32 57, i32 0, i32 0, i32 0, i32 0, i32 0, i32 %1)
  %10 = load i32, ptr @mcw_most.most, align 4
  br label %cleanup

cleanup:                                          ; preds = %if.end, %entry, %if.end20
  %retval.0 = phi i32 [ %10, %if.end20 ], [ %0, %entry ], [ 0, %if.end ]
  ret i32 %retval.0
}

declare i32 @llvm.usub.sat.i32(i32, i32)

