/* SPDX-License-Identifier: GPL-2.0 */
/*
 * memcpy, memmove, memset. From GVM_OP_MIN bytes up (destination in device memory or RAM): the commit's operations
 * (gpu_vm/GPU_VM.md, "Operations"): one sq of the descriptor texel {kind, dst, src or word, bytes} to GVM_OPS, and
 * the function returns at once. Several may be in flight; the commit makes them in the order asked for, from what the
 * core's pass left (its own stores included). Until one is made, a load of what it writes or a store to what it writes
 * or reads ends the core's pass, and the op runs again after it. Code an operation wrote runs after a sync
 * (flush_icache_range). One sq: the kind's word asks for the operation, so it never starts half written.
 * Below GVM_OP_MIN: loops of 16-byte loads and stores (lq/sq: r16-r19), then bytes.
 */
#include <linux/linkage.h>
#include <asm/gvm.h>
#include <asm/string.h>

#define OP_COPY	1
#define OP_FILL	2

	.text
/* void *memcpy(void *dst a0, const void *src a1, size_t n a2) */
ENTRY(memcpy)
ENTRY(__memcpy)
.Lmemcpy:
	li t0, GVM_OP_MIN
	bltu a2, t0, .Lcopy_loop
	li t0, GVM_DEV
	bltu a0, t0, .Lcopy_loop
	li r16, OP_COPY
	mv r17, a0
	mv r18, a1
	mv r19, a2
	sq r16, GVM_OPS(zero)
	ret
.Lcopy_loop:
	mv t1, a0
	srli t2, a2, 4
	beqz t2, 2f
1:	lq r16, 0(a1)
	sq r16, 0(t1)
	addi a1, a1, 16
	addi t1, t1, 16
	addbnz t2, -1, 1b
2:	andi a2, a2, 15
	beqz a2, .Lcopy_done
3:	lbu t3, 0(a1)
	sb t3, 0(t1)
	addi a1, a1, 1
	addi t1, t1, 1
	addbnz a2, -1, 3b
.Lcopy_done:
	ret
END(__memcpy)
END(memcpy)

/* void *memmove(void *dst a0, const void *src a1, size_t n a2) */
ENTRY(memmove)
ENTRY(__memmove)
	/* forward unless dst lies inside (src, src + n): the copy operation is a memmove itself */
	sub t0, a0, a1
	bgeu t0, a2, .Lmemcpy
	li t0, GVM_OP_MIN
	bgeu a2, t0, .Lmemcpy
	add a1, a1, a2
	add t1, a0, a2
	andi t2, a2, 15
	beqz t2, 2f
1:	addi a1, a1, -1
	addi t1, t1, -1
	lbu t3, 0(a1)
	sb t3, 0(t1)
	addbnz t2, -1, 1b
2:	srli t2, a2, 4
	beqz t2, 4f
3:	addi a1, a1, -16
	addi t1, t1, -16
	lq r16, 0(a1)
	sq r16, 0(t1)
	addbnz t2, -1, 3b
4:	ret
END(__memmove)
END(memmove)

/* void *memset(void *dst a0, int c a1, size_t n a2) */
ENTRY(memset)
ENTRY(__memset)
	andi a1, a1, 255
	muli a1, a1, 0x01010101
	li t0, GVM_OP_MIN
	bltu a2, t0, .Lset_loop
	li t0, GVM_DEV
	bltu a0, t0, .Lset_loop
	li r16, OP_FILL
	mv r17, a0
	mv r18, a1
	mv r19, a2
	sq r16, GVM_OPS(zero)
	ret
.Lset_loop:
	mv t1, a0
	mv r16, a1
	mv r17, a1
	mv r18, a1
	mv r19, a1
	srli t2, a2, 4
	beqz t2, 2f
1:	sq r16, 0(t1)
	addi t1, t1, 16
	addbnz t2, -1, 1b
2:	andi a2, a2, 15
	beqz a2, .Lset_done
3:	sb a1, 0(t1)
	addi t1, t1, 1
	addbnz a2, -1, 3b
.Lset_done:
	ret
END(__memset)
END(memset)
