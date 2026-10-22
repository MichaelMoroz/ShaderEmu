// SPDX-License-Identifier: GPL-2.0
/* No MMU: one zone of RAM, from memblock */
#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/mm.h>

#include <asm/page.h>
#include <asm/pgtable.h>

unsigned long empty_zero_page[PAGE_SIZE / sizeof(unsigned long)] __page_aligned_bss;
EXPORT_SYMBOL(empty_zero_page);

void __init paging_init(void)
{
	unsigned long max_zone_pfn[MAX_NR_ZONES] = { 0, };

	max_zone_pfn[ZONE_NORMAL] = max_low_pfn;
	free_area_init(max_zone_pfn);
}

void __init mem_init(void)
{
	memblock_free_all();
}
