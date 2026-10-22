# The packing rules (gvm_asm.h's) at work: exits with 68 (55 + 12 + 1).
    .text
    .globl _start
_start:
    la     t0, arr
    li     t1, 10
    li     a0, 0
loop:
    lw     t2, 0(t0)
    add    a0, a0, t2
    bump   t0, 4
    addbnz t1, -1, loop
    # memory pairs
    la     t3, buf
    sw     a0, 0(t3)
    lw     t4, 4(t3)
    lw     t5, 8(t3)
    lw     t6, 0(t5)
    sw     t6, 12(t3)
    # floats: (1.5 + 2.0)^2 = 12.25
    li     t7, 0x3fc00000
    faddi  t8, t7, 2.0
    fmul   t9, t8, t8
    ftoi   t10, t9
    add    a0, a0, t10
    call   func
    lla    t11, arr
    j      far
    .rept 600
    nop
    .endr
far:
    li     a7, 93
    syscall

func:
    addi   a0, a0, 1
    ret

    .data
arr:
    .word 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
buf:
    .zero 16
