// SPDX-License-Identifier: GPL-2.0
/* The offsets entry.S, head.S and switch_to use */
#define GENERATING_ASM_OFFSETS

#include <linux/kbuild.h>
#include <linux/sched.h>
#include <linux/thread_info.h>
#include <asm/ptrace.h>
#include <asm/processor.h>

void asm_offsets(void);

void asm_offsets(void)
{
	OFFSET(TASK_TI_FLAGS, task_struct, thread_info.flags);
	OFFSET(TASK_TI_PREEMPT_COUNT, task_struct, thread_info.preempt_count);
	OFFSET(TASK_TI_KERNEL_SP, task_struct, thread_info.kernel_sp);
	OFFSET(TASK_TI_USER_SP, task_struct, thread_info.user_sp);
	OFFSET(TASK_THREAD, task_struct, thread);

	OFFSET(THREAD_S0, thread_struct, s);
	OFFSET(THREAD_RA, thread_struct, ra);
	OFFSET(THREAD_SP, thread_struct, sp);

	OFFSET(PT_EPC, pt_regs, gpr[GVM_REG_PC]);
	OFFSET(PT_RA, pt_regs, gpr[GVM_REG_RA]);
	OFFSET(PT_SP, pt_regs, gpr[GVM_REG_SP]);
	OFFSET(PT_GP, pt_regs, gpr[GVM_REG_GP]);
	OFFSET(PT_TP, pt_regs, gpr[GVM_REG_TP]);
	OFFSET(PT_A0, pt_regs, gpr[GVM_REG_A0]);
	OFFSET(PT_STATUS, pt_regs, status);
	OFFSET(PT_CAUSE, pt_regs, cause);
	OFFSET(PT_TVAL, pt_regs, tval);
	OFFSET(PT_ORIG_A0, pt_regs, orig_a0);
	DEFINE(PT_SIZE, sizeof(struct pt_regs));
	DEFINE(PT_SIZE_ON_STACK, ALIGN(sizeof(struct pt_regs), 16));
}
