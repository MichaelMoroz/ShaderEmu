; RUN: llc -mtriple=gpuvm-unknown-linux < %s | FileCheck %s
; Call frame information for C++'s exceptions (GPUVMFrameLowering): the CFA's
; offset after each move of sp, where each callee-saved register is, the CFA
; on fp; a landing pad finds the exception in a0 and the selector in a1. No
; CFI for a function that cannot unwind (C compiled by gpuvm-linux-cc is
; nounwind), nor in an epilogue.

declare void @use(ptr)
declare void @may_throw()
declare i32 @__gxx_personality_v0(...)
declare ptr @__cxa_begin_catch(ptr)
declare void @__cxa_end_catch()

define void @small() uwtable {
; CHECK-LABEL: small:
; CHECK: .cfi_startproc
; CHECK: addi sp, sp, -32
; CHECK: .cfi_def_cfa_offset 32
; CHECK: sw ra, 28(sp)
; CHECK: .cfi_offset ra, -4
; CHECK: call use
; CHECK-NOT: .cfi_
; CHECK: ret
; CHECK: .cfi_endproc
  %a = alloca [4 x i32]
  call void @use(ptr %a)
  ret void
}

define void @nocfi() nounwind {
; CHECK-LABEL: nocfi:
; CHECK-NOT: .cfi_
; CHECK: ret
  %a = alloca [4 x i32]
  call void @use(ptr %a)
  ret void
}

; over 2 KB: sp in two steps around the callee-saved registers' stores
define void @big() uwtable {
; CHECK-LABEL: big:
; CHECK: addi sp, sp, -16
; CHECK: .cfi_def_cfa_offset 16
; CHECK: sw ra, 12(sp)
; CHECK: .cfi_offset ra, -4
; CHECK: addi sp, sp, -4096
; CHECK: .cfi_def_cfa_offset 4112
; CHECK: call use
  %a = alloca [4096 x i8]
  call void @use(ptr %a)
  ret void
}

; fp: the CFA is fp from the prologue's end
define void @dynamic(i32 %n) uwtable {
; CHECK-LABEL: dynamic:
; CHECK: .cfi_def_cfa_offset 16
; CHECK-DAG: .cfi_offset ra, -4
; CHECK-DAG: .cfi_offset s0, -8
; CHECK: addi s0, sp, 16
; CHECK: .cfi_def_cfa s0, 0
; CHECK: call use
  %a = alloca i8, i32 %n
  call void @use(ptr %a)
  ret void
}

; a landing pad: the exception in a0, the selector in a1; no absolute word in
; the tables (a flat program could not relocate it): the personality and the
; types through DW.ref words, pc-relative (0x9b), the LSDA pc-relative (0x1b)
define i32 @catcher() uwtable personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: catcher:
; CHECK: .cfi_personality 155, DW.ref.__gxx_personality_v0
; CHECK: .cfi_lsda 27, .Lexception0
; CHECK: call may_throw
; CHECK: call __cxa_begin_catch
entry:
  invoke void @may_throw() to label %ok unwind label %lp
ok:
  ret i32 0
lp:
  %e = landingpad { ptr, i32 } catch ptr null
  %p = extractvalue { ptr, i32 } %e, 0
  %c = call ptr @__cxa_begin_catch(ptr %p)
  call void @__cxa_end_catch()
  ret i32 1
}
; CHECK: @TType Encoding = indirect pcrel sdata4
