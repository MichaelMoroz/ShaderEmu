/* SPDX-License-Identifier: GPL-2.0 */
/*
 * xchg and cmpxchg by the ISA's amoswap and amocas (gpu_vm/ref/gvm_ref.h): a word, which must be aligned. A byte
 * or a half is a cas loop on the word around it.
 *
 * amocas rd, rb, (ra): rd in is the expected word, out what was there; rb the new word, stored when they matched.
 */
#ifndef _ASM_GPUVM_CMPXCHG_H
#define _ASM_GPUVM_CMPXCHG_H

#include <linux/bug.h>
#include <linux/types.h>

static inline u32 __gvm_xchg32(volatile u32 *p, u32 v)
{
	u32 old;

	__asm__ __volatile__("amoswap %0, %2, (%1)" : "=&r"(old) : "r"(p), "r"(v) : "memory");
	return old;
}

static inline u32 __gvm_cmpxchg32(volatile u32 *p, u32 old, u32 new)
{
	__asm__ __volatile__("amocas %0, %2, (%1)" : "+&r"(old) : "r"(p), "r"(new) : "memory");
	return old;
}

/* A byte or a half inside its aligned word */
static inline unsigned long __gvm_cmpxchg_small(volatile void *ptr, unsigned long old, unsigned long new, int size)
{
	unsigned long a = (unsigned long)ptr;
	volatile u32 *w = (volatile u32 *)(a & ~3UL);
	unsigned int shift = (a & 3) * 8;
	u32 mask = (size == 1 ? 0xffU : 0xffffU) << shift;
	u32 cur = *w, want, seen;

	for (;;) {
		if (((cur & mask) >> shift) != (old & (mask >> shift)))
			return (cur & mask) >> shift;
		want = (cur & ~mask) | ((new << shift) & mask);
		seen = __gvm_cmpxchg32(w, cur, want);
		if (seen == cur)
			return old & (mask >> shift);
		cur = seen;
	}
}

static inline unsigned long __gvm_xchg_small(volatile void *ptr, unsigned long new, int size)
{
	unsigned long a = (unsigned long)ptr;
	volatile u32 *w = (volatile u32 *)(a & ~3UL);
	unsigned int shift = (a & 3) * 8;
	u32 mask = (size == 1 ? 0xffU : 0xffffU) << shift;
	u32 cur = *w, seen;

	for (;;) {
		seen = __gvm_cmpxchg32(w, cur, (cur & ~mask) | ((new << shift) & mask));
		if (seen == cur)
			return (cur & mask) >> shift;
		cur = seen;
	}
}

extern void __gvm_bad_size(void);

static __always_inline unsigned long __gvm_xchg(volatile void *ptr, unsigned long new, int size)
{
	switch (size) {
	case 1:
	case 2:
		return __gvm_xchg_small(ptr, new, size);
	case 4:
		return __gvm_xchg32(ptr, new);
	}
	__gvm_bad_size();
	return 0;
}

static __always_inline unsigned long __gvm_cmpxchg(volatile void *ptr, unsigned long old, unsigned long new,
						   int size)
{
	switch (size) {
	case 1:
	case 2:
		return __gvm_cmpxchg_small(ptr, old, new, size);
	case 4:
		return __gvm_cmpxchg32(ptr, old, new);
	}
	__gvm_bad_size();
	return 0;
}

#define arch_xchg(ptr, x)							\
	((__typeof__(*(ptr)))__gvm_xchg((ptr), (unsigned long)(x), sizeof(*(ptr))))

#define arch_cmpxchg(ptr, o, n)							\
	((__typeof__(*(ptr)))__gvm_cmpxchg((ptr), (unsigned long)(o), (unsigned long)(n), sizeof(*(ptr))))

#define arch_cmpxchg_local(ptr, o, n)	arch_cmpxchg((ptr), (o), (n))

#include <asm-generic/cmpxchg-local.h>

#define arch_cmpxchg64_local(ptr, o, n)	__generic_cmpxchg64_local((ptr), (o), (n))
#define arch_cmpxchg64(ptr, o, n)	arch_cmpxchg64_local((ptr), (o), (n))

#endif /* _ASM_GPUVM_CMPXCHG_H */
