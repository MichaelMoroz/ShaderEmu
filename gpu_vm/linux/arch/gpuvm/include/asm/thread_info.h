/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GPUVM_THREAD_INFO_H
#define _ASM_GPUVM_THREAD_INFO_H

#include <asm/page.h>
#include <linux/const.h>

/* 8 KB of kernel stack a thread */
#define THREAD_SIZE_ORDER	1
#define THREAD_SIZE		(PAGE_SIZE << THREAD_SIZE_ORDER)
#define THREAD_SHIFT		(PAGE_SHIFT + THREAD_SIZE_ORDER)

#ifndef __ASSEMBLY__

/*
 * At offset 0 of task_struct (THREAD_INFO_IN_TASK): tp points at both. entry.S keeps the stack pointers here: the
 * user's while in the kernel, the top of the kernel stack while in user mode.
 */
struct thread_info {
	unsigned long		flags;
	int			preempt_count;
	long			kernel_sp;
	long			user_sp;
	int			cpu;
};

#define INIT_THREAD_INFO(tsk)			\
{						\
	.flags		= 0,			\
	.preempt_count	= INIT_PREEMPT_COUNT,	\
}

#endif /* !__ASSEMBLY__ */

#define TIF_SYSCALL_TRACE	0	/* syscall trace active */
#define TIF_NOTIFY_RESUME	1	/* callback before returning to user */
#define TIF_SIGPENDING		2	/* signal pending */
#define TIF_NEED_RESCHED	3	/* rescheduling necessary */
#define TIF_RESTORE_SIGMASK	4	/* restore signal mask in do_signal() */
#define TIF_MEMDIE		5	/* is terminating due to OOM killer */
#define TIF_NOTIFY_SIGNAL	6	/* signal notifications exist */
#define TIF_POLLING_NRFLAG	7	/* idle polls TIF_NEED_RESCHED */

#define _TIF_SYSCALL_TRACE	(1 << TIF_SYSCALL_TRACE)
#define _TIF_NOTIFY_RESUME	(1 << TIF_NOTIFY_RESUME)
#define _TIF_SIGPENDING		(1 << TIF_SIGPENDING)
#define _TIF_NEED_RESCHED	(1 << TIF_NEED_RESCHED)
#define _TIF_NOTIFY_SIGNAL	(1 << TIF_NOTIFY_SIGNAL)
#define _TIF_POLLING_NRFLAG	(1 << TIF_POLLING_NRFLAG)

#define _TIF_WORK_MASK \
	(_TIF_NOTIFY_RESUME | _TIF_SIGPENDING | _TIF_NEED_RESCHED | _TIF_NOTIFY_SIGNAL)

#define _TIF_SYSCALL_WORK	_TIF_SYSCALL_TRACE

#endif /* _ASM_GPUVM_THREAD_INFO_H */
