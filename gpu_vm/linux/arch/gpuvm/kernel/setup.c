// SPDX-License-Identifier: GPL-2.0
/*
 * The machine's setup: the device tree from head.S's r5 (or GVM_DTB), RAM from its memory node (or
 * CONFIG_GPUVM_RAM_SIZE from GVM_RAM), the boot console first.
 */
#include <linux/console.h>
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/libfdt.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_platform.h>
#include <linux/seq_file.h>
#include <linux/start_kernel.h>

#include <asm/gvm.h>
#include <asm/page.h>
#include <asm/sections.h>
#include <asm/setup.h>

/* head.S: the device tree's address as the loader passed it in r5 */
unsigned long gvm_boot_dtb;

unsigned long memory_start;
EXPORT_SYMBOL(memory_start);
unsigned long memory_end;
EXPORT_SYMBOL(memory_end);

void gvm_early_console_init(void);

static void __init gvm_dt_setup(void)
{
	void *dtb = (void *)(gvm_boot_dtb ? gvm_boot_dtb : GVM_DTB);

	if (fdt_check_header(dtb) || !early_init_dt_scan(dtb))
		pr_warn("gpuvm: no device tree at 0x%08lx\n", (unsigned long)dtb);
	if (!memblock_phys_mem_size()) {
		pr_warn("gpuvm: no memory node: %u MB from 0x%08lx\n",
			CONFIG_GPUVM_RAM_SIZE >> 20, GVM_RAM);
		memblock_add(GVM_RAM, CONFIG_GPUVM_RAM_SIZE);
	}
}

static void __init gvm_bootmem(void)
{
	memory_start = memblock_start_of_DRAM();
	memory_end = memblock_end_of_DRAM();

	min_low_pfn = PFN_UP(memory_start);
	max_low_pfn = max_pfn = PFN_DOWN(memory_end);
	set_max_mapnr(max_low_pfn - ARCH_PFN_OFFSET);
	high_memory = (void *)memory_end;

	memblock_reserve(__pa(_text), _end - _text);
	/*
	 * The compiler may load up to 16 bytes past an access through the same register (a speculative load made early,
	 * gpu_vm/llvm/README.md); gpu_vm faults only beyond RAM's end, so the last 64 bytes of RAM are never used.
	 */
	memblock_reserve(memory_end - 64, 64);
	early_init_fdt_reserve_self();
	early_init_fdt_scan_reserved_mem();
	memblock_dump_all();
	/* the kernel's early allocations from the bottom: the large blocks at the top stay whole for programs (as the
	 * RISC-V machine's no-MMU kernel, linux/nommu/patch_drivers.py) */
	memblock_set_bottom_up(true);
}

void __init setup_arch(char **cmdline_p)
{
#ifdef CONFIG_GPUVM_CONSOLE
	gvm_early_console_init();
#endif
	setup_initial_init_mm(_stext, _etext, _edata, _end);

	gvm_dt_setup();
	*cmdline_p = boot_command_line;
	parse_early_param();

	gvm_bootmem();
	unflatten_device_tree();
	paging_init();
}

static int __init gvm_device_probe(void)
{
	return of_platform_default_populate(NULL, NULL, NULL);
}
arch_initcall(gvm_device_probe);

static int show_cpuinfo(struct seq_file *m, void *v)
{
	unsigned long n = (unsigned long)v - 1;

	seq_printf(m, "processor\t: %lu\n", n);
	seq_puts(m, "isa\t\t: gpu_vm\n");
	seq_printf(m, "timebase\t: %u\n\n", GVM_TIMEBASE);
	return 0;
}

static void *c_start(struct seq_file *m, loff_t *pos)
{
	if (*pos == nr_cpu_ids)
		return NULL;
	*pos = cpumask_next(*pos - 1, cpu_online_mask);
	if (*pos >= nr_cpu_ids)
		return NULL;
	return (void *)(uintptr_t)(1 + *pos);
}

static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	(*pos)++;
	return c_start(m, pos);
}

static void c_stop(struct seq_file *m, void *v)
{
}

const struct seq_operations cpuinfo_op = {
	.start	= c_start,
	.next	= c_next,
	.stop	= c_stop,
	.show	= show_cpuinfo,
};
