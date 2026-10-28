/* logf in float instructions alone (gvm_libm.h): log x = k ln2 + log c + log(1 + r) with r = z/c - 1 an exact pair;
 * about half an ulp. 0, denormals, negatives, inf and nan: musl's generic logf. */
#include "gvm_libm.h"

#ifdef GVM_HOST
#define generic_logf gvm_host_logf
#else
#define logf generic_logf
#include "../logf.c"
#undef logf
#endif

float logf(float x)
{
	uint32_t ix = gvm_asu(x);
	if (ix - 0x00800000u >= 0x7f800000u - 0x00800000u)   /* 0, denormals, negatives, inf, nan */
		return generic_logf(x);
	int k;
	float rh, rl;
	const struct gvm_log_entry *e = gvm_log_reduce(ix, __gvm_log_tab, &k, &rh, &rl);
	float r = rh + rl, kf = (float)k, err;
	float hi = gvm_two_sum((kf * gvm_ln2hi + e->hi) + rh, rl, &err);   /* k ln2hi + hi + rh is exact */
	float lo = err + ((kf * gvm_ln2lo + e->lo) + r * r * (-0.5f + r * (gvm_L0 + r * (gvm_L1 + r * gvm_L2))));
	return hi + lo;
}
