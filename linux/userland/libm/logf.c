/* logf in float instructions alone (flm_libm.h): log x = k ln2 + log c + log(1 + r) with r = z/c - 1 an exact pair;
 * about half an ulp. 0, denormals, negatives, inf and nan: musl's generic logf. */
#include "flm_libm.h"

#define logf generic_logf
#include "../logf.c"
#undef logf

float logf(float x)
{
	uint32_t ix = flm_asu(x);
	if (ix - 0x00800000u >= 0x7f800000u - 0x00800000u)   /* 0, denormals, negatives, inf, nan */
		return generic_logf(x);
	int k;
	float rh, rl;
	const struct flm_log_entry *e = flm_log_reduce(ix, __flm_log_tab, &k, &rh, &rl);
	float r = rh + rl, kf = (float)k, err;
	float hi = flm_two_sum((kf * flm_ln2hi + e->hi) + rh, rl, &err);   /* k ln2hi + hi + rh is exact */
	float lo = err + ((kf * flm_ln2lo + e->lo) + r * r * (-0.5f + r * (flm_L0 + r * (flm_L1 + r * flm_L2))));
	return hi + lo;
}
