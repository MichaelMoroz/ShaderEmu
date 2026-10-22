/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GPUVM_SYSCALL_H
#define _ASM_GPUVM_SYSCALL_H

#include <linux/audit.h>
#include <linux/elf-em.h>
#include <linux/err.h>
#include <linux/sched.h>
#include <asm/ptrace.h>

#define AUDIT_ARCH_GPUVM	(EM_GVM | __AUDIT_ARCH_LE)

extern void * const sys_call_table[];

static inline int syscall_get_nr(struct task_struct *task, struct pt_regs *regs)
{
	return regs->gpr[GVM_REG_A7];
}

static inline void syscall_rollback(struct task_struct *task, struct pt_regs *regs)
{
	regs_a(regs, 0) = regs->orig_a0;
}

static inline long syscall_get_error(struct task_struct *task, struct pt_regs *regs)
{
	unsigned long error = regs_a(regs, 0);

	return IS_ERR_VALUE(error) ? error : 0;
}

static inline long syscall_get_return_value(struct task_struct *task, struct pt_regs *regs)
{
	return regs_a(regs, 0);
}

static inline void syscall_set_return_value(struct task_struct *task, struct pt_regs *regs,
					    int error, long val)
{
	regs_a(regs, 0) = (long)error ?: val;
}

static inline void syscall_get_arguments(struct task_struct *task, struct pt_regs *regs,
					 unsigned long *args)
{
	int i;

	args[0] = regs->orig_a0;
	for (i = 1; i < 6; i++)
		args[i] = regs_a(regs, i);
}

static inline int syscall_get_arch(struct task_struct *task)
{
	return AUDIT_ARCH_GPUVM;
}

#endif /* _ASM_GPUVM_SYSCALL_H */
