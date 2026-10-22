/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_ASM_GPUVM_ELF_H
#define _UAPI_ASM_GPUVM_ELF_H

#include <asm/ptrace.h>

/* gpu_vm/isa.h, "ELF" */
#define R_GVM_NONE	0
#define R_GVM_32	1
#define R_GVM_REL32	2
#define R_GVM_IMM32	3	/* an op's long immediate (word 2 of the bundle): S + A */
#define R_GVM_PCIMM32	4	/* lpc's long immediate: S + A - the bundle */
#define R_GVM_BR9	5	/* control word [25:17]: (S + A - the bundle) / 16 */
#define R_GVM_J18	6	/* control word [31:14] (cc 0: the far j, call) */
#define R_GVM_IMM12	7	/* op word [31:20]: S + A, which must fit */
#define R_GVM_TPREL32	8	/* a long immediate: S + A, from the thread pointer */
#define R_GVM_RELATIVE	9

typedef unsigned long elf_greg_t;
typedef struct user_regs_struct elf_gregset_t;
#define ELF_NGREG (sizeof(elf_gregset_t) / sizeof(elf_greg_t))

/* Floats live in the integer registers: no float regset (a placeholder word) */
typedef struct {
	__u32 unused;
} elf_fpregset_t;

#endif /* _UAPI_ASM_GPUVM_ELF_H */
