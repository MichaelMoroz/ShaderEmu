; RUN: llc -mtriple=gpuvm -O2 < %s | FileCheck %s
; The fill across blocks (GPUVMGlobalFill) runs a load before the branch that
; guards it only where it cannot fault. `lw p, 0(p)` accessed the old p, not
; the value the guarded `lw x, -4(p)` reads through (gpu_vm/userland/apps/bugs/
; spec_load.c: QuickJS read a header before testing the value's tag).

define i32 @copy_out(ptr %pp, ptr %out) {
; CHECK-LABEL: copy_out:
; CHECK: bltui
; CHECK-NOT: bltui
; CHECK: lw {{[a-z0-9]+}}, -4(
entry:
  %0 = load ptr, ptr %pp, align 4
  %p = load ptr, ptr %0, align 4
  %tagp = getelementptr inbounds i8, ptr %0, i32 4
  %tag = load i32, ptr %tagp, align 4
  %cmp = icmp ugt i32 %tag, -10
  br i1 %cmp, label %then, label %end

then:
  %h = getelementptr inbounds i8, ptr %p, i32 -4
  %rc = load i32, ptr %h, align 4
  %inc = add nsw i32 %rc, 1
  store i32 %inc, ptr %h, align 4
  br label %end

end:
  store ptr %p, ptr %out, align 4
  %o = getelementptr inbounds i8, ptr %out, i32 4
  store i32 %tag, ptr %o, align 4
  ret i32 1
}
