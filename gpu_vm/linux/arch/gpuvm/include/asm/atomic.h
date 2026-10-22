/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Atomics by the ISA's amo ops (amoadd, amoand, amoor, amoxor, amoswap, amocas: rd = the old word). An amo is one op,
 * so atomic against this core's interrupts; against other cores only within what the commit makes of a pass (SMP is
 * not done). 64-bit atomics are GENERIC_ATOMIC64's.
 */
#ifndef _ASM_GPUVM_ATOMIC_H
#define _ASM_GPUVM_ATOMIC_H

#include <linux/types.h>
#include <asm/cmpxchg.h>
#include <asm/barrier.h>

#define arch_atomic_read(v)	READ_ONCE((v)->counter)
#define arch_atomic_set(v, i)	WRITE_ONCE(((v)->counter), (i))

#define GVM_ATOMIC_OP(op, amo, c_op)						\
static inline int arch_atomic_fetch_##op(int i, atomic_t *v)			\
{										\
	int old;								\
	__asm__ __volatile__(#amo " %0, %2, (%1)"				\
			     : "=&r"(old) : "r"(&v->counter), "r"(i) : "memory"); \
	return old;								\
}										\
static inline void arch_atomic_##op(int i, atomic_t *v)			\
{										\
	arch_atomic_fetch_##op(i, v);						\
}

GVM_ATOMIC_OP(add, amoadd, +)
GVM_ATOMIC_OP(and, amoand, &)
GVM_ATOMIC_OP(or, amoor, |)
GVM_ATOMIC_OP(xor, amoxor, ^)

#undef GVM_ATOMIC_OP

#define arch_atomic_fetch_add	arch_atomic_fetch_add
#define arch_atomic_fetch_and	arch_atomic_fetch_and
#define arch_atomic_fetch_or	arch_atomic_fetch_or
#define arch_atomic_fetch_xor	arch_atomic_fetch_xor

static inline int arch_atomic_add_return(int i, atomic_t *v)
{
	return arch_atomic_fetch_add(i, v) + i;
}
#define arch_atomic_add_return	arch_atomic_add_return

static inline int arch_atomic_fetch_sub(int i, atomic_t *v)
{
	return arch_atomic_fetch_add(-i, v);
}
#define arch_atomic_fetch_sub	arch_atomic_fetch_sub

static inline void arch_atomic_sub(int i, atomic_t *v)
{
	arch_atomic_fetch_add(-i, v);
}

static inline int arch_atomic_sub_return(int i, atomic_t *v)
{
	return arch_atomic_fetch_add(-i, v) - i;
}
#define arch_atomic_sub_return	arch_atomic_sub_return

#define arch_atomic_xchg(v, n)		arch_xchg(&((v)->counter), (n))
#define arch_atomic_cmpxchg(v, o, n)	arch_cmpxchg(&((v)->counter), (o), (n))

#include <asm-generic/atomic64.h>

#endif /* _ASM_GPUVM_ATOMIC_H */
