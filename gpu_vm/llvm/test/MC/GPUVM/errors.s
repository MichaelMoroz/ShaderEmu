# RUN: not llvm-mc -triple=gpuvm -filetype=obj %s -o /dev/null 2>&1 | FileCheck %s
# (the checks give line:column; the RUN lines are the first 2 of the file)

  addi a0, a1
  beqi a0, 40, 1f
  bbs a0, 32, 1f
  addbnz a0, 32, 1f
  bump sp, 4096
  bfe a0, a1, 32, 1
  bfe a0, a1, 0, 33
  mfsr a0, nosuch
  lw a0, 4(a1
  foo a0
  beq a0, a1, 8
  lw a0, 0x100000000
  lw a0, %hi(x)
1:

# CHECK: :4:3: error: too few operands for instruction
# CHECK: :5:12: error: immediate must be an integer in the range [-32, 31]
# CHECK: :6:11: error: immediate must be an integer in the range [0, 31]
# CHECK: :7:14: error: immediate must be an integer in the range [-32, 31]
# CHECK: :8:12: error: immediate must be an integer in the range [-2048, 2047]
# CHECK: :9:15: error: immediate must be an integer in the range [0, 31]
# CHECK: :10:18: error: length must be an integer in the range [1, 32]
# CHECK: :11:12: error: expected a special register (a name or 0-4095)
# CHECK: :12:14: error: expected ')'
# CHECK: :13:3: error: unrecognized instruction mnemonic
# CHECK: :14:15: error: branch target must be a label or a multiple of 16
# CHECK: :15:10: error: immediate must be a 32-bit value or a symbol
# CHECK: :16:10: error: unknown operator: only %tprel
