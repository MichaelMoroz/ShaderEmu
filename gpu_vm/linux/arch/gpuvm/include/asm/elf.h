/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GPUVM_ELF_H
#define _ASM_GPUVM_ELF_H

#include <uapi/asm/elf.h>
#include <linux/elf-em.h>
#include <asm/page.h>

#define ELF_ARCH		EM_GVM
#define ELF_CLASS		ELFCLASS32
#define ELF_DATA		ELFDATA2LSB

#define elf_check_arch(x)	((x)->e_machine == EM_GVM)

#define CORE_DUMP_USE_REGSET
#define ELF_EXEC_PAGESIZE	PAGE_SIZE
#define ELF_ET_DYN_BASE		0
#define ELF_HWCAP		0
#define ELF_PLATFORM		NULL

#endif /* _ASM_GPUVM_ELF_H */
