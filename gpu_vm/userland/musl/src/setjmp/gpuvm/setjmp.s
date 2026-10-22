# setjmp: ra, sp and s0-s23 (the callee-saved registers; gp and tp are the system's). bits/setjmp.h: 26 words.
	.text
	.global __setjmp
	.global _setjmp
	.global setjmp
	.type __setjmp, %function
	.type _setjmp, %function
	.type setjmp, %function
	.p2align 4
__setjmp:
_setjmp:
setjmp:
	sw ra, 0(a0)
	sw sp, 4(a0)
	sw s0, 8(a0)
	sw s1, 12(a0)
	sw s2, 16(a0)
	sw s3, 20(a0)
	sw s4, 24(a0)
	sw s5, 28(a0)
	sw s6, 32(a0)
	sw s7, 36(a0)
	sw s8, 40(a0)
	sw s9, 44(a0)
	sw s10, 48(a0)
	sw s11, 52(a0)
	sw s12, 56(a0)
	sw s13, 60(a0)
	sw s14, 64(a0)
	sw s15, 68(a0)
	sw s16, 72(a0)
	sw s17, 76(a0)
	sw s18, 80(a0)
	sw s19, 84(a0)
	sw s20, 88(a0)
	sw s21, 92(a0)
	sw s22, 96(a0)
	sw s23, 100(a0)
	li a0, 0
	ret
