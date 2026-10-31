"""musl's mallocng with a group's freed memory kept for its next group (toolchain.sh puts the result into libc.a).

Without an MMU a mapping is a system call of 1,500 to 3,800 instructions and its unmapping of 1,200 to 2,600, with the
pages taken from and given back to the kernel's lists, and a program that frees its last small block of a size and
takes another soon maps a group again and again. So what free would unmap, up to 128 KB, waits in a table of eight for a
group of its length, and is cleared then: mallocng asks a new group to be zero (enframe's and get_nominal_size's
asserts on the bytes before and after a slot, calloc's promise of a large class).
    python3 malloc_keep.py MUSL_SRC_MALLOCNG OUT_DIR"""
import sys
src, out = sys.argv[1:3]


def sub(s, old, new):
    assert s.count(old) == 1, old
    return s.replace(old, new)


m = open(src + '/malloc.c').read()
m = sub(m, '''		p = mmap(0, needed, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
		if (p==MAP_FAILED) {
			free_meta(m);
			return 0;
		}
		m->maplen = needed>>12;
''', '''		p = __malloc_kept_take(needed);
		if (!p) p = mmap(0, needed, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
		if (p==MAP_FAILED) {
			free_meta(m);
			return 0;
		}
		m->maplen = needed>>12;
''')
m = sub(m, 'static struct meta *alloc_group(int sc, size_t req)\n', '''struct __malloc_kept __malloc_kept[8];

/* (under the lock) */
static void *__malloc_kept_take(size_t len)
{
	for (int i=0; i<8; i++) if (__malloc_kept[i].len == len) {
		__malloc_kept[i].len = 0;
		return memset(__malloc_kept[i].base, 0, len);
	}
	return 0;
}

static struct meta *alloc_group(int sc, size_t req)
''')
open(out + '/malloc_keep.c', 'w').write(m)

f = open(src + '/free.c').read()
f = sub(f, '''	wrlock();
	struct mapinfo mi = nontrivial_free(g, idx);
	unlock();
	if (mi.len) {
''', '''	wrlock();
	struct mapinfo mi = nontrivial_free(g, idx);
	if (mi.len && mi.len <= 128<<10) for (int i=0; i<8; i++) if (!__malloc_kept[i].len) {
		__malloc_kept[i].base = mi.base;
		__malloc_kept[i].len = mi.len;
		mi.len = 0;
		break;
	}
	unlock();
	if (mi.len) {
''')
open(out + '/free_keep.c', 'w').write(f)

g = open(src + '/meta.h').read()
g = sub(g, 'struct malloc_context {', '''extern hidden struct __malloc_kept { void *base; size_t len; } __malloc_kept[8];

struct malloc_context {''')
open(out + '/meta.h', 'w').write(g)
