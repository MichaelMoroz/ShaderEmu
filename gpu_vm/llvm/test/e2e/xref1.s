# Relocations across objects, resolved by ld.lld (run.sh checks the disassembly).
  .text
  .globl _start
_start:
  beq a0, a1, target
  lpc a2, target
  call target
  lw a3, %tprel(tv)(tp)
  li a7, 93
  syscall
