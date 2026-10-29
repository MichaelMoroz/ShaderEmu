/* tanf in float instructions (flm_libm.h): tan r = r + r^3 T(r^2) on |r| <= pi/4, -1 / tan r in the odd quadrants
 * (a division). |x| >=
 * 8192, inf and nan: musl's generic tanf. */
#include "flm_libm.h"

#define tanf generic_tanf
#include "../tanf.c"
#undef tanf

/* error 2^-29.4 relative */
static const float
T0 = 0x1.555562p-2f,
T1 = 0x1.110d46p-3f,
T2 = 0x1.bae7bap-5f,
T3 = 0x1.5c3d0ep-6f,
T4 = 0x1.6543ep-7f,
T5 = 0x1.28b7d2p-17f,
T6 = 0x1.214a8p-8f;

float tanf(float x)
{
	if ((flm_asu(x) & 0x7fffffffu) >= FLM_TRIG_MAX)
		return generic_tanf(x);
	int k;   /* tan is odd: |x| reduced, x's sign given to the result (tan(-0) is -0) */
	float rl, r = flm_trig_reduce(__builtin_fabsf(x), &k, &rl), z = r * r;
	float t = r + ((rl + rl * z) + r * z * (T0 + z * (T1 + z * (T2 + z * (T3 + z * (T4 + z * (T5 + z * T6)))))));
	t = (k & 1) ? -1.0f / t : t;
	return flm_asf(flm_asu(t) ^ (flm_asu(x) & 0x80000000u));
}
