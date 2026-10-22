// SPDX-License-Identifier: GPL-2.0
/* Delays on TIME (100 kHz). A wait shorter than a tick is a tick: TIME moves once a pass. */
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/kernel.h>

#include <asm/gvm.h>

void __delay(unsigned long ticks)
{
	unsigned long long end = gvm_time() + ticks;

	while (gvm_time() < end)
		cpu_relax();
}
EXPORT_SYMBOL(__delay);

void __udelay(unsigned long usecs)
{
	__delay(DIV_ROUND_UP(usecs, 1000000 / GVM_TIMEBASE));
}
EXPORT_SYMBOL(__udelay);

void __ndelay(unsigned long nsecs)
{
	__delay(DIV_ROUND_UP(nsecs, 1000000000 / GVM_TIMEBASE));
}
EXPORT_SYMBOL(__ndelay);
