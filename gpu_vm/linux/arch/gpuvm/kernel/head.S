/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The kernel's entry (gpu_vm/linux/README.md, "Boot protocol"): the image is loaded at GVM_RAM (0x02000000) and
 * started at its first byte, in supervisor mode, on core 0, with r5 (a0) the device tree's address (0: the tree is
 * at GVM_DTB, 0x1000, where gvm_cpu --dtb puts it). Any other register is ignored. Another core that starts here
 * waits for good (SMP is not done).
 */
#include <linux/init.h>
#include <linux/linkage.h>
#include <asm/asm-offsets.h>
#include <asm/thread_info.h>
#include <asm/gvm.h>

__HEAD
ENTRY(_start)
	mfsr t0, coreid
	bnez t0, .Lpark

	/* no interrupts, no handler yet: TVEC 0 leaves a trap to gvm_cpu's report until trap_init */
	mtsr ien, zero
	mtsr status, zero
	mtsr scratch, zero
	mtsr tcmp_lo, zero
	mtsr tcmp_hi, zero

	mv s0, a0

	/* the bss */
	la t0, __bss_start
	la t1, __bss_stop
	bgeu t0, t1, 2f
1:	sw zero, 0(t0)
	addi t0, t0, 4
	bltu t0, t1, 1b
2:
	la t0, gvm_boot_dtb
	sw s0, 0(t0)

	la tp, init_task
	la sp, init_thread_union + THREAD_SIZE - PT_SIZE_ON_STACK
	la t0, start_kernel
	callr t0
	/* start_kernel does not return */
.Lpark:
	wait
	j .Lpark
END(_start)
