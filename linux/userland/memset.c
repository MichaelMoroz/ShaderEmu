/*
 * memset for the image's C library, in place of musl's (toolchain.sh puts it into libc.a): musl's own,
 * with fills from 2 KB up made by the commit (uop.h; docs/machine/gpu.md, "A program's copy"): the
 * texels between the ends never enter the core's write cache. Asking ends the pass.
 */
#include <string.h>
#include <stdint.h>

#define UOP_MIN 2048   /* (uop.h) */

/* [a, a + n) in RAM: below the GPU's memory's end, or above the device words */
static inline int uop_in_ram(uintptr_t a, size_t n)
{
	uintptr_t t = a - 0x80000000u, e = t + n;
	if (e < t) return 0;
	return t < 0x07e00000u ? e <= 0x07e00000u : t >= 0x08000000u && e <= 0x12000000u;
}

void *memset(void *dest, int c, size_t n)
{
	unsigned char *s = dest;
	size_t k;

	if (n >= UOP_MIN) {
		uint32_t worth;
		__asm__ __volatile__ ("csrr %0, 0x8b0" : "=r"(worth));
		if (worth && uop_in_ram((uintptr_t)s, n)) {
			for (k = -(uintptr_t)s & 15; k; k--, n--) *s++ = c;
			uint32_t word = 0x01010101u * (unsigned char)c;
			size_t mid = n & ~(size_t)15;
			__asm__ __volatile__ ("csrw 0x8b1, %0\n\tcsrw 0x8b2, %1\n\tcsrw 0x8b3, %2\n\tcsrw 0x8b0, %3"
			                      : : "r"(word), "r"(s), "r"(mid), "r"(2) : "memory");
			s += mid;
			for (n &= 15; n; n--) *s++ = c;
			return dest;
		}
	}

	/* Fill head and tail with minimal branching. Each
	 * conditional ensures that all the subsequently used
	 * offsets are well-defined and in the dest region. */

	if (!n) return dest;
	s[0] = c;
	s[n-1] = c;
	if (n <= 2) return dest;
	s[1] = c;
	s[2] = c;
	s[n-2] = c;
	s[n-3] = c;
	if (n <= 6) return dest;
	s[3] = c;
	s[n-4] = c;
	if (n <= 8) return dest;

	/* Advance pointer to align it at a 4-byte boundary,
	 * and truncate n to a multiple of 4. The previous code
	 * already took care of any head/tail that get cut off
	 * by the alignment. */

	k = -(uintptr_t)s & 3;
	s += k;
	n -= k;
	n &= -4;

	typedef uint32_t __attribute__((__may_alias__)) u32;
	typedef uint64_t __attribute__((__may_alias__)) u64;

	u32 c32 = ((u32)-1)/255 * (unsigned char)c;

	/* In preparation to copy 32 bytes at a time, aligned on
	 * an 8-byte bounary, fill head/tail up to 28 bytes each.
	 * As in the initial byte-based head/tail fill, each
	 * conditional below ensures that the subsequent offsets
	 * are valid (e.g. !(n<=24) implies n>=28). */

	*(u32 *)(s+0) = c32;
	*(u32 *)(s+n-4) = c32;
	if (n <= 8) return dest;
	*(u32 *)(s+4) = c32;
	*(u32 *)(s+8) = c32;
	*(u32 *)(s+n-12) = c32;
	*(u32 *)(s+n-8) = c32;
	if (n <= 24) return dest;
	*(u32 *)(s+12) = c32;
	*(u32 *)(s+16) = c32;
	*(u32 *)(s+20) = c32;
	*(u32 *)(s+24) = c32;
	*(u32 *)(s+n-28) = c32;
	*(u32 *)(s+n-24) = c32;
	*(u32 *)(s+n-20) = c32;
	*(u32 *)(s+n-16) = c32;

	/* Align to a multiple of 8 so we can fill 64 bits at a time,
	 * and avoid writing the same bytes twice as much as is
	 * practical without introducing additional branching. */

	k = 24 + ((uintptr_t)s & 4);
	s += k;
	n -= k;

	/* If this loop is reached, 28 tail bytes have already been
	 * filled, so any remainder when n drops below 32 can be
	 * safely ignored. */

	u64 c64 = c32 | ((u64)c32 << 32);
	for (; n >= 32; n-=32, s+=32) {
		*(u64 *)(s+0) = c64;
		*(u64 *)(s+8) = c64;
		*(u64 *)(s+16) = c64;
		*(u64 *)(s+24) = c64;
	}

	return dest;
}
