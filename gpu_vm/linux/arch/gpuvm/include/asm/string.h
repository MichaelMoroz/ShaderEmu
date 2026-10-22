/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GPUVM_STRING_H
#define _ASM_GPUVM_STRING_H

/*
 * arch/gpuvm/lib/string.S: from GVM_OP_MIN bytes up by the commit's operations (a copy or a fill descriptor stored to
 * GVM_OPS; the core goes on, and stops only where it touches what the operation is still at work on), below that by
 * loops of 16-byte loads and stores.
 */
#define GVM_OP_MIN	4096

#ifndef __ASSEMBLY__
#include <linux/types.h>

#define __HAVE_ARCH_MEMCPY
extern void *memcpy(void *dst, const void *src, __kernel_size_t n);
#define __HAVE_ARCH_MEMMOVE
extern void *memmove(void *dst, const void *src, __kernel_size_t n);
#define __HAVE_ARCH_MEMSET
extern void *memset(void *dst, int c, __kernel_size_t n);
#endif

#endif /* _ASM_GPUVM_STRING_H */
