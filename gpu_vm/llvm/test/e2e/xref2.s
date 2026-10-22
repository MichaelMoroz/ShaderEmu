  .text
  .globl target
target:
  ret
  .section .tdata,"awT",@progbits
  .word 0
  .globl tv
tv:
  .word 7
