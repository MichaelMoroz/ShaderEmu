/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GPUVM_CURRENT_H
#define _ASM_GPUVM_CURRENT_H

#ifndef __ASSEMBLY__

#include <linux/compiler.h>

struct task_struct;

/*
 * In the kernel tp (r4, reserved by the ABI) is the current task (its thread_info is at offset 0); SCRATCH holds it
 * while the core is in user mode. Read by an op, not a global register variable: nothing asks the compiler for
 * getRegisterByName.
 */
static __always_inline struct task_struct *get_current(void)
{
	struct task_struct *t;

	__asm__ ("mv %0, tp" : "=r"(t));
	return t;
}

#define current get_current()

#endif /* __ASSEMBLY__ */

#endif /* _ASM_GPUVM_CURRENT_H */
