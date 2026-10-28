"""No MMU: a flat program's relocations made a word at a time where they lie (GPU-VM linux/drivers/flatreloc_hook.py).

load_flat_file's loop took 37 bundles a relocation on gpu_vm: the entry and the pointer read and written a byte at a time
(copy_from_user and copy_to_user of 4 bytes: no efficient unaligned access), the user range's bounds loaded again for
every access, calc_reloc called twice. Red Alert's 153,564 relocations were 80% of core 0's bundles from its command
to its first frame (gvm_cpu --profile, 6.6 million bundles). With no MMU the program's memory is the kernel's to read
and write: a pointer on a word inside the program is relocated with one load and one store, the bounds kept in
registers; anything else (a pointer off a word, one outside the program, shared libraries) goes on in the generic
loop from that entry, which says what is wrong as before. Idempotent.   python3 patch_flatreloc.py K"""
import sys
root = sys.argv[1]
MARK = 'shaderemu flatreloc'


def patch(rel, pairs):
    path = root + '/' + rel
    s = open(path).read()
    if MARK in s:
        return
    for old, new in pairs:
        assert s.count(old) == 1, (rel, old)
        s = s.replace(old, new)
    open(path, 'w').write(s)


patch('fs/binfmt_flat.c', [
('''	if (rev > OLD_FLAT_VERSION) {
		for (i = 0; i < relocs; i++) {
			u32 addr, relval;
''', '''	if (rev > OLD_FLAT_VERSION) {
		i = 0;
#ifndef CONFIG_MMU
		/* shaderemu flatreloc: a word at a time where it lies; the generic loop from the first entry it cannot make */
		if (!IS_ENABLED(CONFIG_BINFMT_SHARED_FLAT) && access_ok(reloc, relocs * sizeof(u32))) {
			const __be32 *rl = (const __be32 *)reloc;
			unsigned long sc = libinfo->lib_list[id].start_code;
			unsigned long sd = libinfo->lib_list[id].start_data;
			unsigned long tl = libinfo->lib_list[id].text_len;
			unsigned long lim = libinfo->lib_list[id].start_brk - sd + tl;

			for (; i < relocs; i++) {
				unsigned long r = flat_get_relocate_addr(ntohl(rl[i]));
				u32 *p, a;

				if (r > lim)
					break;
				p = (u32 *)(r < tl ? r + sc : r - tl + sd);
				if ((unsigned long)p & 3)
					break;
				a = *p;
				if (!a)
					continue;
				if ((flags & FLAT_FLAG_GOTPIC) == 0)
					a = ntohl((__force __be32)a);
				if (a > lim)
					break;
				*p = a < tl ? a + sc : a - tl + sd;
			}
		}
#endif
		for (; i < relocs; i++) {
			u32 addr, relval;
'''),
])
print('binfmt_flat: relocations a word at a time')
