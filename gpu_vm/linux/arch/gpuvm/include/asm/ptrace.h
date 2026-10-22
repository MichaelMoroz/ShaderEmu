/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GPUVM_PTRACE_H
#define _ASM_GPUVM_PTRACE_H

#include <uapi/asm/ptrace.h>
#include <asm/gvm.h>

#ifndef __ASSEMBLY__

/*
 * What entry.S saves on the kernel stack: gpr[n] is rn, gpr[0] the pc (EPC; r0 reads 0), then the trap's special
 * registers. The first 64 words are user_regs_struct. 68 words: 16-byte aligned, as the stack.
 */
struct pt_regs {
	unsigned long gpr[GVM_NGPR];
	unsigned long status;
	unsigned long cause;
	unsigned long tval;
	unsigned long orig_a0;	/* a0 as the syscall found it (a restart) */
};

#define PT_REG(regs, n)		((regs)->gpr[n])
#define regs_epc(regs)		((regs)->gpr[GVM_REG_PC])
#define regs_ra(regs)		((regs)->gpr[GVM_REG_RA])
#define regs_sp(regs)		((regs)->gpr[GVM_REG_SP])
#define regs_tp(regs)		((regs)->gpr[GVM_REG_TP])
#define regs_a(regs, n)		((regs)->gpr[GVM_REG_A0 + (n)])

#define user_mode(regs)		(((regs)->status & GVM_ST_PS) == 0)

static inline unsigned long instruction_pointer(struct pt_regs *regs)
{
	return regs_epc(regs);
}

static inline void instruction_pointer_set(struct pt_regs *regs, unsigned long val)
{
	regs_epc(regs) = val;
}

#define profile_pc(regs)	instruction_pointer(regs)

static inline unsigned long user_stack_pointer(struct pt_regs *regs)
{
	return regs_sp(regs);
}

static inline void user_stack_pointer_set(struct pt_regs *regs, unsigned long val)
{
	regs_sp(regs) = val;
}

static inline unsigned long kernel_stack_pointer(struct pt_regs *regs)
{
	return regs_sp(regs);
}

static inline unsigned long regs_return_value(struct pt_regs *regs)
{
	return regs_a(regs, 0);
}

static inline void regs_set_return_value(struct pt_regs *regs, unsigned long val)
{
	regs_a(regs, 0) = val;
}

#define current_pt_regs()	task_pt_regs(current)

#endif /* __ASSEMBLY__ */

#endif /* _ASM_GPUVM_PTRACE_H */
