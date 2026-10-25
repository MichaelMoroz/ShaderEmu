/* SPDX-License-Identifier: GPL-2.0 */
/*
 * gpu_vm's trap entry and exit, ret_from_fork and __switch_to (gpu_vm/llvm/ASM.md's syntax).
 *
 * A trap (gvm_ref.h's gvm__trap): EPC = where to resume (a syscall's op itself: the handler returns past it, EPC + 4),
 * CAUSE, TVAL; PIE = IE, IE = 0, PS = S, S = 1; pc = TVEC. eret: pc = EPC, IE = PIE, S = PS.
 *
 * SCRATCH is 0 while the core is in the kernel and the current task while it is in user mode, so the vector finds
 * the task (and through its thread_info the kernel stack) with one xsr. In the kernel tp is the current task.
 *
 * pt_regs: gpr[n] at 4 * n is rn, gpr[0] the pc; r4-r63 are saved and restored four at a time (sq, lq: 16 bytes,
 * the stack is 16-byte aligned). Calls out of this file are `la t0, sym; callr t0` (one bundle; reaches any address,
 * where call reaches +-2 MB).
 */
#include <linux/init.h>
#include <linux/linkage.h>
#include <asm/asm-offsets.h>
#include <asm/thread_info.h>
#include <asm/unistd.h>
#include <asm/gvm.h>

#define FAR_CALL(sym)	la t0, sym; callr t0
#define FAR_JUMP(sym)	la t0, sym; jr t0

	.text
ENTRY(handle_exception)
	/* From user mode SCRATCH is the task: tp = task, SCRATCH = the user's tp. From the kernel it is 0. */
	xsr tp, scratch, tp
	bnez tp, .Lsave_context
	mfsr tp, scratch		/* the kernel's tp back; SCRATCH holds it too until cleared below */
	sw sp, TASK_TI_KERNEL_SP(tp)
.Lsave_context:
	sw sp, TASK_TI_USER_SP(tp)
	lw sp, TASK_TI_KERNEL_SP(tp)
	addi sp, sp, -PT_SIZE_ON_STACK
	sw r1, 4(sp)
	sw r3, 12(sp)
	sq r4, 16(sp)			/* r4 (tp) is the task here: its slot is written again below */
	sq r8, 32(sp)
	sq r12, 48(sp)
	sq r16, 64(sp)
	sq r20, 80(sp)
	sq r24, 96(sp)
	sq r28, 112(sp)
	sq r32, 128(sp)
	sq r36, 144(sp)
	sq r40, 160(sp)
	sq r44, 176(sp)
	sq r48, 192(sp)
	sq r52, 208(sp)
	sq r56, 224(sp)
	sq r60, 240(sp)

	lw s0, TASK_TI_USER_SP(tp)
	mfsr s1, status
	mfsr s2, epc
	mfsr s3, tval
	mfsr s4, cause
	xsr s5, scratch, zero		/* the interrupted tp; SCRATCH = 0: a trap from here on is the kernel's */
	sw s0, PT_SP(sp)
	sw s1, PT_STATUS(sp)
	sw s2, PT_EPC(sp)
	sw s3, PT_TVAL(sp)
	sw s4, PT_CAUSE(sp)
	sw s5, PT_TP(sp)

	/* Interrupts: bit 31 of CAUSE */
	bltz s4, .Lirq

	/* An exception runs with interrupts as the interrupted code had them */
	andi t0, s1, GVM_ST_PIE
	beqz t0, 1f
	li t0, GVM_ST_IE
	srset zero, status, t0
1:
	la ra, ret_from_exception
	li t0, GVM_CAUSE_SYSCALL
	beq s4, t0, handle_syscall

	mv a0, sp
	FAR_JUMP(do_trap)

.Lirq:
	la ra, ret_from_exception
	mv a0, sp
	FAR_JUMP(generic_handle_arch_irq)

handle_syscall:
	/* a0-a5 and a7 are as the program left them (only t0 and the special registers were used) */
	sw a0, PT_ORIG_A0(sp)
	addi s2, s2, 4			/* past the syscall op */
	sw s2, PT_EPC(sp)
	lw t0, TASK_TI_FLAGS(tp)
	andi t0, t0, 0 + _TIF_SYSCALL_WORK
	bnez t0, handle_syscall_trace_enter
check_syscall_nr:
	li t0, __NR_syscalls
	bgeu a7, t0, .Lni_syscall
	la t1, sys_call_table
	lwx t1, t1, a7, 2
	callr t1
	j ret_from_syscall
.Lni_syscall:
	FAR_CALL(sys_ni_syscall)
ret_from_syscall:
	sw a0, PT_A0(sp)
ret_from_syscall_rejected:
	lw t0, TASK_TI_FLAGS(tp)
	andi t0, t0, 0 + _TIF_SYSCALL_WORK
	bnez t0, handle_syscall_trace_exit

ENTRY(ret_from_exception)
	lw s0, PT_STATUS(sp)
	li t0, GVM_ST_IE
	srclr zero, status, t0
	andi s0, s0, GVM_ST_PS
	bnez s0, resume_kernel

resume_userspace:
	/* interrupts are off: the flags are looked at atomically */
	lw s0, TASK_TI_FLAGS(tp)
	andi s1, s0, 0 + _TIF_WORK_MASK
	bnez s1, work_pending

	/* a task with /dev/gpu open runs with this core's operations through: a load waits only on its own core's,
	 * and what they write (a read's buffer, a page) may go next to a worker or the GPU */
	andi s1, s0, 0 + _TIF_GVM_DRAIN
	beqz s1, .Lops_through
	lw s1, GVM_OPS(zero)
	beqz s1, .Lops_through
	sync
.Lops_through:
	/* the kernel stack is empty again; SCRATCH = the task while in user mode */
	addi s0, sp, PT_SIZE_ON_STACK
	sw s0, TASK_TI_KERNEL_SP(tp)
	mtsr scratch, tp

restore_all:
	/* STATUS as saved has IE 0, so nothing interrupts from here to eret */
	lw a0, PT_STATUS(sp)
	lw a2, PT_EPC(sp)
	mtsr status, a0
	mtsr epc, a2
	lq r60, 240(sp)
	lq r56, 224(sp)
	lq r52, 208(sp)
	lq r48, 192(sp)
	lq r44, 176(sp)
	lq r40, 160(sp)
	lq r36, 144(sp)
	lq r32, 128(sp)
	lq r28, 112(sp)
	lq r24, 96(sp)
	lq r20, 80(sp)
	lq r16, 64(sp)
	lq r12, 48(sp)
	lq r8, 32(sp)
	lq r4, 16(sp)
	lw r1, 4(sp)
	lw r3, 12(sp)
	lw sp, PT_SP(sp)
	eret

resume_kernel:
#ifdef CONFIG_PREEMPTION
	lw s0, TASK_TI_PREEMPT_COUNT(tp)
	bnez s0, restore_all
	lw s0, TASK_TI_FLAGS(tp)
	andi s0, s0, 0 + _TIF_NEED_RESCHED
	beqz s0, restore_all
	FAR_CALL(preempt_schedule_irq)
#endif
	j restore_all

work_pending:
	la ra, ret_from_exception
	andi s1, s0, 0 + _TIF_NEED_RESCHED
	bnez s1, work_resched
	/* signals and notify-resume, with interrupts on */
	li t0, GVM_ST_IE
	srset zero, status, t0
	mv a0, sp
	mv a1, s0
	FAR_JUMP(do_notify_resume)
work_resched:
	FAR_JUMP(schedule)

handle_syscall_trace_enter:
	mv a0, sp
	FAR_CALL(do_syscall_trace_enter)
	mv t1, a0
	lw a0, PT_A0(sp)
	lw a1, PT_A0+4(sp)
	lw a2, PT_A0+8(sp)
	lw a3, PT_A0+12(sp)
	lw a4, PT_A0+16(sp)
	lw a5, PT_A0+20(sp)
	lw a7, PT_A0+28(sp)
	la ra, ret_from_exception
	bnez t1, ret_from_syscall_rejected
	j check_syscall_nr

handle_syscall_trace_exit:
	mv a0, sp
	FAR_CALL(do_syscall_trace_exit)
	j ret_from_exception
END(handle_exception)

/* A new task's first return: schedule_tail(prev), prev in a0 from __switch_to */
ENTRY(ret_from_fork)
	la ra, ret_from_exception
	FAR_JUMP(schedule_tail)
END(ret_from_fork)

/* A kernel thread: fn in s0, its argument in s1 (copy_thread); when fn returns (kernel_execve) it goes to user mode */
ENTRY(ret_from_kernel_thread)
	FAR_CALL(schedule_tail)
	la ra, ret_from_exception
	mv a0, s1
	jr s0
END(ret_from_kernel_thread)

/*
 * struct task_struct *__switch_to(prev in a0, next in a1): the callee-saved s0-s23 (r32-r55), ra and sp into
 * prev->thread, next's back, tp = next. a0 is kept: it is prev for schedule_tail.
 */
ENTRY(__switch_to)
	addi t0, a0, TASK_THREAD
	addi t1, a1, TASK_THREAD
	sq r32, THREAD_S0(t0)
	sq r36, THREAD_S0+16(t0)
	sq r40, THREAD_S0+32(t0)
	sq r44, THREAD_S0+48(t0)
	sq r48, THREAD_S0+64(t0)
	sq r52, THREAD_S0+80(t0)
	sw ra, THREAD_RA(t0)
	sw sp, THREAD_SP(t0)
	lq r32, THREAD_S0(t1)
	lq r36, THREAD_S0+16(t1)
	lq r40, THREAD_S0+32(t1)
	lq r44, THREAD_S0+48(t1)
	lq r48, THREAD_S0+64(t1)
	lq r52, THREAD_S0+80(t1)
	lw ra, THREAD_RA(t1)
	lw sp, THREAD_SP(t1)
	mv tp, a1
	ret
END(__switch_to)

/*
 * The return from a signal handler: setup_rt_frame points ra here (no MMU: user mode runs kernel text, and code on
 * the stack would want a sync before the fetch sees it).
 */
	.balign 16
ENTRY(__user_rt_sigreturn)
	li a7, __NR_rt_sigreturn
	syscall
END(__user_rt_sigreturn)
