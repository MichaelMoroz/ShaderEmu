"""No MMU: tmpfs is ramfs, the writable layer of the root's overlay. It gets RENAME_WHITEOUT and trusted.*
xattrs (as shmem has them), so overlayfs can rename and remove what comes from the ROM, folders too."""
import sys
K = sys.argv[1]


def change(path, old, new, mark):
    p = f'{K}/{path}'
    s = open(p).read()
    if mark in s:
        return
    assert s.count(old) == 1, (path, old[:40])
    open(p, 'w').write(s.replace(old, new))


F = 'fs/ramfs/inode.c'
change(F, '#include <linux/seq_file.h>\n', '#include <linux/seq_file.h>\n#include <linux/xattr.h>\n', '<linux/xattr.h>')
RENAME = '''
/* As shmem_rename2: a whiteout is left where the old name was, hashed while the old dentry is still there */
static int ramfs_rename(struct user_namespace *mnt_userns, struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry, unsigned int flags)
{
	if (flags & RENAME_WHITEOUT) {
		struct dentry *whiteout;
		int error;

		if (flags & RENAME_EXCHANGE)
			return -EINVAL;
		if (!simple_empty(new_dentry))
			return -ENOTEMPTY;
		whiteout = d_alloc(old_dentry->d_parent, &old_dentry->d_name);
		if (!whiteout)
			return -ENOMEM;
		error = ramfs_mknod(&init_user_ns, old_dir, whiteout, S_IFCHR | WHITEOUT_MODE, WHITEOUT_DEV);
		dput(whiteout);
		if (error)
			return error;
		d_rehash(whiteout);
		flags &= ~RENAME_WHITEOUT;
	}
	return simple_rename(mnt_userns, old_dir, old_dentry, new_dir, new_dentry, flags);
}

/* trusted.* xattrs (overlayfs's marks): i_private holds an inode's list from the first one set */
static int ramfs_xattr_get(const struct xattr_handler *handler, struct dentry *unused, struct inode *inode,
			   const char *name, void *buffer, size_t size)
{
	struct simple_xattrs *xattrs = smp_load_acquire(&inode->i_private);

	return xattrs ? simple_xattr_get(xattrs, xattr_full_name(handler, name), buffer, size) : -ENODATA;
}

static int ramfs_xattr_set(const struct xattr_handler *handler, struct user_namespace *mnt_userns,
			   struct dentry *unused, struct inode *inode, const char *name, const void *value,
			   size_t size, int flags)
{
	struct simple_xattrs *xattrs = inode->i_private;

	if (!xattrs) {
		if (!value)
			return -ENODATA;
		xattrs = kmalloc(sizeof(*xattrs), GFP_KERNEL);
		if (!xattrs)
			return -ENOMEM;
		simple_xattrs_init(xattrs);
		smp_store_release(&inode->i_private, xattrs);
	}
	return simple_xattr_set(xattrs, xattr_full_name(handler, name), value, size, flags, NULL);
}

static const struct xattr_handler ramfs_trusted_xattr_handler = {
	.prefix = XATTR_TRUSTED_PREFIX,
	.get = ramfs_xattr_get,
	.set = ramfs_xattr_set,
};

static const struct xattr_handler *ramfs_xattr_handlers[] = {
	&ramfs_trusted_xattr_handler,
	NULL
};

static void ramfs_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	if (inode->i_private) {
		simple_xattrs_free(inode->i_private);
		kfree(inode->i_private);
	}
}

static const struct inode_operations ramfs_dir_inode_operations = {'''
change(F, '\nstatic const struct inode_operations ramfs_dir_inode_operations = {', RENAME, 'ramfs_rename(struct')
change(F, '\t.rename\t\t= simple_rename,\n\t.tmpfile\t= ramfs_tmpfile,', '\t.rename\t\t= ramfs_rename,\n\t.tmpfile\t= ramfs_tmpfile,',
       '.rename\t\t= ramfs_rename,')
change(F, '\t.drop_inode\t= generic_delete_inode,\n\t.show_options\t= ramfs_show_options,',
       '\t.drop_inode\t= generic_delete_inode,\n\t.evict_inode\t= ramfs_evict_inode,\n\t.show_options\t= ramfs_show_options,',
       '.evict_inode\t= ramfs_evict_inode,')
change(F, '\tsb->s_op\t\t= &ramfs_ops;\n', '\tsb->s_op\t\t= &ramfs_ops;\n\tsb->s_xattr\t\t= ramfs_xattr_handlers;\n',
       'ramfs_xattr_handlers;\n')
# A file truncated up from nothing gets its pages in one run, so that it can be mapped shared and written in place (the
# installers' unpacking, linux/apps/cabinst.c: workers write a 64 MB pak straight into its pages). The kernel's way is one
# buddy block, at most MAX_ORDER - 1 (4 MB); larger takes the first run of free pages (alloc_pages_run, patch_drivers.py),
# pages one by one as split_page leaves them.
N = 'fs/ramfs/file-nommu.c'
change(N, '''	/* make various checks */
	order = get_order(newsize);
	if (unlikely(order >= MAX_ORDER))
		return -EFBIG;
''', '''	/* make various checks (ShaderEmu: a run of pages past the largest block) */
	order = get_order(newsize);
	if (unlikely(order >= MAX_ORDER) && newsize > ((size_t)256 << 20))
		return -EFBIG;
''', 'ShaderEmu: a run of pages')
change(N, '''	pages = alloc_pages(gfp, order);
	if (!pages)
		return -ENOMEM;

	/* split the high-order page into an array of single pages */
	xpages = 1UL << order;
	npages = (newsize + PAGE_SIZE - 1) >> PAGE_SHIFT;

	split_page(pages, order);
''', '''	npages = (newsize + PAGE_SIZE - 1) >> PAGE_SHIFT;
	pages = order < MAX_ORDER ? alloc_pages(gfp | __GFP_NOWARN, order) : NULL;
	if (pages) {
		/* split the high-order page into an array of single pages */
		xpages = 1UL << order;
		split_page(pages, order);
	} else {
		/* larger than a block, or no block that large free: a run */
		void *run = alloc_pages_run(npages);

		if (!run)
			return -ENOMEM;
		pages = virt_to_page(run);
		xpages = npages;
	}
''', 'void *run = alloc_pages_run(npages);')
change(N, 'int ramfs_nommu_expand_for_mapping(struct inode *inode, size_t newsize)\n',
       'void *alloc_pages_run(unsigned long nr);\n\nint ramfs_nommu_expand_for_mapping(struct inode *inode, size_t newsize)\n',
       'void *alloc_pages_run(unsigned long nr);\n\nint ramfs_nommu')
print('ramfs: whiteouts and trusted xattrs, a large file in one run of pages')
