/* cosf in float instructions alone (flm_libm.h): about an ulp. |x| >= 8192,
 * inf and nan: musl's generic cosf. */
#include "flm_libm.h"

#define cosf generic_cosf
#include "../cosf.c"
#undef cosf

float cosf(float x)
{
	if ((flm_asu(x) & 0x7fffffffu) >= FLM_TRIG_MAX)
		return generic_cosf(x);
	int k;   /* cos is even: |x| reduced */
	float rl, r = flm_trig_reduce(__builtin_fabsf(x), &k, &rl), z = r * r;
	float s = flm_sin_poly(r, rl, z), c = flm_cos_poly(r, rl, z);
	float v = (k & 1) ? s : c;
	return flm_asf(flm_asu(v) ^ ((uint32_t)((k + 1) & 2) << 30));
}
