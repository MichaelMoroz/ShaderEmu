/* cosf on the card's exact float ops (gvm_libm.h): about an ulp, the same bits on gvm_cpu and the card. |x| >= 8192,
 * inf and nan: musl's generic cosf. */
#include "gvm_libm.h"

#ifdef GVM_HOST
#define generic_cosf gvm_host_cosf
#else
#define cosf generic_cosf
#include "../cosf.c"
#undef cosf
#endif

float cosf(float x)
{
	if ((gvm_asu(x) & 0x7fffffffu) >= GVM_TRIG_MAX)
		return generic_cosf(x);
	int k;
	float rl, r = gvm_trig_reduce(x, &k, &rl), z = r * r;
	float s = gvm_sin_poly(r, rl, z), c = gvm_cos_poly(r, rl, z);
	float v = (k & 1) ? s : c;
	return gvm_asf(gvm_asu(v) ^ ((uint32_t)((k + 1) & 2) << 30));
}
