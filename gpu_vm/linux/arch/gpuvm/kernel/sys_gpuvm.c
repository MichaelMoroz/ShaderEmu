// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/syscalls.h>

/* asm-generic's 32-bit mmap: the offset in 4 KB units */
SYSCALL_DEFINE6(mmap2, unsigned long, addr, unsigned long, len, unsigned long, prot, unsigned long, flags,
		unsigned long, fd, unsigned long, offset)
{
	return ksys_mmap_pgoff(addr, len, prot, flags, fd, offset >> (PAGE_SHIFT - 12));
}
