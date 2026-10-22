/* expf on the card's exact float ops (gvm_libm.h): x = n ln2 + s (ln2 in two parts, n ln2hi exact), e^s = 1 + s +
 * s^2 P(s); about an ulp, the same bits on gvm_cpu and the card. Results below 2^-126 are 0. */
#include "gvm_libm.h"

static const float
L2E = 0x1.715476p+0f,
LN2HI = 0x1.62ep-1f,    /* 12 bits */
LN2LO = 0x1.0bfbe8p-15f,
/* error 2^-28.3 relative on |s| <= ln2 / 2 */
P0 = 0x1.fffffcp-2f,
P1 = 0x1.555492p-3f,
P2 = 0x1.5558f2p-5f,
P3 = 0x1.1239ep-7f,
P4 = 0x1.6a243ap-10f;

float expf(float x)
{
	if (!(__builtin_fabsf(x) < 104.0f))
		return x != x ? x : x > 0 ? x * 0x1p127f * 0x1p127f : 0.0f;
	float n = __builtin_roundevenf(x * L2E);
	float s = (x - n * LN2HI) - n * LN2LO;
	float p = 1.0f + (s + s * s * (P0 + s * (P1 + s * (P2 + s * (P3 + s * P4)))));
	return gvm_scale(p, (int)n);
}
