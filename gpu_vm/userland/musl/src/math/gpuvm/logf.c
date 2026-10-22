/* logf on the card's exact float ops (gvm_libm.h): log x = k ln2 + log c + log(1 + r) with r = z/c - 1 an exact pair;
 * about half an ulp, the same bits on gvm_cpu and the card. A denormal x is 0 (the card flushes). */
#include "gvm_libm.h"

float logf(float x)
{
	uint32_t ix = gvm_asu(x);
	if (ix - 0x00800000u >= 0x7f800000u - 0x00800000u) {   /* 0, denormals, negatives, inf, nan */
		if ((ix & 0x7fffffffu) < 0x00800000u)
			return -__builtin_inff();
		if (ix >> 31)
			return __builtin_nanf("");
		return x;                                          /* inf, nan */
	}
	int k;
	float rh, rl;
	const struct gvm_log_entry *e = gvm_log_reduce(ix, __gvm_log_tab, &k, &rh, &rl);
	float r = rh + rl, kf = (float)k, err;
	float hi = gvm_two_sum((kf * gvm_ln2hi + e->hi) + rh, rl, &err);   /* k ln2hi + hi + rh is exact */
	float lo = err + ((kf * gvm_ln2lo + e->lo) + r * r * (-0.5f + r * (gvm_L0 + r * (gvm_L1 + r * gvm_L2))));
	return hi + lo;
}
