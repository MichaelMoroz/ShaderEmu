# The packing rules (ASM.md, "Bundles"; gpu_vm/ref/gvm_asm.h): which ops share a bundle, the path bits (the
# control word's low byte), and what closes a bundle. Checked by hand against the rules.
# RUN: llvm-mc -triple=gpuvm -filetype=obj %s -o %t.o
# RUN: llvm-objdump -d -z -r %t.o | FileCheck %s
# The text llvm-mc prints assembles to the same object.
# RUN: llvm-mc -triple=gpuvm %s | llvm-mc -triple=gpuvm -filetype=obj -o %t2.o
# RUN: cmp %t.o %t2.o

  .text
  add a0, a1, a2
  sub a3, a0, a1
  mul t0, t1, t2
  mul t3, t4, t5
  sll t6, t3, a0
  lw a0, 0(sp)
  lw a1, 4(sp)
  lw a2, 0(a1)
  sw a2, 8(sp)
  lw a3, 12(sp)
  sw a3, 16(sp)
  lw a4, 20(sp)
  lw a5, 0(a4)
  addi a0, a0, 100000
  addi a1, a1, 200000
  addi a2, a2, 1
  fadd t0, t1, t2
  fmul t3, t0, t0
  fsqrt t4, t3
  faddi t5, t4, 0.5
  lw t6, 0(sp)
  fadd t7, t6, t6
  addi t0, t0, 1
  beq t0, t1, 1f
  addi t2, t2, 1
  beq t0, t1, 1f
  addi a0, a0, 1
  addbnz a0, -1, 1f
  addi a0, a1, 1
  addbnz a1, -1, 1f
1:
  lw t0, 0(a0)
  bump a0, 4
  add t1, t1, t0
  addi t2, t2, 1
  add t3, t3, a0
  ret
  addi sp, sp, -16
  sw ra, 12(sp) ;;
  sw s0, 8(sp)
  .endbundle
  addi a0, a0, 1
  syscall
  addi a0, a0, 2
  jr a0
  la a0, 1b
  la a1, 1b
  nop
  .word 0x12345678
  nop
  j 1b

# CHECK: 0: 46 01 04 1c 05 42 04 18 00 00 00 00 93 02 00 00 add a0, a1, a2; sub a3, a0, a1 ;;
# CHECK-NEXT: 10: 4e 33 05 3c 00 00 00 00 00 00 00 00 8b 02 00 00 mul t0, t1, t2 ;;
# CHECK-NEXT: 20: 11 34 05 48 d0 04 05 14 00 00 00 00 93 02 00 00 mul t3, t4, t5; sll t6, t3, a0 ;;
# CHECK-NEXT: 30: 42 01 08 00 82 01 48 00 00 00 00 00 95 02 00 00 lw a0, 0(sp); lw a1, 4(sp) ;;
# CHECK-NEXT: 40: c6 01 08 00 00 00 00 00 00 00 00 00 8c 02 00 00 lw a2, 0(a1) ;;
# CHECK-NEXT: 50: c2 81 88 00 02 02 c8 00 00 00 00 00 97 02 00 00 sw a2, 8(sp); lw a3, 12(sp) ;;
# CHECK-NEXT: 60: 02 82 08 01 42 02 48 01 00 00 00 00 97 02 00 00 sw a3, 16(sp); lw a4, 20(sp) ;;
# CHECK-NEXT: 70: 89 02 08 00 45 01 06 00 a0 86 01 00 d4 02 00 00 lw a5, 0(a4); addi a0, a0, 100000 ;;
# CHECK-NEXT: 80: 86 01 06 00 c7 01 16 00 40 0d 03 00 b3 02 00 00 addi a1, a1, 200000; addi a2, a2, 1 ;;
# CHECK-NEXT: 90: 4e 03 0c 3c 0d 14 0c 34 00 00 00 00 91 02 00 00 fadd t0, t1, t2; fmul t3, t0, t0 ;;
# CHECK-NEXT: a0: 50 94 0d 00 91 04 0e 3f 00 00 00 00 91 02 00 00 fsqrt t4, t3; faddi t5, t4, 0.5 ;;
# CHECK-NEXT: b0: c2 04 08 00 13 05 0c 4c 00 00 00 00 92 02 00 00 lw t6, 0(sp); fadd t7, t6, t6 ;;
# CHECK-NEXT: c0: 4d 03 16 00 00 00 00 00 00 00 00 00 8b 02 00 00 addi t0, t0, 1 ;;
# CHECK-NEXT: d0: 00 00 00 00 00 00 00 00 00 00 00 00 03 6a 0a 38 beq t0, t1, 0x120 ;;
# CHECK-NEXT: e0: cf 03 16 00 00 00 00 00 00 00 00 00 0b 6a 08 38 addi t2, t2, 1; beq t0, t1, 0x120 ;;
# CHECK-NEXT: f0: 45 01 16 00 00 00 00 00 00 00 00 00 8b 02 00 00 addi a0, a0, 1 ;;
# CHECK-NEXT: 100: 00 00 00 00 00 00 00 00 00 00 00 00 83 28 04 fc addbnz a0, -1, 0x120 ;;
# CHECK-NEXT: 110: 46 01 16 00 00 00 00 00 00 00 00 00 8b 30 02 fc addi a0, a1, 1; addbnz a1, -1, 0x120 ;;
# CHECK-NEXT: 120: 45 03 08 00 8e 03 04 34 00 00 00 00 10 78 41 00 lw t0, 0(a0); add t1, t1, t0; bump a0, 4 ;;
# CHECK-NEXT: 130: cf 03 16 00 10 04 04 14 00 00 00 00 10 50 00 00 addi t2, t2, 1; add t3, t3, a0; ret ;;
# CHECK-NEXT: 140: 82 00 06 ff 00 00 00 00 00 00 00 00 8b 02 00 00 addi sp, sp, -16 ;;
# CHECK-NEXT: 150: 42 80 c8 00 00 00 00 00 00 00 00 00 8e 02 00 00 sw ra, 12(sp) ;;
# CHECK-NEXT: 160: 02 88 88 00 00 00 00 00 00 00 00 00 8e 02 00 00 sw s0, 8(sp) ;;
# CHECK-NEXT: 170: 45 01 16 00 00 00 00 00 00 00 00 00 8b 02 00 00 addi a0, a0, 1 ;;
# CHECK-NEXT: 180: 00 00 01 00 45 01 26 00 00 00 00 00 10 50 01 00 syscall; addi a0, a0, 2; jr a0 ;;
# CHECK-NEXT: 190: 40 01 06 00 00 00 00 00 00 00 00 00 ab 02 00 00 li a0, 0 ;;
# CHECK-NEXT: 00000198: R_GVM_IMM32 .text+0x120
# CHECK-NEXT: 1a0: 80 01 06 00 00 00 04 00 00 00 00 00 b3 02 00 00 li a1, 0; nop ;;
# CHECK-NEXT: 000001a8: R_GVM_IMM32 .text+0x120
# CHECK-NEXT: 1b0: 78 56 34 12 00 00 00 00 00 00 00 00 00 00 00 00 <unknown>
# CHECK-NEXT: 1c0: 00 00 04 00 00 00 00 00 00 00 00 00 0b 02 ec 03 nop; j 0x120 ;;
