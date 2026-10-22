// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/uaccess.h>

#include <asm/gvm.h>
#include <asm/processor.h>
#include <asm/switch_to.h>

extern asmlinkage void ret_from_fork(void);
extern asmlinkage void ret_from_kernel_thread(void);

/*
 * Called with interrupts off; returns with them on. wait looks at IPEND & IEN whatever IE is, so an interrupt that
 * comes before it is not lost; it is taken once IE is set.
 */
void arch_cpu_idle(void)
{
	__asm__ __volatile__("wait" : : : "memory");
	raw_local_irq_enable();
}

void __show_regs(struct pt_regs *regs)
{
	int i;

	show_regs_print_info(KERN_DEFAULT);
	pr_cont("epc: %08lx ra: %08lx sp: %08lx status: %08lx cause: %08lx tval: %08lx\n",
		regs_epc(regs), regs_ra(regs), regs_sp(regs), regs->status, regs->cause, regs->tval);
	if (!user_mode(regs))
		pr_cont("epc: %pS\n ra: %pS\n", (void *)regs_epc(regs), (void *)regs_ra(regs));
	for (i = 4; i < GVM_NGPR; i += 4)
		pr_cont("r%-2d: %08lx %08lx %08lx %08lx\n", i, regs->gpr[i], regs->gpr[i + 1],
			regs->gpr[i + 2], regs->gpr[i + 3]);
}

void show_regs(struct pt_regs *regs)
{
	__show_regs(regs);
	if (!user_mode(regs))
		show_stack(NULL, (unsigned long *)regs_sp(regs), KERN_DEFAULT);
}

/* A flat program's start (binfmt_flat): user mode, interrupts on after the eret */
void start_thread(struct pt_regs *regs, unsigned long pc, unsigned long sp)
{
	regs->status = GVM_ST_PIE;
	regs_epc(regs) = pc;
	regs_sp(regs) = sp;
}

void flush_thread(void)
{
}

int copy_thread(unsigned long clone_flags, unsigned long usp, unsigned long arg,
		struct task_struct *p, unsigned long tls)
{
	struct pt_regs *childregs = task_pt_regs(p);

	memset(&p->thread, 0, sizeof(p->thread));
	if (unlikely(p->flags & (PF_KTHREAD | PF_IO_WORKER))) {
		/* a kernel thread: fn(arg) from ret_from_kernel_thread, in the supervisor's mode */
		memset(childregs, 0, sizeof(*childregs));
		childregs->status = GVM_ST_PS | GVM_ST_PIE;
		p->thread.ra = (unsigned long)ret_from_kernel_thread;
		p->thread.s[0] = usp;	/* fn */
		p->thread.s[1] = arg;
	} else {
		*childregs = *current_pt_regs();
		if (usp)
			regs_sp(childregs) = usp;
		if (clone_flags & CLONE_SETTLS)
			regs_tp(childregs) = tls;
		regs_a(childregs, 0) = 0;	/* the child's fork() */
		p->thread.ra = (unsigned long)ret_from_fork;
	}
	p->thread.sp = (unsigned long)childregs;
	return 0;
}

unsigned long __get_wchan(struct task_struct *task)
{
	return 0;
}
