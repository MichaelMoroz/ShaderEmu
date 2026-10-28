"""No-MMU kernel: the GPU's and the sound card's mmap at their physical addresses, the devices found by page_is_ram, and buddy blocks up to 32 MB."""
import re, sys
K = sys.argv[1]


def patch(path, fn, phys, fops):
    p = f'{K}/{path}'
    s = open(p).read()
    if 'nommu_area' in s:
        return
    area = (f'\n#ifndef CONFIG_MMU\nstatic unsigned long {fn}_nommu_area(struct file *file, unsigned long addr, unsigned long len,\n'
            f'\t\tunsigned long pgoff, unsigned long flags)\n{{\n\treturn {phys} + (pgoff << PAGE_SHIFT);\n}}\n#endif\n')
    i = s.index(f'static const struct file_operations {fops}')
    s = s[:i] + area + '\n' + s[i:]
    s = s.replace(f'\t.mmap\t\t= {fn}_mmap,\n', f'\t.mmap\t\t= {fn}_mmap,\n#ifndef CONFIG_MMU\n\t.get_unmapped_area = {fn}_nommu_area,\n#endif\n', 1)
    assert 'get_unmapped_area' in s
    open(p, 'w').write(s)


patch('drivers/misc/shaderemu_gpu.c', 'shaderemu_gpu', 'GPU_PHYS', 'shaderemu_gpu_fops')
patch('drivers/misc/shaderemu_sound.c', 'shaderemu_sound', 'SND_REGS', 'shaderemu_sound_fops')
# (RAM lay on both sides of the devices until 28 October 2026, their pages in the memory map's range, so
# pfn_valid did not say whether the device tree gave them to the kernel; page_is_ram does, wherever they are)
for path in ('drivers/misc/shaderemu_gpu.c', 'drivers/misc/shaderemu_sound.c', 'drivers/video/fbdev/shaderemu_fb.c',
             'drivers/input/misc/shaderemu_input.c', 'drivers/net/shaderemu_net.c'):
    p = f'{K}/{path}'
    s = open(p).read()
    if 'pfn_valid(' in s:
        s = re.sub(r'\bpfn_valid\(([A-Z_]+) >> PAGE_SHIFT\)', r'page_is_ram(\1 >> PAGE_SHIFT)', s)
        assert 'pfn_valid(' not in s, path
        open(p, 'w').write(s)
p = f'{K}/arch/riscv/Kconfig'
s = open(p).read()
if 'FORCE_MAX_ZONEORDER' not in s:
    s += '\nconfig FORCE_MAX_ZONEORDER\n\tint\n\tdefault 14 if !MMU\n\tdefault 11\n'
    open(p, 'w').write(s)
# the kernel's early allocations from the bottom: the large blocks at the top stay whole for programs
p = f'{K}/arch/riscv/mm/init.c'
s = open(p).read()
old = '\tmemblock_enforce_memory_limit(memory_limit);\n'
if 'memblock_set_bottom_up' not in s:
    assert s.count(old) == 1
    s = s.replace(old, old + '#ifndef CONFIG_MMU\n\tmemblock_set_bottom_up(true);\n#endif\n')
    open(p, 'w').write(s)
# a mapping takes the first run of free pages long enough, wherever it lies: not a power of two
# of pages on its own boundary, which a fragmented machine soon has none of
RUN = r'''
#ifndef CONFIG_MMU
/* No MMU: the first run of free pages at least `nr` long, as single pages (do_mmap_private) */
void *alloc_pages_run(unsigned long nr)
{
	struct zone *zone;
	bool drained = false;

again:
	for_each_populated_zone(zone) {
		unsigned long flags, pfn, p, start = 0, have = 0, end = zone_end_pfn(zone);

		spin_lock_irqsave(&zone->lock, flags);
		for (pfn = zone->zone_start_pfn; pfn < end && have < nr;) {
			struct page *page = pfn_to_page(pfn);

			if (PageBuddy(page)) {
				if (!have)
					start = pfn;
				have += 1UL << buddy_order(page);
				pfn += 1UL << buddy_order(page);
			} else {
				have = 0;
				pfn++;
			}
		}
		if (have < nr) {
			spin_unlock_irqrestore(&zone->lock, flags);
			continue;
		}
		for (pfn = start; pfn < start + nr;) {
			struct page *page = pfn_to_page(pfn);
			unsigned int order = buddy_order(page);
			int mt = get_pageblock_migratetype(page);
			unsigned long top = pfn + (1UL << order);

			__mod_zone_freepage_state(zone, -(1L << order), mt);
			del_page_from_free_list(page, zone, order);
			/* what the run does not need goes back as aligned blocks */
			for (p = start + nr; p < top;) {
				unsigned int o = min_t(unsigned int, __ffs(p), ilog2(top - p));

				__free_one_page(pfn_to_page(p), p, zone, o, mt, FPI_NONE);
				p += 1UL << o;
			}
			for (p = pfn; p < top && p < start + nr; p++)
				post_alloc_hook(pfn_to_page(p), 0, GFP_KERNEL);
			pfn = top;
		}
		spin_unlock_irqrestore(&zone->lock, flags);
		return page_address(pfn_to_page(start));
	}
	if (!drained) {
		drain_all_pages(NULL);
		drained = true;
		goto again;
	}
	return NULL;
}
#endif
'''
p = f'{K}/mm/page_alloc.c'
s = open(p).read()
if 'what the run does not need goes back' not in s:
    if 'alloc_pages_run' in s:
        s = s[:s.index('\n#ifndef CONFIG_MMU\n/* No MMU: the first run of free pages')]
    s += RUN
    open(p, 'w').write(s)
p = f'{K}/mm/nommu.c'
s = open(p).read()
old = '\tbase = alloc_pages_exact(total << PAGE_SHIFT, GFP_KERNEL);\n'
if 'alloc_pages_run' not in s:
    assert s.count(old) == 1
    s = s.replace(old, '\tbase = total >= 16 ? alloc_pages_run(total) : NULL;\n\tif (!base)\n\t\tbase = alloc_pages_exact(total << PAGE_SHIFT, GFP_KERNEL);\n')
    s = s.replace('static int do_mmap_private(', 'void *alloc_pages_run(unsigned long nr);\n\nstatic int do_mmap_private(', 1)
    open(p, 'w').write(s)
# /proc/cpuinfo names Sv32 only with an MMU (5.17 prints it whatever the kernel was built with)
p = f'{K}/arch/riscv/kernel/cpu.c'
s = open(p).read()
old = '\tprint_mmu(m);\n'
if 'IS_ENABLED(CONFIG_MMU))\n\t\tprint_mmu' not in s:
    assert s.count(old) == 1
    s = s.replace(old, '\tif (IS_ENABLED(CONFIG_MMU))\n\t\tprint_mmu(m);\n')
    open(p, 'w').write(s)
print('drivers and MAX_ORDER patched')
