// SPDX-License-Identifier: GPL-2.0
/* ptrace: the registers as one regset (NT_PRSTATUS: user_regs_struct), and the syscall trace hooks */
#include <linux/elf.h>
#include <linux/ptrace.h>
#include <linux/regset.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/tracehook.h>

#include <asm/ptrace.h>

enum gvm_regset {
	REGSET_GPR,
};

static int gvm_gpr_get(struct task_struct *target, const struct user_regset *regset, struct membuf to)
{
	return membuf_write(&to, task_pt_regs(target)->gpr, sizeof(struct user_regs_struct));
}

static int gvm_gpr_set(struct task_struct *target, const struct user_regset *regset,
		       unsigned int pos, unsigned int count, const void *kbuf, const void __user *ubuf)
{
	struct user_regs_struct regs;
	int ret;

	memcpy(&regs, task_pt_regs(target)->gpr, sizeof(regs));
	ret = user_regset_copyin(&pos, &count, &kbuf, &ubuf, &regs, 0, -1);
	if (!ret) {
		regs.gpr[0] &= ~3UL;	/* a pc is a word of a bundle */
		memcpy(task_pt_regs(target)->gpr, &regs, sizeof(regs));
	}
	return ret;
}

static const struct user_regset gvm_regsets[] = {
	[REGSET_GPR] = {
		.core_note_type = NT_PRSTATUS,
		.n = ELF_NGREG,
		.size = sizeof(elf_greg_t),
		.align = sizeof(elf_greg_t),
		.regset_get = gvm_gpr_get,
		.set = gvm_gpr_set,
	},
};

static const struct user_regset_view gvm_user_view = {
	.name = "gpuvm",
	.e_machine = EM_GVM,
	.regsets = gvm_regsets,
	.n = ARRAY_SIZE(gvm_regsets),
};

const struct user_regset_view *task_user_regset_view(struct task_struct *task)
{
	return &gvm_user_view;
}

void ptrace_disable(struct task_struct *child)
{
}

long arch_ptrace(struct task_struct *child, long request, unsigned long addr, unsigned long data)
{
	return ptrace_request(child, request, addr, data);
}

/* entry.S: nonzero skips the call */
asmlinkage __visible int do_syscall_trace_enter(struct pt_regs *regs)
{
	if (test_thread_flag(TIF_SYSCALL_TRACE))
		if (tracehook_report_syscall_entry(regs))
			return -1;
	return 0;
}

asmlinkage __visible void do_syscall_trace_exit(struct pt_regs *regs)
{
	if (test_thread_flag(TIF_SYSCALL_TRACE))
		tracehook_report_syscall_exit(regs, 0);
}
