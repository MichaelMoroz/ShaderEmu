// SPDX-License-Identifier: GPL-2.0
/*
 * Signals: an rt_sigframe {siginfo, ucontext} on the user's stack; the handler gets a0 = the signal, a1 = &info,
 * a2 = &uc and returns to __user_rt_sigreturn in the kernel's text (no MMU: user mode runs it as it is).
 */
#include <linux/linkage.h>
#include <linux/signal.h>
#include <linux/syscalls.h>
#include <linux/tracehook.h>
#include <linux/uaccess.h>

#include <asm/gvm.h>
#include <asm/ucontext.h>
#include <asm/unistd.h>

extern u32 __user_rt_sigreturn[];

struct rt_sigframe {
	struct siginfo info;
	struct ucontext uc;
};

static long restore_sigcontext(struct pt_regs *regs, struct sigcontext __user *sc)
{
	/* sc_regs is the start of pt_regs: the pc and r1-r63; STATUS stays the kernel's */
	long err = __copy_from_user(regs->gpr, &sc->sc_regs, sizeof(sc->sc_regs));

	regs_epc(regs) &= ~3UL;
	return err;
}

SYSCALL_DEFINE0(rt_sigreturn)
{
	struct pt_regs *regs = current_pt_regs();
	struct rt_sigframe __user *frame;
	sigset_t set;

	current->restart_block.fn = do_no_restart_syscall;

	frame = (struct rt_sigframe __user *)regs_sp(regs);
	if (!access_ok(frame, sizeof(*frame)))
		goto badframe;
	if (__copy_from_user(&set, &frame->uc.uc_sigmask, sizeof(set)))
		goto badframe;
	set_current_blocked(&set);
	if (restore_sigcontext(regs, &frame->uc.uc_mcontext))
		goto badframe;
	if (restore_altstack(&frame->uc.uc_stack))
		goto badframe;
	return regs_a(regs, 0);

badframe:
	force_sig(SIGSEGV);
	return 0;
}

static void __user *get_sigframe(struct ksignal *ksig, struct pt_regs *regs, size_t framesize)
{
	unsigned long sp = regs_sp(regs);

	if (on_sig_stack(sp) && !likely(on_sig_stack(sp - framesize)))
		return (void __user __force *)(-1UL);
	sp = sigsp(sp, ksig) - framesize;
	sp &= ~0xfUL;
	return (void __user *)sp;
}

static int setup_rt_frame(struct ksignal *ksig, sigset_t *set, struct pt_regs *regs)
{
	struct rt_sigframe __user *frame = get_sigframe(ksig, regs, sizeof(*frame));
	long err = 0;

	if (!access_ok(frame, sizeof(*frame)))
		return -EFAULT;

	err |= copy_siginfo_to_user(&frame->info, &ksig->info);
	err |= __put_user(0, &frame->uc.uc_flags);
	err |= __put_user(NULL, &frame->uc.uc_link);
	err |= __save_altstack(&frame->uc.uc_stack, regs_sp(regs));
	err |= __copy_to_user(&frame->uc.uc_mcontext.sc_regs, regs->gpr, sizeof(frame->uc.uc_mcontext.sc_regs));
	err |= __copy_to_user(&frame->uc.uc_sigmask, set, sizeof(*set));
	if (err)
		return -EFAULT;

	regs_ra(regs) = (unsigned long)__user_rt_sigreturn;
	regs_epc(regs) = (unsigned long)ksig->ka.sa.sa_handler;
	regs_sp(regs) = (unsigned long)frame;
	regs_a(regs, 0) = ksig->sig;
	regs_a(regs, 1) = (unsigned long)&frame->info;
	regs_a(regs, 2) = (unsigned long)&frame->uc;
	return 0;
}

/* A syscall's EPC is past its op (entry.S): 4 back runs it again */
static void handle_signal(struct ksignal *ksig, struct pt_regs *regs)
{
	sigset_t *oldset = sigmask_to_save();

	if (regs->cause == GVM_CAUSE_SYSCALL) {
		regs->cause = -1UL;	/* no second restart */
		switch (regs_a(regs, 0)) {
		case -ERESTART_RESTARTBLOCK:
		case -ERESTARTNOHAND:
			regs_a(regs, 0) = -EINTR;
			break;
		case -ERESTARTSYS:
			if (!(ksig->ka.sa.sa_flags & SA_RESTART)) {
				regs_a(regs, 0) = -EINTR;
				break;
			}
			fallthrough;
		case -ERESTARTNOINTR:
			regs_a(regs, 0) = regs->orig_a0;
			regs_epc(regs) -= 4;
			break;
		}
	}
	signal_setup_done(setup_rt_frame(ksig, oldset, regs), ksig, 0);
}

static void do_signal(struct pt_regs *regs)
{
	struct ksignal ksig;

	if (get_signal(&ksig)) {
		handle_signal(&ksig, regs);
		return;
	}

	if (regs->cause == GVM_CAUSE_SYSCALL) {
		regs->cause = -1UL;
		switch (regs_a(regs, 0)) {
		case -ERESTARTNOHAND:
		case -ERESTARTSYS:
		case -ERESTARTNOINTR:
			regs_a(regs, 0) = regs->orig_a0;
			regs_epc(regs) -= 4;
			break;
		case -ERESTART_RESTARTBLOCK:
			regs_a(regs, 0) = regs->orig_a0;
			regs->gpr[GVM_REG_A7] = __NR_restart_syscall;
			regs_epc(regs) -= 4;
			break;
		}
	}
	restore_saved_sigmask();
}

asmlinkage __visible void do_notify_resume(struct pt_regs *regs, unsigned long thread_info_flags)
{
	if (thread_info_flags & (_TIF_SIGPENDING | _TIF_NOTIFY_SIGNAL))
		do_signal(regs);
	if (thread_info_flags & _TIF_NOTIFY_RESUME)
		tracehook_notify_resume(regs);
}
