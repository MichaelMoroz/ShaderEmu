/*
 * The commit's copy and fill as a program asks for them (memcpy.S, memset.S; docs/machine/gpu.md,
 * "A program's copy"): CSRs 0x8b1 source (a fill's word), 0x8b2 destination, 0x8b3 length,
 * then 0x8b0 1 a copy or 2 a fill. Whole texels: the destination and the length on 16 bytes, a
 * copy's source at the destination's place in a texel.
 */
/* memcpy's least (memset.c has its own, 2 KB): on the card a copy that is the commit's beat sixteen words a turn from
   4 KB up (8 sources to 4 destinations in turn: 3.8 MB/s against 2.5 at 4 KB, 12.0 against 2.4 at 16 KB) and lost at
   2 KB (2.1 against 2.6) */
#define UOP_MIN 4096

/* to FAIL unless [REG, REG + LEN) lies in the GPU's memory, 0x80000000 to its end at 0x81e00000,
   or in RAM, 0x82000000 to 0x92000000 (not the device words between). Uses t4 to t6. */
.macro UOP_IN_RAM reg, len, fail
	li	t4, 0x80000000
	sub	t5, \reg, t4
	add	t6, t5, \len
	bltu	t6, t5, \fail
	li	t4, 0x01e00000
	bgeu	t5, t4, 91f
	bgtu	t6, t4, \fail
	j	92f
91:	li	t4, 0x02000000
	bltu	t5, t4, \fail
	li	t4, 0x12000000
	bgtu	t6, t4, \fail
92:
.endm
