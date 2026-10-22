/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GPUVM_CACHEFLUSH_H
#define _ASM_GPUVM_CACHEFLUSH_H

#include <asm/gvm.h>

/*
 * The fetch reads RAM as the last commit left it: code stored in this pass runs after a sync (gpu_vm/GPU_VM.md,
 * "Code stored in a pass runs after a sync"). A loaded flat program, anything written and then executed.
 */
#define flush_icache_range flush_icache_range
static inline void flush_icache_range(unsigned long start, unsigned long end)
{
	gvm_sync();
}

#include <asm-generic/cacheflush.h>

#endif /* _ASM_GPUVM_CACHEFLUSH_H */
