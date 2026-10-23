/* gpu_vm (gpu_vm/userland/musl): musl's memcpy with the aligned copy in rounds of 64 bytes */
#include <string.h>
#include <stdint.h>
#include <endian.h>

void *memcpy(void *restrict dest, const void *restrict src, size_t n)
{
	unsigned char *d = dest;
	const unsigned char *s = src;

#ifdef __GNUC__

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define LS >>
#define RS <<
#else
#define LS <<
#define RS >>
#endif

	typedef uint32_t __attribute__((__may_alias__)) u32;
	uint32_t w, x;

	for (; (uintptr_t)s % 4 && n; n--) *d++ = *s++;

	if ((uintptr_t)d % 4 == 0) {
		/* gpu_vm: 64 bytes a round, the loads first (two a bundle), then the stores (two a bundle): about 18 bundles
		   for 64 bytes, where four words a round took 6 bundles for 16 (1024 bytes: 396 bundles to 286) */
		for (size_t k = n / 64; k; k--, s+=64, d+=64) {
			const u32 *S = (const u32 *)s;
			u32 *D = (u32 *)d;
			uint32_t a0 = S[0], a1 = S[1], a2 = S[2], a3 = S[3], a4 = S[4], a5 = S[5], a6 = S[6], a7 = S[7];
			uint32_t a8 = S[8], a9 = S[9], a10 = S[10], a11 = S[11], a12 = S[12], a13 = S[13], a14 = S[14], a15 = S[15];
			D[0] = a0; D[1] = a1; D[2] = a2; D[3] = a3; D[4] = a4; D[5] = a5; D[6] = a6; D[7] = a7;
			D[8] = a8; D[9] = a9; D[10] = a10; D[11] = a11; D[12] = a12; D[13] = a13; D[14] = a14; D[15] = a15;
		}
		n %= 64;
		if (n&32) {
			const u32 *S = (const u32 *)s;
			u32 *D = (u32 *)d;
			uint32_t a0 = S[0], a1 = S[1], a2 = S[2], a3 = S[3], a4 = S[4], a5 = S[5], a6 = S[6], a7 = S[7];
			D[0] = a0; D[1] = a1; D[2] = a2; D[3] = a3; D[4] = a4; D[5] = a5; D[6] = a6; D[7] = a7;
			d += 32; s += 32;
		}
		if (n&16) {
			const u32 *S = (const u32 *)s;
			u32 *D = (u32 *)d;
			uint32_t a0 = S[0], a1 = S[1], a2 = S[2], a3 = S[3];
			D[0] = a0; D[1] = a1; D[2] = a2; D[3] = a3;
			d += 16; s += 16;
		}
		if (n&8) {
			*(u32 *)(d+0) = *(u32 *)(s+0);
			*(u32 *)(d+4) = *(u32 *)(s+4);
			d += 8; s += 8;
		}
		if (n&4) {
			*(u32 *)(d+0) = *(u32 *)(s+0);
			d += 4; s += 4;
		}
		if (n&2) {
			*d++ = *s++; *d++ = *s++;
		}
		if (n&1) {
			*d = *s;
		}
		return dest;
	}

	if (n >= 32) switch ((uintptr_t)d % 4) {
	case 1:
		w = *(u32 *)s;
		*d++ = *s++;
		*d++ = *s++;
		*d++ = *s++;
		n -= 3;
		for (; n>=17; s+=16, d+=16, n-=16) {
			x = *(u32 *)(s+1);
			*(u32 *)(d+0) = (w LS 24) | (x RS 8);
			w = *(u32 *)(s+5);
			*(u32 *)(d+4) = (x LS 24) | (w RS 8);
			x = *(u32 *)(s+9);
			*(u32 *)(d+8) = (w LS 24) | (x RS 8);
			w = *(u32 *)(s+13);
			*(u32 *)(d+12) = (x LS 24) | (w RS 8);
		}
		break;
	case 2:
		w = *(u32 *)s;
		*d++ = *s++;
		*d++ = *s++;
		n -= 2;
		for (; n>=18; s+=16, d+=16, n-=16) {
			x = *(u32 *)(s+2);
			*(u32 *)(d+0) = (w LS 16) | (x RS 16);
			w = *(u32 *)(s+6);
			*(u32 *)(d+4) = (x LS 16) | (w RS 16);
			x = *(u32 *)(s+10);
			*(u32 *)(d+8) = (w LS 16) | (x RS 16);
			w = *(u32 *)(s+14);
			*(u32 *)(d+12) = (x LS 16) | (w RS 16);
		}
		break;
	case 3:
		w = *(u32 *)s;
		*d++ = *s++;
		n -= 1;
		for (; n>=19; s+=16, d+=16, n-=16) {
			x = *(u32 *)(s+3);
			*(u32 *)(d+0) = (w LS 8) | (x RS 24);
			w = *(u32 *)(s+7);
			*(u32 *)(d+4) = (x LS 8) | (w RS 24);
			x = *(u32 *)(s+11);
			*(u32 *)(d+8) = (w LS 8) | (x RS 24);
			w = *(u32 *)(s+15);
			*(u32 *)(d+12) = (x LS 8) | (w RS 24);
		}
		break;
	}
	if (n&16) {
		*d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
		*d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
		*d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
		*d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
	}
	if (n&8) {
		*d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
		*d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
	}
	if (n&4) {
		*d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
	}
	if (n&2) {
		*d++ = *s++; *d++ = *s++;
	}
	if (n&1) {
		*d = *s;
	}
	return dest;
#endif

	for (; n; n--) *d++ = *s++;
	return dest;
}
