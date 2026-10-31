"""No MMU: a directory of the ROM listed where it lies, as romfs_hook.py's lookup does. romfs_readdir read each entry's
head through romfs_dev_read, measured its name a byte group at a time and read that again (some 700 instructions an
entry with its copies, 33 thousand a listing of /usr/share; 2.5% of a desktop's start, 3.3% of the IDE's).
Idempotent.   python3 patch_romfs.py K"""
import sys
p = sys.argv[1] + '/fs/romfs/super.c'
s = open(p).read()
if 'shaderemu romfs_readdir' not in s:
    old = '''	/* Not really failsafe, but we are read-only... */
	for (;;) {
		if (!offset || offset >= maxoff) {
			offset = maxoff;
			ctx->pos = offset;
			goto out;
		}
		ctx->pos = offset;

		/* Fetch inode info */
'''
    new = '''#ifdef CONFIG_ROMFS_ON_MTD
	{
		/* shaderemu romfs_readdir: the entries where they lie in the ROM */
		size_t size;
		const u_char *rom = romfs_rom(i->i_sb, &size);

		while (rom && offset && offset < maxoff && offset + ROMFH_SIZE + sizeof(fsname) <= size) {
			const u_char *entry = rom + offset;
			u32 next = be32_to_cpup((const __be32 *)entry);

			ctx->pos = offset;
			ino = (next & ROMFH_TYPE) == ROMFH_HRD ? be32_to_cpup((const __be32 *)entry + 1) : offset;
			if (!dir_emit(ctx, (const char *)entry + ROMFH_SIZE, strnlen((const char *)entry + ROMFH_SIZE, sizeof(fsname) - 1),
				      ino, romfs_dtype_table[next & ROMFH_TYPE]))
				goto out;
			offset = next & ROMFH_MASK;
		}
	}
#endif

''' + old
    assert s.count(old) == 1
    open(p, 'w', newline='\n').write(s.replace(old, new))

# A name looked up in its directory's table of names, which the image has (slim.py, a directory of 32 entries or more:
# its header's size is where the table lies): the entries are a list in the ROM, and /usr/bin or /usr/share walked to the
# end, a name that is not there too (the shell's PATH), was 4,100 instructions a lookup: 3.6% of a desktop's start, 4% of
# nxweb's page, 2.2% of nxview's picture.
s = open(p).read()
if 'shaderemu romfs_hashed' not in s:
    old = """		while (rom && len > 0 && offset && offset < maxoff && offset + ROMFH_SIZE + len + 1 <= size) {
			const u_char *entry = rom + offset;
			u32 next = be32_to_cpup((const __be32 *)entry);

			if (entry[ROMFH_SIZE] == (u_char)name[0]"""
    assert s.count(old) == 1
    s = s.replace(old, """		u32 table = be32_to_cpu(ri.size);	/* shaderemu romfs_hashed */

		if (rom && len > 0 && table && !(table & 3) && table + 8 <= size) {
			const u32 *tab = (const u32 *)(rom + table);
			u32 mask = tab[0], h = 0;
			int k;

			for (k = 0; k < len; k++)
				h = h * 31 + (u_char)name[k];
			for (tab++;; h++) {
				const u_char *e;

				offset = tab[h & mask];
				if (!offset)
					break;
				e = rom + offset + ROMFH_SIZE;
				if (offset + ROMFH_SIZE + len + 1 <= size && !e[len] && !memcmp(e, name, len))
					break;
			}
		}
""" + old)
    open(p, 'w', newline='\n').write(s)
print('romfs: a directory listed where it lies, a name found by its table')
