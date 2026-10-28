/* sinf in float instructions alone (flm_libm.h): about an ulp. |x| >= 8192,
 * inf and nan: musl's generic sinf (double in software). */
#include "flm_libm.h"

#define sinf generic_sinf
#include "../sinf.c"
#undef sinf

float sinf(float x)
{
	if ((flm_asu(x) & 0x7fffffffu) >= FLM_TRIG_MAX)
		return generic_sinf(x);
	int k;   /* sin is odd: |x| reduced, x's sign given to the result (sin(-0) is -0) */
	float rl, r = flm_trig_reduce(__builtin_fabsf(x), &k, &rl), z = r * r;
	float s = flm_sin_poly(r, rl, z), c = flm_cos_poly(r, rl, z);
	float v = (k & 1) ? c : s;
	return flm_asf(flm_asu(v) ^ ((uint32_t)(k & 2) << 30) ^ (flm_asu(x) & 0x80000000u));
}
