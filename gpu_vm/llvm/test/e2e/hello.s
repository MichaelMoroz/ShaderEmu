# gpu_vm's hello: write(1, msg, 14), exit(0), by semihosting (gvm_cpu) or Linux.
    .text
    .globl _start
_start:
    li    a7, 64
    li    a0, 1
    la    a1, msg
    li    a2, 14
    syscall
    li    a7, 93
    li    a0, 0
    syscall

    .data
msg:
    .ascii "Hello, world!\n"
