# A call beyond +-2 MB: ld.lld puts a thunk between (run.sh links .far at 0x02400000). Exits with 7.
    .text
    .globl _start
_start:
    call far
    li a7, 93
    j exit
    .section .far,"ax",@progbits
far:
    li a0, 7
    ret
exit:
    syscall
