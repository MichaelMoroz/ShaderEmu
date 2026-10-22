/* sinf on the card's exact float ops (gvm_libm.h): about an ulp, the same bits on gvm_cpu and the card. |x| >= 8192,
 * inf and nan: musl's generic sinf (double in software). */
#include "gvm_libm.h"

#ifdef GVM_HOST
#define generic_sinf gvm_host_sinf
#else
#define sinf generic_sinf
#include "../sinf.c"
#undef sinf
#endif

float sinf(float x)
{
	if ((gvm_asu(x) & 0x7fffffffu) >= GVM_TRIG_MAX)
		return generic_sinf(x);
	int k;   /* sin is odd: |x| reduced, x's sign given to the result (sin(-0) is -0) */
	float rl, r = gvm_trig_reduce(__builtin_fabsf(x), &k, &rl), z = r * r;
	float s = gvm_sin_poly(r, rl, z), c = gvm_cos_poly(r, rl, z);
	float v = (k & 1) ? c : s;
	return gvm_asf(gvm_asu(v) ^ ((uint32_t)(k & 2) << 30) ^ (gvm_asu(x) & 0x80000000u));
}
