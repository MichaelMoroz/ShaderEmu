/*
 * memmove for the image's C library, in place of musl's (toolchain.sh puts it into libc.a). Ranges that do not
 * overlap are memcpy's. musl's own goes by words only when both pointers are at one place in a word, and byte by
 * byte otherwise: OpenTTD's scroll moves its rows by any number of pixels, and that byte loop was 15.5% of its first
 * core's instructions. Here the destination is put on a word and the source's words are read whole, each word
 * stored made of two by shifts; a word read reaches past the range only within the word that holds its last byte.
 * The words are read ahead of the stores that could overwrite them, in either direction.
 */
#include <string.h>
#include <stdint.h>

typedef uint32_t __attribute__((__may_alias__)) W;

void *memmove(void *dest, const void *src, size_t n)
{
	unsigned char *d = dest;
	const unsigned char *s = src;

	if (d == s) return d;
	if ((uintptr_t)s - (uintptr_t)d - n <= -2 * n) return memcpy(d, s, n);

	if (d < s) {
		if (n >= 8) {
			while ((uintptr_t)d & 3) *d++ = *s++, n--;
			unsigned k = (uintptr_t)s & 3;
			if (!k) {
				for (; n >= 16; n -= 16, d += 16, s += 16) {
					W a = ((const W *)s)[0], b = ((const W *)s)[1], c = ((const W *)s)[2], e = ((const W *)s)[3];
					((W *)d)[0] = a, ((W *)d)[1] = b, ((W *)d)[2] = c, ((W *)d)[3] = e;
				}
				for (; n >= 4; n -= 4, d += 4, s += 4) *(W *)d = *(const W *)s;
			} else {
				const W *p = (const W *)(s - k);
				unsigned r = 8 * k, l = 32 - r;
				W w = p[0];
				for (; n >= 16; n -= 16, d += 16, s += 16, p += 4) {
					W a = p[1], b = p[2], c = p[3], e = p[4];
					((W *)d)[0] = w >> r | a << l;
					((W *)d)[1] = a >> r | b << l;
					((W *)d)[2] = b >> r | c << l;
					((W *)d)[3] = c >> r | e << l;
					w = e;
				}
				for (; n >= 4; n -= 4, d += 4, s += 4) {
					W a = *++p;
					*(W *)d = w >> r | a << l;
					w = a;
				}
			}
		}
		for (; n; n--) *d++ = *s++;
	} else {
		if (n >= 8) {
			while ((uintptr_t)(d + n) & 3) n--, d[n] = s[n];
			unsigned k = (uintptr_t)(s + n) & 3;
			if (!k) {
				for (; n >= 16; ) {
					n -= 16;
					W a = ((const W *)(s + n))[0], b = ((const W *)(s + n))[1], c = ((const W *)(s + n))[2], e = ((const W *)(s + n))[3];
					((W *)(d + n))[3] = e, ((W *)(d + n))[2] = c, ((W *)(d + n))[1] = b, ((W *)(d + n))[0] = a;
				}
				for (; n >= 4; ) n -= 4, *(W *)(d + n) = *(const W *)(s + n);
			} else {
				/* q: the word that holds the range's last byte; a word stored at d + n - 4 is made of q[-1] and q[0] */
				const W *q = (const W *)(s + n - k);
				unsigned r = 8 * k, l = 32 - r;
				W w = q[0];
				for (; n >= 16; q -= 4) {
					n -= 16;
					W a = q[-1], b = q[-2], c = q[-3], e = q[-4];
					((W *)(d + n))[3] = a >> r | w << l;
					((W *)(d + n))[2] = b >> r | a << l;
					((W *)(d + n))[1] = c >> r | b << l;
					((W *)(d + n))[0] = e >> r | c << l;
					w = e;
				}
				for (; n >= 4; ) {
					n -= 4;
					W a = *--q;
					*(W *)(d + n) = a >> r | w << l;
					w = a;
				}
			}
		}
		while (n) n--, d[n] = s[n];
	}
	return dest;
}
