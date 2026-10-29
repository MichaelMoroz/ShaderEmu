"""No MMU: a program's memory taken from the free lists and given back in blocks. After patch_drivers.py, whose
alloc_pages_run this changes.

- alloc_pages_run takes a free block of the run's order or larger from the free lists when there is one (the excess
  goes back as aligned blocks, as before); it walks the zone from its first page only when there is none. The walk
  was tens of thousands of instructions a call, two calls a program's start.
- free_page_series gives back aligned blocks of pages that only the mapping holds as one free of their order, not a
  page at a time through the per-cpu lists.

Idempotent (a marker in each file).   python3 patch_fastrun.py K"""
import os, sys

tree = sys.argv[1]

p = os.path.join(tree, 'mm/page_alloc.c')
s = open(p).read()
if 'a free block of the run' not in s:
    old = '\t\tfor (pfn = zone->zone_start_pfn; pfn < end && have < nr;) {\n'
    assert s.count(old) == 1, 'alloc_pages_run not found (patch_drivers.py first)'
    s = s.replace(old, '''		/* a free block of the run's order or larger from the free lists; the walk only without one */
		{
			unsigned int o;
			int mt;

			for (o = order_base_2(nr); o < MAX_ORDER && have < nr; o++)
				for (mt = 0; mt < MIGRATE_PCPTYPES && have < nr; mt++) {
					struct page *b = list_first_entry_or_null(&zone->free_area[o].free_list[mt],
										  struct page, lru);
					if (b) {
						start = page_to_pfn(b);
						have = 1UL << o;
					}
				}
		}
		if (have < nr)
''' + old)
    open(p, 'w').write(s)

p = os.path.join(tree, 'mm/nommu.c')
s = open(p).read()
if 'aligned blocks of pages that only' not in s:
    old = '''static void free_page_series(unsigned long from, unsigned long to)
{
	for (; from < to; from += PAGE_SIZE) {
		struct page *page = virt_to_page(from);

		atomic_long_dec(&mmap_pages_allocated);
		put_page(page);
	}
}'''
    assert s.count(old) == 1, 'free_page_series not found'
    s = s.replace(old, '''static void free_page_series(unsigned long from, unsigned long to)
{
	/* aligned blocks of pages that only this mapping holds go back whole, one free of their order */
	while (from < to) {
		struct page *page = virt_to_page(from);
		unsigned long pfn = page_to_pfn(page), n = (to - from) >> PAGE_SHIFT, i;
		unsigned int o = min_t(unsigned int, pfn ? __ffs(pfn) : MAX_ORDER - 1, ilog2(n));

		o = min_t(unsigned int, o, MAX_ORDER - 1);
		for (i = 0; o && i < (1UL << o); i++)
			if (page_count(page + i) != 1 || PageReserved(page + i))
				o = 0;
		if (o) {
			for (i = 1; i < (1UL << o); i++)
				set_page_count(page + i, 0);
			atomic_long_sub(1L << o, &mmap_pages_allocated);
			__free_pages(page, o);
		} else {
			atomic_long_dec(&mmap_pages_allocated);
			put_page(page);
		}
		from += PAGE_SIZE << o;
	}
}''')
    open(p, 'w').write(s)
print('alloc_pages_run from the free lists, free_page_series in blocks')
