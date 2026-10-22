/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * gpu_vm's system calls: asm-generic's numbers, 32-bit with 64-bit times only (as rv32: no time32 calls, no
 * stat64; statx and the *_time64 calls). The call: the number in r12 (a7), the arguments in r5-r10 (a0-a5), the
 * `syscall` op; the result in r5 (a0), a negative errno on failure. No include guard: the syscall table includes it
 * again with __SYSCALL defined.
 */

#define __ARCH_WANT_SYS_CLONE3

#include <asm-generic/unistd.h>
