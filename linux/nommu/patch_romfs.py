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
print('romfs: a directory listed where it lies')
