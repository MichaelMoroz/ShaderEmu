// SPDX-License-Identifier: GPL-2.0
#include <linux/linkage.h>
#include <linux/syscalls.h>
#include <asm-generic/syscalls.h>
#include <asm/syscall.h>

#undef __SYSCALL
#define __SYSCALL(nr, call)	[nr] = (call),

void * const sys_call_table[__NR_syscalls] = {
	[0 ... __NR_syscalls - 1] = sys_ni_syscall,
#include <asm/unistd.h>
};
