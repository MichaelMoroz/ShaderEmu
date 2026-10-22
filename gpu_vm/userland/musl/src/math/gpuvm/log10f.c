/* log10f on the card's exact float ops (gvm_libm.h): log x as hi + lo (logf's way), times log10 e in two parts. */
#include "gvm_libm.h"

static const float
IVLN10HI = 0x1.bccp-2f,   /* 12 bits */
IVLN10LO = -0x1.09d5b2p-15f,
IVLN10 = 0x1.bcb7b2p-2f;

float log10f(float x)
{
	uint32_t ix = gvm_asu(x);
	if (ix - 0x00800000u >= 0x7f800000u - 0x00800000u) {
		if ((ix & 0x7fffffffu) < 0x00800000u)
			return -__builtin_inff();
		if (ix >> 31)
			return __builtin_nanf("");
		return x;
	}
	int k;
	float rh, rl;
	const struct gvm_log_entry *e = gvm_log_reduce(ix, __gvm_log_tab, &k, &rh, &rl);
	float r = rh + rl, kf = (float)k, err;
	float hi = gvm_two_sum((kf * gvm_ln2hi + e->hi) + rh, rl, &err);
	float lo = err + ((kf * gvm_ln2lo + e->lo) + r * r * (-0.5f + r * (gvm_L0 + r * (gvm_L1 + r * gvm_L2))));
	/* (hi + lo) / ln10: hi in two halves of 12 bits, each times IVLN10HI exact */
	float hh = gvm_asf(gvm_asu(hi) & 0xfffff000u), hl = hi - hh;
	float a = hh * IVLN10HI, b = hl * IVLN10HI + (hi * IVLN10LO + lo * IVLN10);
	return a + b;
}
