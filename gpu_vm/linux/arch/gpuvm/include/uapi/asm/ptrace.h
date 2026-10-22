/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_ASM_GPUVM_PTRACE_H
#define _UAPI_ASM_GPUVM_PTRACE_H

#ifndef __ASSEMBLY__

#include <linux/types.h>

/*
 * A thread's registers as ptrace's NT_PRSTATUS regset and a signal's sigcontext give them: word n is rn, but word 0
 * (r0 reads 0) is the pc. The kernel's pt_regs starts with these 64 words.
 */
struct user_regs_struct {
	unsigned long gpr[64];
};

#endif /* __ASSEMBLY__ */

/* Indexes of user_regs_struct.gpr (gpu_vm/GPU_VM.md, "ABI") */
#define GVM_REG_PC	0
#define GVM_REG_RA	1
#define GVM_REG_SP	2
#define GVM_REG_GP	3
#define GVM_REG_TP	4
#define GVM_REG_A0	5	/* a0-a7: r5-r12; a0, a1 the results; a7 the syscall number */
#define GVM_REG_A7	12
#define GVM_REG_T0	13	/* t0-t18: r13-r31 */
#define GVM_REG_S0	32	/* s0-s23: r32-r55; s0 the frame pointer */
#define GVM_REG_T19	56	/* t19-t26: r56-r63 */
#define GVM_NGPR	64

#endif /* _UAPI_ASM_GPUVM_PTRACE_H */
