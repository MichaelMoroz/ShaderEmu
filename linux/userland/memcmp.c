/*
 * memcmp for the image's C library, in place of musl's (toolchain.sh puts it into libc.a), which goes a byte a turn
 * (htop's memcmp was 14.6% of its first core). Here a word a turn, the first pointer on a word and the second's words
 * read whole and shifted where it is elsewhere in a word; the bytes that differ are then found a byte at a time as
 * before. A word read reaches past the range only within the word that holds its last byte. (strcmp a word a turn
 * was slower for tcc, whose 103 thousand calls a compile are short names at different places in a word: 25 to 27
 * instructions a call against musl's byte loop's 21, which stays.)
 */
#include <string.h>
#include <stdint.h>

typedef uint32_t __attribute__((__may_alias__)) W;

int memcmp(const void *vl, const void *vr, size_t n)
{
	const unsigned char *l = vl, *r = vr;

	if (n >= 8) {
		for (; (uintptr_t)l & 3; n--, l++, r++)
			if (*l != *r) return *l - *r;
		unsigned k = (uintptr_t)r & 3;
		if (!k) {
			for (; n >= 4 && *(const W *)l == *(const W *)r; n -= 4, l += 4, r += 4);
		} else {
			const W *p = (const W *)(r - k);
			unsigned sr = 8 * k, sl = 32 - sr;
			W w = p[0];
			for (; n >= 4; n -= 4, l += 4, r += 4) {
				W a = *++p;
				if (*(const W *)l != (w >> sr | a << sl)) break;
				w = a;
			}
		}
	}
	for (; n && *l == *r; n--, l++, r++);
	return n ? *l - *r : 0;
}
