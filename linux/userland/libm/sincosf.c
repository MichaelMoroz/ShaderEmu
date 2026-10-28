/* sincosf in float instructions alone (flm_libm.h): one reduction for both. |x| >= 8192, inf and nan: musl's
 * generic sincosf. */
#define _GNU_SOURCE
#include "flm_libm.h"

#define sincosf generic_sincosf
#include "../sincosf.c"
#undef sincosf

void sincosf(float x, float *sin, float *cos)
{
	if ((flm_asu(x) & 0x7fffffffu) >= FLM_TRIG_MAX) {
		generic_sincosf(x, sin, cos);
		return;
	}
	int k;   /* |x| reduced: sin takes x's sign, as sinf */
	float rl, r = flm_trig_reduce(__builtin_fabsf(x), &k, &rl), z = r * r;
	float s = flm_sin_poly(r, rl, z), c = flm_cos_poly(r, rl, z);
	float vs = (k & 1) ? c : s, vc = (k & 1) ? s : c;
	*sin = flm_asf(flm_asu(vs) ^ ((uint32_t)(k & 2) << 30) ^ (flm_asu(x) & 0x80000000u));
	*cos = flm_asf(flm_asu(vc) ^ ((uint32_t)((k + 1) & 2) << 30));
}
