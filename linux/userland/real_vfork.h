/* vfork() as the kernel has it. The C library's is a fork on this machine (musl has no
 * vfork of its own for riscv32), which copies page tables and then every page written.
 * The child runs on the parent's memory until it execs or exits: system calls only. */
#ifndef REAL_VFORK_H
#define REAL_VFORK_H
#include <sys/types.h>

/* clone(CLONE_VM | CLONE_VFORK | SIGCHLD) with no frame: both come back through ra */
__asm__(".text\n.type real_vfork,%function\nreal_vfork:\n"
	"\tli a7, 220\n\tli a0, 0x4111\n\tli a1, 0\n\tli a2, 0\n\tli a3, 0\n\tli a4, 0\n\tecall\n\tret\n");
pid_t real_vfork(void) __attribute__((returns_twice));

#endif
