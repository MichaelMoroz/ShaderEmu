/* SPDX-License-Identifier: GPL-2.0 */
/*
 * gpu_vm's machine constants for the kernel, C and assembly (gpu_vm/isa.h is the truth; its numbers carry a `u`
 * suffix the assembler does not take, so they are repeated here).
 */
#ifndef _ASM_GPUVM_GVM_H
#define _ASM_GPUVM_GVM_H

#include <linux/const.h>

/* Special registers (the assembler also takes their names: status, epc, ...) */
#define GVM_SR_STATUS	0
#define GVM_SR_EPC	1
#define GVM_SR_CAUSE	2
#define GVM_SR_TVAL	3
#define GVM_SR_TVEC	4
#define GVM_SR_SCRATCH	5
#define GVM_SR_IEN	6
#define GVM_SR_IPEND	7
#define GVM_SR_TCMP_LO	8
#define GVM_SR_TCMP_HI	9
#define GVM_SR_TIME_LO	10
#define GVM_SR_TIME_HI	11
#define GVM_SR_CYCLE	12
#define GVM_SR_COREID	13

/* STATUS: a trap makes PIE = IE, IE = 0, PS = S, S = 1; eret makes IE = PIE, S = PS */
#define GVM_ST_IE	_AC(1, UL)
#define GVM_ST_PIE	_AC(2, UL)
#define GVM_ST_PS	_AC(4, UL)	/* the mode before the trap was the supervisor's */
#define GVM_ST_S	_AC(8, UL)	/* read only */

/* CAUSE; an interrupt is GVM_CAUSE_IRQ | its bit's number (gvm_ref.h's gvm_run) */
#define GVM_CAUSE_ILLEGAL	1
#define GVM_CAUSE_BREAK		2
#define GVM_CAUSE_SYSCALL	3
#define GVM_CAUSE_LOAD		4
#define GVM_CAUSE_STORE		5
#define GVM_CAUSE_FETCH		6
#define GVM_CAUSE_PRIV		7
#define GVM_CAUSE_IRQ		_AC(0x80000000, UL)

/* IEN and IPEND bits */
#define GVM_IRQ_SOFT	0
#define GVM_IRQ_TIMER	1
#define GVM_IRQ_EXT	2
#define GVM_NR_CPU_IRQS	3

/* The address space */
#define GVM_BOOT	_AC(0x00000000, UL)	/* read only to the cores */
#define GVM_DTB		_AC(0x00001000, UL)	/* where gvm_cpu --dtb puts the device tree */
#define GVM_DEV		_AC(0x00010000, UL)	/* device memory */
#define GVM_MBOX	_AC(0x00c00000, UL)
#define GVM_CONTROL	_AC(0x01000000, UL)
#define GVM_RAM		_AC(0x02000000, UL)
#define GVM_ROM		_AC(0x40000000, UL)
#define GVM_ROM_END	_AC(0x60000000, UL)
#define GVM_OPS		_AC(0xf0000000, UL)

/* TIME and TCMP's rate */
#define GVM_TIMEBASE	100000

#ifndef __ASSEMBLY__

#define gvm_mfsr(sr) ({ unsigned long __v;					\
	__asm__ __volatile__("mfsr %0, " #sr : "=r"(__v) : : "memory");	\
	__v; })

#define gvm_mtsr(sr, val) __asm__ __volatile__("mtsr " #sr ", %0" : : "r"((unsigned long)(val)) : "memory")

/* sr |= bits, sr &= ~bits; the old value */
#define gvm_srset(sr, bits) ({ unsigned long __v;				\
	__asm__ __volatile__("srset %0, " #sr ", %1" : "=r"(__v) : "r"((unsigned long)(bits)) : "memory"); \
	__v; })
#define gvm_srclr(sr, bits) ({ unsigned long __v;				\
	__asm__ __volatile__("srclr %0, " #sr ", %1" : "=r"(__v) : "r"((unsigned long)(bits)) : "memory"); \
	__v; })

/* Ends this core's pass: every store so far is committed (seen by the host, the other cores and the fetch). */
static inline void gvm_sync(void)
{
	__asm__ __volatile__("sync" : : : "memory");
}

/* TIME, 64 bits at 100 kHz (the same for the whole of a pass) */
static inline unsigned long long gvm_time(void)
{
	unsigned long hi, lo, hi2;

	do {
		hi = gvm_mfsr(time_hi);
		lo = gvm_mfsr(time_lo);
		hi2 = gvm_mfsr(time_hi);
	} while (hi != hi2);
	return ((unsigned long long)hi << 32) | lo;
}

#endif /* __ASSEMBLY__ */

#endif /* _ASM_GPUVM_GVM_H */
