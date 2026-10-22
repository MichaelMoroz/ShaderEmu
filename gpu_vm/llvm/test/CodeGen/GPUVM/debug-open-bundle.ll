; RUN: llc -mtriple=gpuvm -O2 -filetype=obj < %s -o %t.o
; RUN: llvm-objdump -d -z %t.o | FileCheck %s
; A function whose last bundle has no control op (a body of `break` alone: an
; abstract class's deleting destructor, gpu_vm/userland/apps/bugs/
; dtor_debug.cpp) with debug information: its bundle is closed at the body's
; end, so that the labels after it are in the section when debug info asks.

; CHECK: <trapping>:
; CHECK: break ;;

define void @trapping() !dbg !5 {
entry:
  call void @llvm.trap(), !dbg !8
  unreachable
}

declare void @llvm.trap()

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!3, !4}

!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "clang", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "t.c", directory: "/")
!3 = !{i32 2, !"Dwarf Version", i32 4}
!4 = !{i32 2, !"Debug Info Version", i32 3}
!5 = distinct !DISubprogram(name: "trapping", scope: !1, file: !1, line: 1, type: !6, scopeLine: 1, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0)
!6 = !DISubroutineType(types: !7)
!7 = !{null}
!8 = !DILocation(line: 2, column: 3, scope: !5)
