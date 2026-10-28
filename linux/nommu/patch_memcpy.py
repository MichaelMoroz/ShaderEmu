"""No MMU: the kernel's memcpy without its byte loops for misaligned copies, and the parallel copy for any large copy
whose two ends lie alike in a texel (docs/plans/riscv-profile.md, items 3 and 4; the plan's section 2, item 4).

arch/riscv/lib/memcpy.S as memcpy_hook.py leaves it copies a byte at a time, 5 instructions and a store a byte,
whenever source and destination differ in their low two bits (read() into a buffer that lies otherwise than the page
cache: 6.7% of Quake's timedemo, 3.6% of Doom's attract loop), and sends to the commit's parallel copy only copies of
1 KB or more with both ends and the length on a texel, so a large copy that is merely co-aligned took the 64-byte word
loop (211 of Doom's 888 passes ended on a full write cache).

Here:
- 1 KB or more, both in the linear map, source and destination at one place in a texel (SE_BULK_MASK): bytes up to
  the destination's texel, the parallel copy for the whole texels, bytes after;
- otherwise co-aligned to a word: bytes to the word, 64 bytes a step, words, bytes (as before);
- otherwise: bytes to the destination's word, then words made of two aligned source words shifted together, eight
  bytes a step (13 instructions and two stores where it was 40 and eight), bytes after. A source word is read whole
  where the copy needs only part of it: within one word, never past a page.
   python3 patch_memcpy.py K"""
import sys
K = sys.argv[1]
p = K + '/arch/riscv/lib/memcpy.S'
s = open(p).read()
MARK = 'shaderemu: memcpy without byte loops'
if MARK not in s:
    assert 'ShaderEmu: a large, texel-aligned copy' in s and 'ShaderEmu: both ends on a texel' in s, \
        'memcpy.S is not memcpy_hook.py\'s'
    s = r'''/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2013 Regents of the University of California
 *
 * ''' + MARK + r''' (linux/nommu/patch_memcpy.py)
 */

#include <linux/linkage.h>
#include <asm/asm.h>
#include <asm/page.h>

#define SE_BULK_MASK 15		/* the parallel copy's ends: at one place in a texel */

/* void *memcpy(void *, const void *, size_t) */
ENTRY(__memcpy)
WEAK(memcpy)
	move t6, a0		/* the destination as it goes; a0 is returned */
	/*
	 * A large copy within the linear map whose ends lie alike in a texel: one parallel operation of the
	 * machine's commit (CSRs 0x0b1-0x0b3: source, destination, length; 1 to 0x0b0 runs it) for its whole
	 * texels, bytes on either side.
	 */
	li t0, 1024
	bltu a2, t0, 20f
	li t0, PAGE_OFFSET
	bltu a0, t0, 20f
	bltu a1, t0, 20f
	xor t1, a0, a1
	andi t1, t1, SE_BULK_MASK
	bnez t1, 20f
	add a3, a0, a2		/* the end */
	andi t1, a0, SE_BULK_MASK
	beqz t1, 2f
	addi t2, a0, SE_BULK_MASK
	andi t2, t2, ~SE_BULK_MASK	/* the first texel */
1:
	lbu a5, 0(a1)
	addi a1, a1, 1
	sb a5, 0(t6)
	addi t6, t6, 1
	bltu t6, t2, 1b
2:
	andi t2, a3, ~SE_BULK_MASK	/* the end of the last whole texel */
	sub t1, t2, t6
	csrw 0x0b1, a1
	csrw 0x0b2, t6
	csrw 0x0b3, t1
	li t0, 1
	csrw 0x0b0, t0
	add a1, a1, t1
	move t6, t2
	sub a2, a3, t6
	j 80f

20:
	/* Small copies a byte at a time */
	sltiu a3, a2, 16
	bnez a3, 80f
	xor a3, t6, a1
	andi a3, a3, 3
	bnez a3, 60f

	/* Co-aligned to a word: bytes to the word */
	andi a3, a1, 3
	beqz a3, 32f
	andi a4, a1, ~3
	addi a4, a4, 4
	sub a5, a4, a1
	sub a2, a2, a5
31:
	lbu a5, 0(a1)
	addi a1, a1, 1
	sb a5, 0(t6)
	addi t6, t6, 1
	bltu a1, a4, 31b
32:
	/* 64 bytes a step */
	andi a4, a2, ~63
	beqz a4, 40f
	add a3, a1, a4
33:
	lw a4,  0(a1)
	lw a5,  4(a1)
	lw a6,  8(a1)
	lw a7, 12(a1)
	lw t0, 16(a1)
	lw t1, 20(a1)
	lw t2, 24(a1)
	lw t3, 28(a1)
	lw t4, 32(a1)
	lw t5, 36(a1)
	sw a4,  0(t6)
	sw a5,  4(t6)
	sw a6,  8(t6)
	sw a7, 12(t6)
	sw t0, 16(t6)
	sw t1, 20(t6)
	sw t2, 24(t6)
	sw t3, 28(t6)
	sw t4, 32(t6)
	sw t5, 36(t6)
	lw a4, 40(a1)
	lw a5, 44(a1)
	lw a6, 48(a1)
	lw a7, 52(a1)
	lw t0, 56(a1)
	lw t1, 60(a1)
	addi a1, a1, 64
	sw a4, 40(t6)
	sw a5, 44(t6)
	sw a6, 48(t6)
	sw a7, 52(t6)
	sw t0, 56(t6)
	sw t1, 60(t6)
	addi t6, t6, 64
	bltu a1, a3, 33b
	andi a2, a2, 63
40:
	/* words */
	andi a4, a2, ~3
	beqz a4, 80f
	add a3, a1, a4
41:
	lw a4, 0(a1)
	addi a1, a1, 4
	sw a4, 0(t6)
	addi t6, t6, 4
	bltu a1, a3, 41b
	andi a2, a2, 3
	j 80f

60:
	/* Misaligned: bytes to the destination's word */
	andi a3, t6, 3
	beqz a3, 62f
	sub a3, x0, t6
	andi a3, a3, 3		/* bytes to the word */
	sub a2, a2, a3
	add a4, t6, a3
61:
	lbu a5, 0(a1)
	addi a1, a1, 1
	sb a5, 0(t6)
	addi t6, t6, 1
	bltu t6, a4, 61b
62:
	/* then two destination words a step, each of two aligned source words shifted together */
	andi a4, a2, ~7
	beqz a4, 80f
	andi a6, a1, 3
	slli a6, a6, 3		/* the source's offset in bits */
	li a7, 32
	sub a7, a7, a6		/* and what is left of a word */
	andi t0, a1, ~3		/* the source's word */
	add a3, t6, a4		/* the destination's end of this part */
	lw t1, 0(t0)
63:
	lw t2, 4(t0)
	srl a4, t1, a6
	sll a5, t2, a7
	or a4, a4, a5
	sw a4, 0(t6)
	lw t1, 8(t0)
	srl a4, t2, a6
	sll a5, t1, a7
	or a4, a4, a5
	sw a4, 4(t6)
	addi t0, t0, 8
	addi t6, t6, 8
	bltu t6, a3, 63b
	andi a5, a2, ~7
	add a1, a1, a5
	andi a2, a2, 7

80:
	/* bytes */
	beqz a2, 82f
	add a3, a1, a2
81:
	lbu a4, 0(a1)
	addi a1, a1, 1
	sb a4, 0(t6)
	addi t6, t6, 1
	bltu a1, a3, 81b
82:
	ret
END(__memcpy)
'''
    open(p, 'w').write(s)
print('memcpy: no byte loops for misaligned copies, the parallel copy for texel-alike ends')
