/* The start of a bFLT program (crt_arch.h's _start calls this with the stack binfmt_flat made and START_c).
   binfmt_flat leaves argc, argv[], 0, envp[], 0 and no auxiliary vector; musl's __init_libc reads one after envp.
   This copies the pointers onto its own stack with an auxiliary vector after them and calls START_c with that:
   the page size, the ids (so that musl does not take the program for a set-id one) and a program header table of
   one entry, __gvm_phdr, which elf2flt fills in with the ELF's PT_TLS (its address relocated like any other). */
#include <elf.h>
#include "syscall.h"

/* Initialized, so that it is in .data, where elf2flt can write it; p_type 0 (PT_NULL) when there is no TLS. */
hidden Elf32_Phdr __gvm_phdr = { .p_align = 1 };

hidden _Noreturn void __gvm_flat_start(long *p, void (*start_c)(long *))
{
	long argc = p[0], envc = 0, i, n = 0;
	char **argv = (char **)(p + 1), **envp = argv + argc + 1;
	while (envp[envc]) envc++;
	long v[1 + argc + 1 + envc + 1 + 2 * 9];
	v[n++] = argc;
	for (i = 0; i < argc; i++) v[n++] = (long)argv[i];
	v[n++] = 0;
	for (i = 0; i < envc; i++) v[n++] = (long)envp[i];
	v[n++] = 0;
	v[n++] = AT_PAGESZ; v[n++] = 4096;
	v[n++] = AT_PHDR; v[n++] = (long)&__gvm_phdr;
	v[n++] = AT_PHENT; v[n++] = sizeof(Elf32_Phdr);
	v[n++] = AT_PHNUM; v[n++] = 1;
	v[n++] = AT_UID; v[n++] = __syscall(SYS_getuid);
	v[n++] = AT_EUID; v[n++] = __syscall(SYS_geteuid);
	v[n++] = AT_GID; v[n++] = __syscall(SYS_getgid);
	v[n++] = AT_EGID; v[n++] = __syscall(SYS_getegid);
	v[n++] = AT_NULL; v[n++] = 0;
	start_c(v);
	for (;;);
}
