# setjmp and longjmp for the test runtime: the buffer keeps ra, sp and s0-s23 (gp and tp are the system's).
	.text
	.globl	setjmp
	.globl	_setjmp
	.p2align	4
setjmp:
_setjmp:
	sw	ra, 0(a0)
	sw	sp, 4(a0)
	sw	s0, 8(a0)
	sw	s1, 12(a0)
	sw	s2, 16(a0)
	sw	s3, 20(a0)
	sw	s4, 24(a0)
	sw	s5, 28(a0)
	sw	s6, 32(a0)
	sw	s7, 36(a0)
	sw	s8, 40(a0)
	sw	s9, 44(a0)
	sw	s10, 48(a0)
	sw	s11, 52(a0)
	sw	s12, 56(a0)
	sw	s13, 60(a0)
	sw	s14, 64(a0)
	sw	s15, 68(a0)
	sw	s16, 72(a0)
	sw	s17, 76(a0)
	sw	s18, 80(a0)
	sw	s19, 84(a0)
	sw	s20, 88(a0)
	sw	s21, 92(a0)
	sw	s22, 96(a0)
	sw	s23, 100(a0)
	li	a0, 0
	ret

	.globl	longjmp
	.globl	_longjmp
	.p2align	4
longjmp:
_longjmp:
	lw	ra, 0(a0)
	lw	sp, 4(a0)
	lw	s0, 8(a0)
	lw	s1, 12(a0)
	lw	s2, 16(a0)
	lw	s3, 20(a0)
	lw	s4, 24(a0)
	lw	s5, 28(a0)
	lw	s6, 32(a0)
	lw	s7, 36(a0)
	lw	s8, 40(a0)
	lw	s9, 44(a0)
	lw	s10, 48(a0)
	lw	s11, 52(a0)
	lw	s12, 56(a0)
	lw	s13, 60(a0)
	lw	s14, 64(a0)
	lw	s15, 68(a0)
	lw	s16, 72(a0)
	lw	s17, 76(a0)
	lw	s18, 80(a0)
	lw	s19, 84(a0)
	lw	s20, 88(a0)
	lw	s21, 92(a0)
	lw	s22, 96(a0)
	lw	s23, 100(a0)
	sltui	t0, a1, 1		# longjmp(b, 0) returns 1
	add	a0, a1, t0
	ret
