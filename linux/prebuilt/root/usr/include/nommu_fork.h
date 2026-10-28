/* No MMU: a fork is a vfork, whose child runs on its parent's memory until it execs or exits. */
#ifndef __ASSEMBLER__
int vfork(void) __attribute__((returns_twice));	/* (pid_t is int: no header, which autoconf's select test and gnulib meet) */
#define fork vfork
#endif
