/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GPUVM_PROCESSOR_H
#define _ASM_GPUVM_PROCESSOR_H

#include <linux/const.h>
#include <asm/ptrace.h>

/* No MMU: one address space; a flat program's stack is placed by binfmt_flat */
#define TASK_SIZE		0xffffffffUL
#define STACK_TOP		TASK_SIZE
#define STACK_TOP_MAX		STACK_TOP
#define TASK_UNMAPPED_BASE	0

#ifndef __ASSEMBLY__

struct task_struct;

/* What __switch_to keeps of a task: the callee-saved registers (gpu_vm's ABI) */
struct thread_struct {
	unsigned long s[24];	/* s0-s23: r32-r55 */
	unsigned long ra;
	unsigned long sp;	/* the kernel's */
};

#define INIT_THREAD {					\
	.sp = sizeof(init_stack) + (long)&init_stack,	\
}

#define task_pt_regs(tsk)						\
	((struct pt_regs *)(task_stack_page(tsk) + THREAD_SIZE) - 1)

#define KSTK_EIP(tsk)		(regs_epc(task_pt_regs(tsk)))
#define KSTK_ESP(tsk)		(regs_sp(task_pt_regs(tsk)))

static inline void release_thread(struct task_struct *dead_task)
{
}

extern void start_thread(struct pt_regs *regs, unsigned long pc, unsigned long sp);
extern unsigned long __get_wchan(struct task_struct *p);

#define cpu_relax()		barrier()

#endif /* __ASSEMBLY__ */

#endif /* _ASM_GPUVM_PROCESSOR_H */
