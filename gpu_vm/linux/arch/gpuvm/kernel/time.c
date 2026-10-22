// SPDX-License-Identifier: GPL-2.0
/*
 * The timer: TIME (special registers 10-11) counts at 100 kHz, the same for a whole pass; the timer line is pending
 * while TCMP (8-9) is not 0 and TIME >= TCMP. A clocksource and sched_clock from TIME, a one-shot clockevent from TCMP
 * (0 turns it off).
 */
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/sched_clock.h>

#include <asm/gvm.h>

extern struct irq_domain *gvm_intc_domain;
void gvm_console_tick(void);

static void gvm_set_tcmp(unsigned long long t)
{
	gvm_mtsr(tcmp_hi, (unsigned long)(t >> 32));
	gvm_mtsr(tcmp_lo, (unsigned long)t);
}

static u64 gvm_cs_read(struct clocksource *cs)
{
	return gvm_time();
}

static u64 notrace gvm_sched_clock_read(void)
{
	return gvm_time();
}

static struct clocksource gvm_clocksource = {
	.name	= "gvm-time",
	.rating	= 300,
	.read	= gvm_cs_read,
	.mask	= CLOCKSOURCE_MASK(64),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

static int gvm_next_event(unsigned long delta, struct clock_event_device *ce)
{
	gvm_set_tcmp(gvm_time() + delta);
	return 0;
}

static int gvm_timer_shutdown(struct clock_event_device *ce)
{
	gvm_set_tcmp(0);
	return 0;
}

static struct clock_event_device gvm_clockevent = {
	.name			= "gvm-tcmp",
	.features		= CLOCK_EVT_FEAT_ONESHOT,
	.rating			= 300,
	.set_next_event		= gvm_next_event,
	.set_state_shutdown	= gvm_timer_shutdown,
	.set_state_oneshot_stopped = gvm_timer_shutdown,
};

static irqreturn_t gvm_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *ce = dev_id;

	gvm_set_tcmp(0);	/* the line drops; the handler sets the next compare */
#ifdef CONFIG_GPUVM_CONSOLE
	gvm_console_tick();	/* input is polled from the tick (no device interrupt) */
#endif
	ce->event_handler(ce);
	return IRQ_HANDLED;
}

void __init time_init(void)
{
	int irq;

	gvm_set_tcmp(0);
	clocksource_register_hz(&gvm_clocksource, GVM_TIMEBASE);
	sched_clock_register(gvm_sched_clock_read, 64, GVM_TIMEBASE);

	irq = irq_create_mapping(gvm_intc_domain, GVM_IRQ_TIMER);
	if (!irq)
		panic("gvm timer: no irq");
	if (request_irq(irq, gvm_timer_interrupt, IRQF_TIMER, "gvm-timer", &gvm_clockevent))
		panic("gvm timer: cannot request its irq");

	gvm_clockevent.cpumask = cpumask_of(0);
	gvm_clockevent.irq = irq;
	clockevents_config_and_register(&gvm_clockevent, GVM_TIMEBASE, 1, 0x7fffffff);

	/* __delay counts TIME's ticks: calibrate_delay takes this and skips its loop */
	lpj_fine = GVM_TIMEBASE / HZ;
}
