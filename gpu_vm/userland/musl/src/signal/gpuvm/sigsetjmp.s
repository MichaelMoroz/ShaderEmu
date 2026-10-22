# sigsetjmp(buf, savemask): as RISC-V's. With a mask, ra and s0 are kept in the sigjmp_buf past __jmp_buf (__fl at
# 104 and __ss[2] at 116: __sigsetjmp_tail uses __ss[0-1]); setjmp is called with s0 = buf, so a longjmp comes back
# here with s0 = buf, and __sigsetjmp_tail restores (or saved) the mask.
	.text
	.global sigsetjmp
	.global __sigsetjmp
	.type sigsetjmp, %function
	.type __sigsetjmp, %function
	.hidden __sigsetjmp_tail
	.p2align 4
sigsetjmp:
__sigsetjmp:
	bnez a1, 1f
	j setjmp
1:
	sw ra, 104(a0)
	sw s0, 116(a0)
	mv s0, a0
	call setjmp
	mv a1, a0
	mv a0, s0
	lw s0, 116(a0)
	lw ra, 104(a0)
	j __sigsetjmp_tail
