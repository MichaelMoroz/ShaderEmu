/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GPUVM_CACHE_H
#define _ASM_GPUVM_CACHE_H

/* A texel of the state texture: 16 bytes, the unit of the write cache and of a bundle */
#define L1_CACHE_SHIFT	4
#define L1_CACHE_BYTES	(1 << L1_CACHE_SHIFT)

#endif /* _ASM_GPUVM_CACHE_H */
