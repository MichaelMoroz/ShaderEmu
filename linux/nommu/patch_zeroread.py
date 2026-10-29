"""No MMU: a read of /dev/zero clears the reader's buffer up to a megabyte at a time, not a page, and a write does
not touch its buffer's pages first.

From 4 KB up the kernel's memset is the machine's parallel fill (memset_hook.py), and a page a step asked for one a
page: the page was "just for latency reasons", and a fill of a megabyte takes no longer to ask for than one of a
page. A write read a byte of each page of its buffer first (fault_in_readable, from generic_perform_write) to fault
it in: with no MMU a buffer that passes access_ok has no page to fault in.   python3 patch_zeroread.py K"""
import sys
K = sys.argv[1]
MARK = 'shaderemu zeroread'


def patch(rel, pairs):
    path = K + '/' + rel
    s = open(path).read()
    if MARK in s:
        return
    for old, new in pairs:
        assert s.count(old) == 1, (rel, old)
        s = s.replace(old, new)
    open(path, 'w').write(s)


patch('mm/gup.c', [
('''	if (!user_read_access_begin(uaddr, size))
		return size;
	if (!PAGE_ALIGNED(uaddr)) {
		unsafe_get_user(c, uaddr, out);
''', '''	if (!user_read_access_begin(uaddr, size))
		return size;
	if (!IS_ENABLED(CONFIG_MMU)) {	/* shaderemu zeroread: no page to fault in */
		user_read_access_end();
		return 0;
	}
	if (!PAGE_ALIGNED(uaddr)) {
		unsafe_get_user(c, uaddr, out);
'''),
])
patch('drivers/char/mem.c', [
('''		if (chunk > PAGE_SIZE)
			chunk = PAGE_SIZE;	/* Just for latency reasons */
''', '''		if (chunk > (1 << 20))
			chunk = (1 << 20);	/* shaderemu zeroread: one memset, one parallel fill */
'''),
('''		size_t chunk = min_t(size_t, count, PAGE_SIZE);
		size_t left;

		left = clear_user(buf + cleared, chunk);
''', '''		size_t chunk = min_t(size_t, count, (1 << 20));	/* shaderemu zeroread */
		size_t left;

		left = clear_user(buf + cleared, chunk);
'''),
])
print('/dev/zero a megabyte a step, no fault-in of a write\'s buffer')
