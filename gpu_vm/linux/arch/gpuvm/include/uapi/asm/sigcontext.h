/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_ASM_GPUVM_SIGCONTEXT_H
#define _UAPI_ASM_GPUVM_SIGCONTEXT_H

#include <asm/ptrace.h>

/*
 * A signal handler's machine context: the registers (floats are in them too: one register file), the pc in
 * sc_regs.gpr[0].
 */
struct sigcontext {
	struct user_regs_struct sc_regs;
};

#endif /* _UAPI_ASM_GPUVM_SIGCONTEXT_H */
