"""No MMU: work of the boot that nothing reads (docs/machine/boot.md).

- mm/page_alloc.c, mm/memblock.c: while memblock hands over its free ranges every page in them is as memmap_init_range
  made it (free, count 0, no flags), and the ranges hold no reserved page, so __free_pages_core's loop and
  free_pages_prepare's look at each tail page find nothing: 1.4 of the boot's 7.3 million instructions.
- CONFIG_CONSOLE_LOGLEVEL_DEFAULT=4 (kernel.sh): the kernel's notices stay in its log and off the console, warnings still come.
- net/ipv4/route.c: the IP identifiers of 4,096 hash slots start at 0, not random (a machine's network is the world's own):
  300 thousand instructions and 1,024 texels stored.
- lib/kobject_uevent.c: no uevent while the kernel is booting: nothing listens before the first program, and nothing here
  replays them (devtmpfs makes its nodes by itself).
   python3 patch_bootwork.py K"""
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
    ('''static __always_inline bool free_pages_prepare(struct page *page,
			unsigned int order, bool check_free, fpi_t fpi_flags)
{
''', '''/* ShaderEmu: memblock is handing over its free ranges, whose pages are all as memmap_init_range made them */
bool se_pages_pristine;
#define se_early_pristine() unlikely(se_pages_pristine)

static __always_inline bool free_pages_prepare(struct page *page,
			unsigned int order, bool check_free, fpi_t fpi_flags)
{
'''),
    ('''		for (i = 1; i < (1 << order); i++) {
			if (compound)
				bad += free_tail_pages_check(page, page + i);
''', '''		for (i = se_early_pristine() ? 1 << order : 1; i < (1 << order); i++) {
			if (compound)
				bad += free_tail_pages_check(page, page + i);
'''),
    ('''	/* (memmap_init_range made the pages free, count 0 and not reserved: stores only where that changed) */
	for (loop = 0; loop < nr_pages; loop++, p++) {
''', '''	/* (memmap_init_range made the pages free, count 0 and not reserved: stores only where that changed) */
	for (loop = se_early_pristine() ? nr_pages : 0; loop < nr_pages; loop++, p++) {
'''),
])
patch('mm/memblock.c', [
    ('''	for_each_free_mem_range(i, NUMA_NO_NODE, MEMBLOCK_NONE, &start, &end,
				NULL)
		count += __free_memory_core(start, end);

	return count;
''', '''	/* ShaderEmu: the pages of these ranges are as memmap_init_range made them, flat memory only (patch_memmap.py) */
	{
		extern bool se_pages_pristine;

		se_pages_pristine = IS_ENABLED(CONFIG_FLATMEM) && !IS_ENABLED(CONFIG_DEFERRED_STRUCT_PAGE_INIT);
		for_each_free_mem_range(i, NUMA_NO_NODE, MEMBLOCK_NONE, &start, &end,
					NULL)
			count += __free_memory_core(start, end);
		se_pages_pristine = false;
	}

	return count;
'''),
])
patch('net/ipv4/route.c', [
    ('''	prandom_bytes(ip_idents, (ip_idents_mask + 1) * sizeof(*ip_idents));
''', '''	/* ShaderEmu: left 0 (memblock's memory is cleared): no randomness needed, 300 thousand instructions at boot */
	memset(ip_idents, 0, 0);
'''),
])
patch('lib/kobject_uevent.c', [
    ('''	/* search the kset we belong to */
	top_kobj = kobj;
''', '''	/* ShaderEmu: nothing listens yet, and nothing here replays what was sent */
	if (system_state < SYSTEM_RUNNING)
		return 0;

	/* search the kset we belong to */
	top_kobj = kobj;
'''),
])
print('boot work: pristine pages, no IP id fill, no uevents at boot')
