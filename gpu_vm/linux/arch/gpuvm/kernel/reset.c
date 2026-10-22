// SPDX-License-Identifier: GPL-2.0
/*
 * Halt and power off: with TVEC 0 a syscall is the host's (gvm_cpu's semihosting), and exit_group stops the machine.
 * On a machine with no such host the core traps to address 0 and stays there.
 */
#include <linux/export.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/pm.h>
#include <linux/reboot.h>

#include <asm/gvm.h>

void (*pm_power_off)(void);
EXPORT_SYMBOL(pm_power_off);

static void __noreturn gvm_host_exit(unsigned long code)
{
	local_irq_disable();
	gvm_mtsr(ien, 0);
	gvm_mtsr(tvec, 0);
	for (;;) {
		/* exit_group(code); nothing comes back from it (a0 and a7 are not restored) */
		__asm__ __volatile__("mv a0, %0\n\tli a7, 94\n\tsyscall" : : "r"(code) : "memory");
		__asm__ __volatile__("wait" : : : "memory");
	}
}

void machine_restart(char *cmd)
{
	do_kernel_restart(cmd);
	gvm_host_exit(1);
}

void machine_halt(void)
{
	gvm_host_exit(0);
}

void machine_power_off(void)
{
	if (pm_power_off)
		pm_power_off();
	gvm_host_exit(0);
}
