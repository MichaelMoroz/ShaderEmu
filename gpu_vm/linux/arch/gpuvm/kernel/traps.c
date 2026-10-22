// SPDX-License-Identifier: GPL-2.0
/*
 * gpu_vm's exceptions (CAUSE 1-7): a fault in user mode is a signal, in the kernel an oops.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/kdebug.h>
#include <linux/module.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/sched/task_stack.h>
#include <linux/signal.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#include <asm/gvm.h>
#include <asm/processor.h>
#include <asm/ptrace.h>

extern asmlinkage void handle_exception(void);

int show_unhandled_signals = 1;

static DEFINE_SPINLOCK(die_lock);

void die(struct pt_regs *regs, const char *str)
{
	static int die_counter;
	int ret;

	oops_enter();

	spin_lock_irq(&die_lock);
	console_verbose();
	bust_spinlocks(1);

	pr_emerg("%s [#%d]\n", str, ++die_counter);
	print_modules();
	show_regs(regs);

	ret = notify_die(DIE_OOPS, str, regs, 0, regs->cause, SIGSEGV);

	bust_spinlocks(0);
	add_taint(TAINT_DIE, LOCKDEP_NOW_UNRELIABLE);
	spin_unlock_irq(&die_lock);
	oops_exit();

	if (in_interrupt())
		panic("Fatal exception in interrupt");
	if (panic_on_oops)
		panic("Fatal exception");
	if (ret != NOTIFY_STOP)
		make_task_dead(SIGSEGV);
}

static void do_trap_error(struct pt_regs *regs, int signo, int code, unsigned long addr, const char *str)
{
	if (user_mode(regs)) {
		if (show_unhandled_signals && unhandled_signal(current, signo) && printk_ratelimit()) {
			pr_info("%s[%d]: unhandled signal %d code 0x%x at 0x%08lx tval 0x%08lx (%s)\n",
				current->comm, task_pid_nr(current), signo, code, regs_epc(regs), regs->tval, str);
			show_regs(regs);
		}
		force_sig_fault(signo, code, (void __user *)addr);
	} else {
		die(regs, str);
	}
}

/* entry.S: every exception but a syscall */
asmlinkage void do_trap(struct pt_regs *regs)
{
	unsigned long epc = regs_epc(regs);

	switch (regs->cause) {
	case GVM_CAUSE_ILLEGAL:
		do_trap_error(regs, SIGILL, ILL_ILLOPC, epc, "illegal instruction");
		break;
	case GVM_CAUSE_BREAK:
		if (user_mode(regs))
			force_sig_fault(SIGTRAP, TRAP_BRKPT, (void __user *)epc);
		else
			die(regs, "kernel break");
		break;
	case GVM_CAUSE_LOAD:
		do_trap_error(regs, SIGSEGV, SEGV_ACCERR, regs->tval, "load fault");
		break;
	case GVM_CAUSE_STORE:
		do_trap_error(regs, SIGSEGV, SEGV_ACCERR, regs->tval, "store fault");
		break;
	case GVM_CAUSE_FETCH:
		do_trap_error(regs, SIGSEGV, SEGV_ACCERR, regs->tval, "fetch fault");
		break;
	case GVM_CAUSE_PRIV:
		do_trap_error(regs, SIGILL, ILL_PRVOPC, epc, "privileged op");
		break;
	default:
		do_trap_error(regs, SIGILL, ILL_ILLTRP, epc, "unknown trap");
		break;
	}
}

/* No frame unwinder: the kernel text addresses found on the stack */
void show_stack(struct task_struct *task, unsigned long *sp, const char *loglvl)
{
	unsigned long *end, addr;

	if (!sp) {
		if (task && task != current)
			sp = (unsigned long *)task->thread.sp;
		else
			sp = (unsigned long *)&sp;
	}
	end = (unsigned long *)(((unsigned long)sp | (THREAD_SIZE - 1)) + 1);

	printk("%sCall Trace:\n", loglvl);
	while (sp < end) {
		addr = *sp++;
		if (__kernel_text_address(addr))
			printk("%s [<%08lx>] %pS\n", loglvl, addr, (void *)addr);
	}
}

void __init trap_init(void)
{
	gvm_mtsr(scratch, 0);
	gvm_mtsr(tvec, (unsigned long)&handle_exception);
}
