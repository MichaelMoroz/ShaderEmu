/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GPUVM_BARRIER_H
#define _ASM_GPUVM_BARRIER_H

#ifndef __ASSEMBLY__

/*
 * A core sees its own loads and stores in program order, and the others' only after a commit (the end of a pass):
 * fence orders a core's own memory ops (gvm_ref.h makes it nothing). Between cores a barrier is not enough (a sync
 * is): SMP is not done.
 */
#define mb()		__asm__ __volatile__("fence" : : : "memory")
#define rmb()		mb()
#define wmb()		mb()

#include <asm-generic/barrier.h>

#endif /* __ASSEMBLY__ */

#endif /* _ASM_GPUVM_BARRIER_H */
