"""No MMU: user memory checked by its ranges, and a get_user or put_user one access.

- access_ok: a user's range must start in memory the kernel may read: the ROM (0x40000000 up, a mapped file of it is
  a user's pointer there), or the devices and RAM after them (0x80000000 to 0x92000000: a program's
  buffers in the GPU's memory are its pointers too), and end by RAM's end (TASK_SIZE_MAX, which the strings'
  functions ask for). asm-generic's UACCESS_MEMCPY said yes to anything, so a bad pointer given to a system call
  faulted in the kernel or wrote over the firmware; now it is EFAULT. A range of 0 bytes is anywhere.
- get_user, put_user: one load or store (two for 64 bits), aligned or not: the machine makes a misaligned access
  itself, in the general step (experiments/rvc_opt/src/emu.h). UACCESS_MEMCPY made every one a get_unaligned or put_unaligned: a word four byte loads or stores and
  the shifts between.
   python3 patch_uaccess.py K"""
import sys
K = sys.argv[1]
p = K + '/arch/riscv/Kconfig'
s = open(p).read()
old = '\tselect UACCESS_MEMCPY if !MMU\n'
if old in s:
    s = s.replace(old, '')
    open(p, 'w').write(s)
p = K + '/arch/riscv/include/asm/uaccess.h'
s = open(p).read()
MARK = '/* shaderemu: user memory without an MMU */'
old = '#else /* CONFIG_MMU */\n#include <asm-generic/uaccess.h>\n#endif /* CONFIG_MMU */\n'
new = '#else /* CONFIG_MMU */\n' + MARK + r'''
#include <linux/string.h>
#include <linux/types.h>
#include <asm/unaligned.h>

#define SE_ROM_START	0x40000000UL
#define SE_ROM_END	0x80000000UL
#define SE_RAM_START	0x80000000UL	/* the devices (docs/system/nommu.md, "Memory"), then RAM from 0x82000000 */
#define SE_RAM_END	0x92000000UL

/* the strings' functions (strncpy_from_user, strnlen_user) ask for a range from the string to here */
#define TASK_SIZE_MAX	SE_RAM_END

static inline int __access_ok(unsigned long addr, unsigned long size)
{
	/* RAM first, in one comparison when the size is known; nothing at all is always there (musl's writev of
	   {NULL, 0} after its buffer); a range that starts in the ROM may end anywhere up to RAM's end (a string
	   in the ROM is asked for up to there) */
	if (likely(size <= SE_RAM_END - SE_RAM_START && addr - SE_RAM_START <= SE_RAM_END - SE_RAM_START - size))
		return 1;
	return !size || (addr - SE_ROM_START < SE_ROM_END - SE_ROM_START && size <= SE_RAM_END - addr);
}
#define __access_ok __access_ok

static __always_inline int __get_user_fn(size_t size, const void __user *from, void *to)
{
	const void *p = (const void __force *)from;

	switch (size) {
	case 1:
		*(u8 *)to = *(const u8 *)p;
		return 0;
	case 2:
		*(u16 *)to = *(const u16 *)p;
		return 0;
	case 4:
		*(u32 *)to = *(const u32 *)p;
		return 0;
	case 8:
		*(u64 *)to = ((const u32 *)p)[0] | (u64)((const u32 *)p)[1] << 32;
		return 0;
	}
	return -EFAULT;
}
#define __get_user_fn(sz, u, k)	__get_user_fn(sz, u, k)

static __always_inline int __put_user_fn(size_t size, void __user *to, void *from)
{
	void *p = (void __force *)to;

	switch (size) {
	case 1:
		*(u8 *)p = *(u8 *)from;
		return 0;
	case 2:
		*(u16 *)p = *(u16 *)from;
		return 0;
	case 4:
		*(u32 *)p = *(u32 *)from;
		return 0;
	case 8:
		((u32 *)p)[0] = (u32)*(u64 *)from;
		((u32 *)p)[1] = (u32)(*(u64 *)from >> 32);
		return 0;
	}
	return -EFAULT;
}
#define __put_user_fn(sz, u, k)	__put_user_fn(sz, u, k)

/* kernel addresses (copy_from_kernel_nofault): as asm-generic's UACCESS_MEMCPY makes them */
#define __get_kernel_nofault(dst, src, type, err_label)			\
do {									\
	*((type *)dst) = get_unaligned((type *)(src));			\
	if (0)								\
		goto err_label;						\
} while (0)

#define __put_kernel_nofault(dst, src, type, err_label)			\
do {									\
	put_unaligned(*((type *)src), (type *)(dst));			\
	if (0)								\
		goto err_label;						\
} while (0)

#define HAVE_GET_KERNEL_NOFAULT 1

static inline __must_check unsigned long raw_copy_from_user(void *to, const void __user *from, unsigned long n)
{
	memcpy(to, (const void __force *)from, n);
	return 0;
}

static inline __must_check unsigned long raw_copy_to_user(void __user *to, const void *from, unsigned long n)
{
	memcpy((void __force *)to, from, n);
	return 0;
}
#define INLINE_COPY_FROM_USER
#define INLINE_COPY_TO_USER

#include <asm-generic/uaccess.h>
#endif /* CONFIG_MMU */
'''
if MARK not in s:
    assert s.count(old) == 1, 'uaccess.h: the no-MMU branch is not as expected'
    s = s.replace(old, new)
    open(p, 'w').write(s)
print('uaccess patched')
