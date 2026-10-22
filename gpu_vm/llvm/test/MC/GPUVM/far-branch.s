# A branch reaches +-256 bundles; a j beyond becomes the far j (cc 0, sub 5, path 0).
# RUN: not llvm-mc -triple=gpuvm -filetype=obj --defsym ERR=1 %s -o /dev/null 2>&1 | FileCheck --check-prefix=ERR %s
# RUN: llvm-mc -triple=gpuvm -filetype=obj %s -o %t.o
# RUN: llvm-objdump -d -z %t.o | FileCheck %s

0:
  bne a0, a1, 1f
  add a0, a1, a2
  j 1f
  .rept 506
  nop
  .endr
1:
  j 2f
2:
  nop ;;
  nop
  j 0b
  .ifdef ERR
# ERR: :[[@LINE+1]]:15: error: branch target out of range
  beq a0, a1, 0b
  .endif

# 255 bundles: still near
# CHECK: 0: 00 00 00 00 00 00 00 00 00 00 00 00 83 2a fe 19 bne a0, a1, 0xff0 ;;
# 254 bundles, near (beq r0, r0), the ops' path
# CHECK: 10: 46 01 04 1c 00 00 00 00 00 00 00 00 0b 02 fc 01 add a0, a1, a2; j 0xff0 ;;
# CHECK: ff0: 00 00 00 00 00 00 00 00 00 00 00 00 03 02 02 00 j 0x1000 ;;
# CHECK: 1000: 00 00 04 00 00 00 00 00 00 00 00 00 8b 02 00 00 nop ;;
# -257 bundles: far
# CHECK: 1010: 00 00 04 00 00 00 00 00 00 00 00 00 08 e8 bf ff nop; j 0x0 ;;
