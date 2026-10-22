; RUN: llc -mtriple=gpuvm -relocation-model=pic < %s | FileCheck %s
; RUN: llc -mtriple=gpuvm -relocation-model=pic -filetype=obj < %s | llvm-readobj -r - | FileCheck %s --check-prefix=REL
; Position-independent code (-fPIE/-fPIC): a symbol's address from the
; bundle's (lpc, R_GVM_PCIMM32), with its offset in the immediate, so that
; the text has no absolute relocation; a data word that holds an address keeps
; its R_GVM_32.

@g = global [100 x i32] zeroinitializer
@e = external global i32
@p = global ptr @e

define i32 @load_global() {
; CHECK-LABEL: load_global:
; CHECK: lpc [[R:[a-z0-9]+]], g+20
; CHECK: lw a0, 0([[R]])
  %q = getelementptr [100 x i32], ptr @g, i32 0, i32 5
  %v = load i32, ptr %q
  ret i32 %v
}

define ptr @address() {
; CHECK-LABEL: address:
; CHECK: lpc a0, e+12
  %q = getelementptr i32, ptr @e, i32 3
  ret ptr %q
}

define ptr @function() {
; CHECK-LABEL: function:
; CHECK: lpc a0, address
  ret ptr @address
}

; REL-NOT: R_GVM_IMM32
; REL: R_GVM_PCIMM32 g 0x14
; REL: R_GVM_PCIMM32 e 0xC
; REL: R_GVM_PCIMM32 address 0x0
; REL: R_GVM_32 e 0x0
