/* asinf: musl's (FreeBSD's e_asinf.c) with its double sqrt (compiler-rt's software) replaced by
 * the float one and fdlibm's split of the root (e_asin.c's way for |x| near 1): about an ulp, float ops alone. */
#include "flm_libm.h"

static const float
pio2_hi = 1.5707962513e+00f, /* 0x3fc90fda */
pio2_lo = 7.5497894159e-08f, /* 0x33a22168 */
pio4_hi = 0.785398125648f,   /* 0x3f490fda */
pS0 = 1.6666586697e-01f,
pS1 = -4.2743422091e-02f,
pS2 = -8.6563630030e-03f,
qS1 = -7.0662963390e-01f;

static float R(float z)
{
	float p = z * (pS0 + z * (pS1 + z * pS2)), q = 1.0f + z * qS1;
	return p / q;
}

float asinf(float x)
{
	union { float f; uint32_t i; } u = { x };
	uint32_t hx = u.i, ix = hx & 0x7fffffff;
	if (ix >= 0x3f800000) {   /* |x| >= 1 */
		if (ix == 0x3f800000)
			return x * pio2_hi + x * pio2_lo;
		return 0 / (x - x);
	}
	if (ix < 0x3f000000) {    /* |x| < 0.5 */
		if (ix < 0x39800000)
			return x;
		return x + x * R(x * x);
	}
	/* 1 > |x| >= 0.5: asin = pi/2 - 2 asin(sqrt((1 - |x|) / 2)), the root s = df + c exactly to float's
	 * precision twice over */
	float z = (1 - __builtin_fabsf(x)) * 0.5f, s = flm_sqrtf(z), df, c, p, q;
	union { float f; uint32_t i; } v = { s };
	v.i &= 0xfffff000;
	df = v.f;
	c = (z - df * df) / (s + df);
	p = 2 * s * R(z) - (pio2_lo - 2 * c);
	q = pio4_hi - 2 * df;
	x = pio4_hi - (p - q);
	return (hx >> 31) ? -x : x;
}
