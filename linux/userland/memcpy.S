/*
 * memcpy for the image's C library, in place of musl's (toolchain.sh puts it into libc.a).
 * Two pointers on word boundaries, which most are, go sixteen words a turn and then by the
 * bits of what is left, with no stack frame. Anything else is musl's own (memcpy_any).
 *
 * From 2 KB up, with both pointers at one place in a texel (16 bytes) and both ranges in RAM,
 * the texels between the ends are the commit's copy (docs/machine/gpu.md, "A program's copy"):
 * they never enter the core's write cache. Asking ends the pass. CSR 0x8b0 reads 1 where that
 * is worth it (the first core; 0 on a worker, whose copy would wait for other cores', and on a
 * machine without it), so the same program runs anywhere.
 */
#include "uop.h"

	.text
	.globl	memcpy
	.type	memcpy, @function
memcpy:
	li	t0, UOP_MIN - 1
	bltu	t0, a2, 20f
0:	or	t0, a0, a1
	andi	t0, t0, 3
	bnez	t0, 9f
	mv	t1, a0
	li	t0, 64
	bltu	a2, t0, 2f
	andi	t2, a2, -64
	add	t2, t2, t1
1:
	lw	t3, 0(a1)
	lw	t4, 4(a1)
	lw	t5, 8(a1)
	lw	t6, 12(a1)
	lw	a3, 16(a1)
	lw	a4, 20(a1)
	lw	a5, 24(a1)
	lw	a6, 28(a1)
	sw	t3, 0(t1)
	sw	t4, 4(t1)
	sw	t5, 8(t1)
	sw	t6, 12(t1)
	sw	a3, 16(t1)
	sw	a4, 20(t1)
	sw	a5, 24(t1)
	sw	a6, 28(t1)
	lw	t3, 32(a1)
	lw	t4, 36(a1)
	lw	t5, 40(a1)
	lw	t6, 44(a1)
	lw	a3, 48(a1)
	lw	a4, 52(a1)
	lw	a5, 56(a1)
	lw	a6, 60(a1)
	sw	t3, 32(t1)
	sw	t4, 36(t1)
	sw	t5, 40(t1)
	sw	t6, 44(t1)
	sw	a3, 48(t1)
	sw	a4, 52(t1)
	sw	a5, 56(t1)
	sw	a6, 60(t1)
	addi	a1, a1, 64
	addi	t1, t1, 64
	bne	t1, t2, 1b
	andi	a2, a2, 63
	beqz	a2, 8f
2:	andi	t0, a2, 32
	beqz	t0, 3f
	lw	t3, 0(a1)
	lw	t4, 4(a1)
	lw	t5, 8(a1)
	lw	t6, 12(a1)
	lw	a3, 16(a1)
	lw	a4, 20(a1)
	lw	a5, 24(a1)
	lw	a6, 28(a1)
	sw	t3, 0(t1)
	sw	t4, 4(t1)
	sw	t5, 8(t1)
	sw	t6, 12(t1)
	sw	a3, 16(t1)
	sw	a4, 20(t1)
	sw	a5, 24(t1)
	sw	a6, 28(t1)
	addi	a1, a1, 32
	addi	t1, t1, 32
3:	andi	t0, a2, 16
	beqz	t0, 4f
	lw	t3, 0(a1)
	lw	t4, 4(a1)
	lw	t5, 8(a1)
	lw	t6, 12(a1)
	sw	t3, 0(t1)
	sw	t4, 4(t1)
	sw	t5, 8(t1)
	sw	t6, 12(t1)
	addi	a1, a1, 16
	addi	t1, t1, 16
4:	andi	t0, a2, 8
	beqz	t0, 5f
	lw	t3, 0(a1)
	lw	t4, 4(a1)
	sw	t3, 0(t1)
	sw	t4, 4(t1)
	addi	a1, a1, 8
	addi	t1, t1, 8
5:	andi	t0, a2, 4
	beqz	t0, 6f
	lw	t3, 0(a1)
	sw	t3, 0(t1)
	addi	a1, a1, 4
	addi	t1, t1, 4
6:	andi	t0, a2, 3
	beqz	t0, 8f
	/* the last bytes by themselves: a word's store would write past the end */
	andi	t0, a2, 2
	beqz	t0, 7f
	lhu	t3, 0(a1)
	sh	t3, 0(t1)
	addi	a1, a1, 2
	addi	t1, t1, 2
7:	andi	t0, a2, 1
	beqz	t0, 8f
	lbu	t3, 0(a1)
	sb	t3, 0(t1)
8:	ret
9:	tail	memcpy_any

20:	xor	t0, a0, a1
	andi	t0, t0, 15
	bnez	t0, 0b
	csrr	t0, 0x8b0
	beqz	t0, 0b
	UOP_IN_RAM a0, a2, 0b
	UOP_IN_RAM a1, a2, 0b
	mv	t1, a0
	neg	t2, a0
	andi	t2, t2, 15
	sub	a2, a2, t2
	beqz	t2, 22f
21:	lbu	t3, 0(a1)
	sb	t3, 0(t1)
	addi	a1, a1, 1
	addi	t1, t1, 1
	addi	t2, t2, -1
	bnez	t2, 21b
22:	andi	t2, a2, -16
	csrw	0x8b1, a1
	csrw	0x8b2, t1
	csrw	0x8b3, t2
	li	t0, 1
	csrw	0x8b0, t0
	add	a1, a1, t2
	add	t1, t1, t2
	andi	a2, a2, 15
	beqz	a2, 8b
23:	lbu	t3, 0(a1)
	sb	t3, 0(t1)
	addi	a1, a1, 1
	addi	t1, t1, 1
	addi	a2, a2, -1
	bnez	a2, 23b
	ret
	.size	memcpy, . - memcpy
