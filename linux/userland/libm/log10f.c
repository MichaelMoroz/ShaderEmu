/* log10f in float instructions alone (flm_libm.h): log x as hi + lo (logf's way), times log10 e in two parts.
 * 0, denormals, negatives, inf and nan: musl's generic log10f. */
#include "flm_libm.h"

#define log10f generic_log10f
#include "../log10f.c"
#undef log10f

static const float
IVLN10HI = 0x1.bccp-2f,   /* 12 bits */
IVLN10LO = -0x1.09d5b2p-15f,
IVLN10 = 0x1.bcb7b2p-2f;

float log10f(float x)
{
	uint32_t ix = flm_asu(x);
	if (ix - 0x00800000u >= 0x7f800000u - 0x00800000u)   /* 0, denormals, negatives, inf, nan */
		return generic_log10f(x);
	int k;
	float rh, rl;
	const struct flm_log_entry *e = flm_log_reduce(ix, __flm_log_tab, &k, &rh, &rl);
	float r = rh + rl, kf = (float)k, err;
	float hi = flm_two_sum((kf * flm_ln2hi + e->hi) + rh, rl, &err);
	float lo = err + ((kf * flm_ln2lo + e->lo) + r * r * (-0.5f + r * (flm_L0 + r * (flm_L1 + r * flm_L2))));
	/* (hi + lo) / ln10: hi in two halves of 12 bits, each times IVLN10HI exact */
	float hh = flm_asf(flm_asu(hi) & 0xfffff000u), hl = hi - hh;
	float a = hh * IVLN10HI, b = hl * IVLN10HI + (hi * IVLN10LO + lo * IVLN10);
	return a + b;
}
