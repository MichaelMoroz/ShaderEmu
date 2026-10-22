# longjmp(buf, val): back to setjmp's return with val (1 for 0).
	.text
	.global __longjmp
	.global _longjmp
	.global longjmp
	.type __longjmp, %function
	.type _longjmp, %function
	.type longjmp, %function
	.p2align 4
__longjmp:
_longjmp:
longjmp:
	lw ra, 0(a0)
	lw sp, 4(a0)
	lw s0, 8(a0)
	lw s1, 12(a0)
	lw s2, 16(a0)
	lw s3, 20(a0)
	lw s4, 24(a0)
	lw s5, 28(a0)
	lw s6, 32(a0)
	lw s7, 36(a0)
	lw s8, 40(a0)
	lw s9, 44(a0)
	lw s10, 48(a0)
	lw s11, 52(a0)
	lw s12, 56(a0)
	lw s13, 60(a0)
	lw s14, 64(a0)
	lw s15, 68(a0)
	lw s16, 72(a0)
	lw s17, 76(a0)
	lw s18, 80(a0)
	lw s19, 84(a0)
	lw s20, 88(a0)
	lw s21, 92(a0)
	lw s22, 96(a0)
	lw s23, 100(a0)
	seqi t0, a1, 0
	add a0, a1, t0
	ret
