; RUN: llc -mtriple=gpuvm-unknown-linux < %s | FileCheck %s
; GPUVMHoistConstants makes a branch's constant in the entry block. When the
; entry ends in an invoke, the constant goes before the call: after it, only
; the normal path would make it, and the landing pad's path would compare
; against whatever the register held (cxx_eh's main, 24 October 2026).

declare void @may_throw()
declare i32 @get()
declare void @use(i32)
declare i32 @__gxx_personality_v0(...)
declare ptr @__cxa_begin_catch(ptr)
declare void @__cxa_end_catch()

define void @f() personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: f:
; CHECK: li [[K:s[0-9]+]], 1000
; CHECK: call may_throw
; CHECK: call get
; CHECK: bne a0, [[K]],
entry:
  invoke void @may_throw() to label %cont unwind label %lp
lp:
  %e = landingpad { ptr, i32 } catch ptr null
  %p = extractvalue { ptr, i32 } %e, 0
  %c = call ptr @__cxa_begin_catch(ptr %p)
  call void @__cxa_end_catch()
  br label %cont
cont:
  %v = call i32 @get()
  %t = icmp eq i32 %v, 1000
  br i1 %t, label %yes, label %no
yes:
  call void @use(i32 1)
  ret void
no:
  ret void
}
