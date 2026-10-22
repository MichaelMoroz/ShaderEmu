/* powf on the card's exact float ops (gvm_libm.h): 2^(y log2 x), log2 x as hi + lo (about 2^-36 relative), y times
 * it by Dekker's exact product, 2^ of the pair as exp2f makes it; about an ulp, the same bits on gvm_cpu and the card.
 * A negative x with an integer y is made from |x|; zeros, denormals, inf, nan, |y| >= 2^30 and a negative x with a
 * fraction in y: musl's generic powf. */
#include "gvm_libm.h"

#ifdef GVM_HOST
#define generic_powf gvm_host_powf
#else
#define powf generic_powf
#include "../powf.c"
#undef powf
#endif

/* a = ah + al, each of 12 bits at most */
static inline void split(float a, float *ah, float *al)
{
	float c = a * 4097.0f;
	*ah = c - (c - a);
	*al = a - *ah;
}

float powf(float x, float y)
{
	uint32_t ix = gvm_asu(x), iy = gvm_asu(y);
	uint32_t sign = 0;
	if (ix - 0x00800000u >= 0x7f800000u - 0x00800000u || (iy & 0x7fffffffu) >= 0x4e800000u) {
		/* a negative normal x with an integer y (|y| >= 2^24 is even) */
		if (ix - 0x80800000u < 0x7f000000u && (iy & 0x7fffffffu) < 0x4e800000u && y == __builtin_truncf(y)) {
			if ((iy & 0x7fffffffu) < 0x4b800000u)
				sign = (uint32_t)((int)y & 1) << 31;
			ix &= 0x7fffffffu;
		} else
			return generic_powf(x, y);
	}
	if ((iy & 0x7fffffffu) < 0x00800000u)   /* y 0 (or a denormal, which is 0 to the card) */
		return 1.0f;
	float l, h = gvm_log2_hl(ix, &l);
	/* y (h + l) = p + pl: p = y h rounded, its error exactly (Dekker) */
	float yh, yl, hh, hl;
	split(y, &yh, &yl);
	split(h, &hh, &hl);
	float p = y * h;
	float pl = ((((yh * hh - p) + yh * hl) + yl * hh) + yl * hl) + y * l;
	if (!(__builtin_fabsf(p) < 150.0f))
		return gvm_asf(gvm_asu(p > 0 ? 0x1p127f * 0x1p127f : 0.0f) | sign);
	float n = __builtin_roundevenf(p);
	float r = (p - n) + pl;
	return gvm_asf(gvm_asu(gvm_scale(gvm_exp2_poly(r), (int)n)) | sign);
}
