/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GPUVM_TIMEX_H
#define _ASM_GPUVM_TIMEX_H

#include <asm/gvm.h>

#define CLOCK_TICK_RATE	GVM_TIMEBASE

typedef unsigned long cycles_t;

/* The bundles this core has run */
static inline cycles_t get_cycles(void)
{
	return gvm_mfsr(cycle);
}
#define get_cycles get_cycles

#endif /* _ASM_GPUVM_TIMEX_H */
