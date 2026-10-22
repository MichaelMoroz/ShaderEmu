/* exp2f on the card's exact float ops (gvm_libm.h): 2^n 2^r, n = round(x), r = x - n exactly; about an ulp, the same
 * bits on gvm_cpu and the card. Results below 2^-126 are 0 (the card flushes denormals). */
#include "gvm_libm.h"

float exp2f(float x)
{
	if (!(__builtin_fabsf(x) < 150.0f))   /* inf, nan, and beyond the range */
		return x != x ? x : x > 0 ? x * 0x1p127f * 0x1p127f : 0.0f;
	float n = __builtin_roundevenf(x);
	return gvm_scale(gvm_exp2_poly(x - n), (int)n);
}
