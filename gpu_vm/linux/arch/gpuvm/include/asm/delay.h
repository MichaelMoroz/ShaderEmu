/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GPUVM_DELAY_H
#define _ASM_GPUVM_DELAY_H

/*
 * Delays wait on TIME (100 kHz: 10 us a tick, the same for a whole pass). A "loop" of __delay is a tick of TIME,
 * and loops_per_jiffy is GVM_TIMEBASE / HZ (time_init's lpj_fine).
 */
extern void __delay(unsigned long ticks);
extern void __udelay(unsigned long usecs);
extern void __ndelay(unsigned long nsecs);

#define udelay(n)	__udelay(n)
#define ndelay(n)	__ndelay(n)

#endif /* _ASM_GPUVM_DELAY_H */
