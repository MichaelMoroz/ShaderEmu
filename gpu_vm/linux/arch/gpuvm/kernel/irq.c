// SPDX-License-Identifier: GPL-2.0
/*
 * The core's own interrupts: IEN masks and IPEND shows three lines, 0 software, 1 timer, 2 external. An interrupt is
 * taken between bundles with STATUS.IE set; CAUSE is GVM_CAUSE_IRQ | the line's number. One domain of three hwirqs
 * ("gpuvm,cpu-intc" in the device tree; #interrupt-cells 1).
 */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/seq_file.h>

#include <asm/gvm.h>

struct irq_domain *gvm_intc_domain;

static void gvm_intc_mask(struct irq_data *d)
{
	gvm_srclr(ien, 1UL << d->hwirq);
}

static void gvm_intc_unmask(struct irq_data *d)
{
	gvm_srset(ien, 1UL << d->hwirq);
}

static struct irq_chip gvm_intc_chip = {
	.name		= "gvm-intc",
	.irq_mask	= gvm_intc_mask,
	.irq_unmask	= gvm_intc_unmask,
};

static int gvm_intc_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hw)
{
	irq_set_chip_and_handler(irq, &gvm_intc_chip, handle_level_irq);
	irq_set_noprobe(irq);
	return 0;
}

static const struct irq_domain_ops gvm_intc_ops = {
	.map	= gvm_intc_map,
	.xlate	= irq_domain_xlate_onecell,
};

static void gvm_handle_irq(struct pt_regs *regs)
{
	irq_hw_number_t hw = regs->cause & ~GVM_CAUSE_IRQ;

	if (generic_handle_domain_irq(gvm_intc_domain, hw)) {
		/* a line nobody asked for: masked, or it would be taken again at once */
		gvm_srclr(ien, 1UL << hw);
		pr_warn_ratelimited("gvm-intc: spurious interrupt %lu\n", (unsigned long)hw);
	}
}

void __init init_IRQ(void)
{
	struct device_node *node = of_find_compatible_node(NULL, NULL, "gpuvm,cpu-intc");

	gvm_mtsr(ien, 0);
	gvm_intc_domain = irq_domain_add_linear(node, GVM_NR_CPU_IRQS, &gvm_intc_ops, NULL);
	if (!gvm_intc_domain)
		panic("gvm-intc: no irq domain");
	irq_set_default_host(gvm_intc_domain);
	if (set_handle_irq(gvm_handle_irq))
		panic("gvm-intc: handle_arch_irq is taken");
	of_node_put(node);
}
