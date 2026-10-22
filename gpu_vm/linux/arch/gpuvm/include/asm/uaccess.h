/* SPDX-License-Identifier: GPL-2.0 */
/*
 * User memory (no MMU: the user's pointers are addresses, read and written by memcpy; gpu_vm/linux/README.md).
 *
 * - access_ok: a user's range must lie in RAM (memory_start to memory_end). Nothing else is user memory here, and a
 *   kernel access outside RAM faults (the boot area takes no stores, the device window and the operations region are
 *   not memory, past RAM's end nothing): with this check a bad pointer given to a system call is -EFAULT, not a
 *   fault in the kernel (an oops, and the process killed holding whatever lock it held). A wild pointer inside RAM is
 *   not caught (no MMU). A range of 0 bytes is anywhere. The strings' functions read at most up to RAM's end (TASK_SIZE_MAX).
 * - get_user, put_user: one load or store when the pointer is aligned (asm-generic's UACCESS_MEMCPY makes every one a
 *   get_unaligned or put_unaligned: four byte accesses on gpu_vm), the bytes otherwise.
 */
#ifndef _ASM_GPUVM_UACCESS_H
#define _ASM_GPUVM_UACCESS_H

#include <linux/string.h>
#include <linux/types.h>
#include <asm/unaligned.h>

extern unsigned long memory_start, memory_end;

#define TASK_SIZE_MAX	memory_end

static inline int __access_ok(unsigned long addr, unsigned long size)
{
	/* nothing at all is always there (musl's writev of {NULL, 0} after its buffer) */
	return !size || (addr >= memory_start && addr <= memory_end && size <= memory_end - addr);
}
#define __access_ok __access_ok

static __always_inline int __get_user_fn(size_t size, const void __user *from, void *to)
{
	const void *p = (const void __force *)from;
	unsigned long a = (unsigned long)p;

	switch (size) {
	case 1:
		*(u8 *)to = *(const u8 *)p;
		return 0;
	case 2:
		*(u16 *)to = (a & 1) ? get_unaligned((const u16 *)p) : *(const u16 *)p;
		return 0;
	case 4:
		*(u32 *)to = (a & 3) ? get_unaligned((const u32 *)p) : *(const u32 *)p;
		return 0;
	case 8:
		if (a & 3)
			*(u64 *)to = get_unaligned((const u64 *)p);
		else
			*(u64 *)to = ((const u32 *)p)[0] | (u64)((const u32 *)p)[1] << 32;
		return 0;
	}
	return -EFAULT;
}
#define __get_user_fn(sz, u, k)	__get_user_fn(sz, u, k)

static __always_inline int __put_user_fn(size_t size, void __user *to, void *from)
{
	void *p = (void __force *)to;
	unsigned long a = (unsigned long)p;

	switch (size) {
	case 1:
		*(u8 *)p = *(u8 *)from;
		return 0;
	case 2:
		if (a & 1)
			put_unaligned(*(u16 *)from, (u16 *)p);
		else
			*(u16 *)p = *(u16 *)from;
		return 0;
	case 4:
		if (a & 3)
			put_unaligned(*(u32 *)from, (u32 *)p);
		else
			*(u32 *)p = *(u32 *)from;
		return 0;
	case 8:
		if (a & 3) {
			put_unaligned(*(u64 *)from, (u64 *)p);
		} else {
			((u32 *)p)[0] = (u32)*(u64 *)from;
			((u32 *)p)[1] = (u32)(*(u64 *)from >> 32);
		}
		return 0;
	}
	return -EFAULT;
}
#define __put_user_fn(sz, u, k)	__put_user_fn(sz, u, k)

/* kernel addresses (copy_from_kernel_nofault): as asm-generic's UACCESS_MEMCPY makes them */
#define __get_kernel_nofault(dst, src, type, err_label)			\
do {									\
	*((type *)dst) = get_unaligned((type *)(src));			\
	if (0)								\
		goto err_label;						\
} while (0)

#define __put_kernel_nofault(dst, src, type, err_label)			\
do {									\
	put_unaligned(*((type *)src), (type *)(dst));			\
	if (0)								\
		goto err_label;						\
} while (0)

#define HAVE_GET_KERNEL_NOFAULT 1

static inline __must_check unsigned long raw_copy_from_user(void *to, const void __user *from, unsigned long n)
{
	memcpy(to, (const void __force *)from, n);
	return 0;
}

static inline __must_check unsigned long raw_copy_to_user(void __user *to, const void *from, unsigned long n)
{
	memcpy((void __force *)to, from, n);
	return 0;
}
#define INLINE_COPY_FROM_USER
#define INLINE_COPY_TO_USER

#include <asm-generic/uaccess.h>

#endif /* _ASM_GPUVM_UACCESS_H */
