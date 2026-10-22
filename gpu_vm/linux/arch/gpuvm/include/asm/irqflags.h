/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GPUVM_IRQFLAGS_H
#define _ASM_GPUVM_IRQFLAGS_H

#include <asm/gvm.h>

/* STATUS.IE; interrupts are taken between bundles only, so each of these is atomic to its own core */
static inline unsigned long arch_local_save_flags(void)
{
	return gvm_mfsr(status);
}

static inline void arch_local_irq_enable(void)
{
	gvm_srset(status, GVM_ST_IE);
}

static inline void arch_local_irq_disable(void)
{
	gvm_srclr(status, GVM_ST_IE);
}

static inline unsigned long arch_local_irq_save(void)
{
	return gvm_srclr(status, GVM_ST_IE);
}

static inline int arch_irqs_disabled_flags(unsigned long flags)
{
	return !(flags & GVM_ST_IE);
}

static inline int arch_irqs_disabled(void)
{
	return arch_irqs_disabled_flags(arch_local_save_flags());
}

static inline void arch_local_irq_restore(unsigned long flags)
{
	gvm_srset(status, flags & GVM_ST_IE);
}

#endif /* _ASM_GPUVM_IRQFLAGS_H */
