# Operand forms: immediates that start with '(', memory operands, register names, float literals, special registers.
# RUN: llvm-mc -triple=gpuvm %s | FileCheck %s

  andi t0, t0, (1 << 4)
# CHECK: andi t0, t0, 16
  lw a0, (8)(sp)
# CHECK: lw a0, 8(sp)
  lw a1, (sp)
# CHECK: lw a1, 0(sp)
  addi a0, a0, (2+3)*4
# CHECK: addi a0, a0, 20
  add r32, fp, s0
# CHECK: add s0, s0, s0
  add zero, ra, sp
# CHECK: add zero, ra, sp
  faddi a0, a1, -1.5e-1
# CHECK: faddi a0, a1, -0.150000006
  fmuli a0, a1, inf
# CHECK: fmuli a0, a1, 0x7f800000
  mfsr a0, time_lo
# CHECK: mfsr a0, time_lo
  sq t0, 0xf0000000
# CHECK: sq t0, -268435456(zero)
  lw a3, %tprel(tv)(tp)
# CHECK: lw a3, %tprel(tv)(tp)
  li a0, sym+4
# CHECK: li a0, sym+4
