/* sincosf in float instructions alone (gvm_libm.h): one reduction for both. |x| >= 8192, inf and nan: musl's
 * generic sincosf. */
#define _GNU_SOURCE
#include "gvm_libm.h"

#ifdef GVM_HOST
#define generic_sincosf gvm_host_sincosf
#else
#define sincosf generic_sincosf
#include "../sincosf.c"
#undef sincosf
#endif

void sincosf(float x, float *sin, float *cos)
{
	if ((gvm_asu(x) & 0x7fffffffu) >= GVM_TRIG_MAX) {
		generic_sincosf(x, sin, cos);
		return;
	}
	int k;   /* |x| reduced: sin takes x's sign, as sinf */
	float rl, r = gvm_trig_reduce(__builtin_fabsf(x), &k, &rl), z = r * r;
	float s = gvm_sin_poly(r, rl, z), c = gvm_cos_poly(r, rl, z);
	float vs = (k & 1) ? c : s, vc = (k & 1) ? s : c;
	*sin = gvm_asf(gvm_asu(vs) ^ ((uint32_t)(k & 2) << 30) ^ (gvm_asu(x) & 0x80000000u));
	*cos = gvm_asf(gvm_asu(vc) ^ ((uint32_t)((k + 1) & 2) << 30));
}
