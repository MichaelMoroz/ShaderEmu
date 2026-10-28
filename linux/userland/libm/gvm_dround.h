/* floor, ceil, trunc and round of a double by its bits alone (userland/README.md, "libm"): double arithmetic is
 * compiler-rt's software, musl's generic versions add and subtract 2^52 (two of its calls, about 550 bundles); these
 * mask the fraction's bits in a few integer ops, exactly. */
#ifndef GVM_DROUND_H
#define GVM_DROUND_H
#include <stdint.h>

static inline uint64_t gvm_asu64(double d)
{
	union { double d; uint64_t u; } v = { d };
	return v.u;
}

static inline double gvm_asd(uint64_t u)
{
	union { uint64_t u; double d; } v = { u };
	return v.d;
}

/* mode: 0 trunc, 1 floor, 2 ceil, 3 round (half away from zero) */
static inline double gvm_dround(double x, int mode)
{
	uint64_t u = gvm_asu64(x);
	int e = (int)(u >> 52 & 0x7ff) - 0x3ff, neg = (int)(u >> 63);
	if (e >= 52)                                   /* an integer, inf or nan */
		return x;
	if (e < 0) {                                   /* |x| < 1 */
		uint64_t sign = u & 0x8000000000000000ull;
		if ((u << 1) == 0)
			return x;
		if (mode == 1 && neg)
			return -1.0;
		if (mode == 2 && !neg)
			return 1.0;
		if (mode == 3 && e == -1)
			return gvm_asd(sign | 0x3ff0000000000000ull);   /* +-1 */
		return gvm_asd(sign);                      /* +-0 */
	}
	uint64_t m = 0x000fffffffffffffull >> e;
	if ((u & m) == 0)
		return x;
	if (mode == 3)
		u += (m + 1) >> 1;                         /* half: the magnitude rounds away from zero */
	else if ((mode == 1 && neg) || (mode == 2 && !neg))
		u += m;                                    /* away from zero: up to the next integer's magnitude */
	return gvm_asd(u & ~m);
}
#endif
