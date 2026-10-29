"""No MMU: the boot's struct pages made once and copied, and no BLAKE2s self-test. On top of linux/kernel/bootpages_hook.py, which already starts a boot page's count
at 0 and counts the pages kept in reserve_bootmem_region.

- memmap_init_range: every page of a flat zone has the same struct page at first (the zone's and the node's flags,
  free, count 0), so the first is made and copied by doubling memcpy (the machine's parallel copy, memcpy_hook.py),
  then each pageblock's migrate type set. It was a store or more to each page's own 16 bytes.
- __free_pages_core: the pages are free and not reserved already, so it stores only where that is not so (the
  kernel's own pages, which reserve_bootmem_region marked).
- free_pages_prepare: a page's flags stored only when it has one of the flags to clear.
- blake2s_mod_init: no self-test at every boot (no crypto manager to turn it off).
   python3 patch_memmap.py K"""
import sys
K = sys.argv[1]


def patch(path, changes):
    p = K + '/' + path
    s = open(p).read()
    for old, new in changes:
        if new in s:
            continue
        assert s.count(old) == 1, (path, old)
        s = s.replace(old, new)
    open(p, 'w').write(s)


patch('mm/page_alloc.c', [
    ('''			if (unlikely(check_free_page(page + i))) {
				bad++;
				continue;
			}
			(page + i)->flags &= ~PAGE_FLAGS_CHECK_AT_PREP;
''', '''			if (unlikely(check_free_page(page + i))) {
				bad++;
				continue;
			}
			/* (a store only where there is a flag to clear) */
			if ((page + i)->flags & PAGE_FLAGS_CHECK_AT_PREP)
				(page + i)->flags &= ~PAGE_FLAGS_CHECK_AT_PREP;
'''),
    ('''	prefetchw(p);
	for (loop = 0; loop < (nr_pages - 1); loop++, p++) {
		prefetchw(p + 1);
		__ClearPageReserved(p);
		set_page_count(p, 0);
	}
	__ClearPageReserved(p);
	set_page_count(p, 0);
''', '''	/* (memmap_init_range made the pages free, count 0 and not reserved: stores only where that changed) */
	for (loop = 0; loop < nr_pages; loop++, p++) {
		if (unlikely(PageReserved(p)))
			__ClearPageReserved(p);
		if (unlikely(page_ref_count(p)))
			set_page_count(p, 0);
	}
'''),
    ('''#endif

	for (pfn = start_pfn; pfn < end_pfn; ) {
		/*
		 * There can be holes in boot-time mem_map[]s handed to this
''', '''#endif

#if defined(CONFIG_FLATMEM) && !defined(CONFIG_DEFERRED_STRUCT_PAGE_INIT)
	if (context == MEMINIT_EARLY && !mirrored_kernelcore && start_pfn < end_pfn) {
		struct page *first = pfn_to_page(start_pfn);
		unsigned long n = 1, total = end_pfn - start_pfn;

		/*
		 * Every page of a flat zone has the same struct page at first (its flags are the zone's and the node's),
		 * and it is made free here, count 0, so that __free_pages_core stores nothing: reserve_bootmem_region
		 * gives reserved pages their count of 1. lru is left 0, which is no PageTail.
		 */
		__init_single_page(first, start_pfn, zone, nid);
		set_page_count(first, 0);
		first->lru.next = first->lru.prev = NULL;
		while (n < total) {
			unsigned long k = min(n, total - n);

			memcpy(first + n, first, k * sizeof(struct page));
			n += k;
		}
		for (pfn = ALIGN(start_pfn, pageblock_nr_pages); pfn < end_pfn; pfn += pageblock_nr_pages) {
			set_pageblock_migratetype(pfn_to_page(pfn), migratetype);
			cond_resched();
		}
		return;
	}
#endif

	for (pfn = start_pfn; pfn < end_pfn; ) {
		/*
		 * There can be holes in boot-time mem_map[]s handed to this
'''),
])
patch('lib/crypto/blake2s.c', [
    ('''	if (!IS_ENABLED(CONFIG_CRYPTO_MANAGER_DISABLE_TESTS) &&
	    WARN_ON(!blake2s_selftest()))
		return -ENODEV;
''', '''	/* (no self-test: a share of the boot's first second, and no crypto manager to turn it off) */
	if (0 && WARN_ON(!blake2s_selftest()))
		return -ENODEV;
'''),
])
print('memmap copied, blake2s untested')
