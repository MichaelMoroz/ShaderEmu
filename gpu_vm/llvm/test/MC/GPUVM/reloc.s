# Relocations: a symbol in a long immediate (R_GVM_IMM32, %tprel R_GVM_TPREL32), lpc (R_GVM_PCIMM32), a branch
# (R_GVM_BR9 at the control word), j and call (R_GVM_J18: j far, cc 0, path 0), data (R_GVM_32, R_GVM_REL32);
# a j to a label of the section is resolved near.
# RUN: llvm-mc -triple=gpuvm -filetype=obj %s -o %t.o
# RUN: llvm-readelf -r %t.o | FileCheck --check-prefix=REL %s
# RUN: llvm-objdump -d -z %t.o | FileCheck %s

  .text
  .globl f
f:
  la a0, ext
  lw a1, ext+8(zero)
  lpc a2, ext
  beq a0, a1, ext
  j ext
  call ext
  lw a3, %tprel(tv)(tp)
  j near
  nop
near:
  j f
  lla a0, near
  .data
  .word ext
  .word ext - .
  .word f

# REL: 00000008 00000303 R_GVM_IMM32 00000000 ext + 0
# REL: 00000018 00000303 R_GVM_IMM32 00000000 ext + 8
# REL: 00000028 00000304 R_GVM_PCIMM32 00000000 ext + 0
# REL: 0000002c 00000305 R_GVM_BR9 00000000 ext + 0
# REL: 0000003c 00000306 R_GVM_J18 00000000 ext + 0
# REL: 0000004c 00000306 R_GVM_J18 00000000 ext + 0
# REL: 00000058 00000408 R_GVM_TPREL32 00000000 tv + 0
# REL: 0000007c 00000206 R_GVM_J18 00000000 f + 0
# REL: 00000000 00000301 R_GVM_32 00000000 ext + 0
# REL: 00000004 00000302 R_GVM_REL32 00000000 ext + 0
# REL: 00000008 00000201 R_GVM_32 00000000 f + 0
# CHECK: 0: 40 01 06 00 00 00 00 00 00 00 00 00 af 02 00 00 li a0, 0 ;;
# CHECK: 10: 80 01 08 00 00 00 00 00 00 00 00 00 ab 02 00 00 lw a1, 0(zero) ;;
# CHECK: 20: c0 e1 00 00 00 00 00 00 00 00 00 00 28 2a 00 18 lpc a2, 0x20; beq a0, a1, 0x20 ;;
# CHECK: 30: 00 00 00 00 00 00 00 00 00 00 00 00 00 28 00 00 j 0x30 ;;
# CHECK: 40: 00 00 00 00 00 00 00 00 00 00 00 00 00 30 00 00 call 0x40 ;;
# CHECK: 50: 04 02 08 00 00 00 00 00 00 00 00 00 2b 02 04 00 lw a3, 0(tp); j 0x70 ;;
# CHECK: 60: 00 00 04 00 00 00 00 00 00 00 00 00 8f 02 00 00 nop ;;
# CHECK: 70: 00 00 00 00 00 00 00 00 00 00 00 00 00 28 00 00 j 0x70 ;;
# CHECK: 80: 40 e1 00 00 00 00 00 00 f0 ff ff ff a8 02 00 00 lpc a0, 0x70 ;;
