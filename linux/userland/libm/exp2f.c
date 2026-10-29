/* exp2f in float instructions alone (flm_libm.h): 2^n 2^r, n = round(x), r = x - n exactly; about an ulp. x outside
 * (-126, 128): musl's generic exp2f. */
#include "flm_libm.h"

#define exp2f generic_exp2f
#include "../exp2f.c"
#undef exp2f

float exp2f(float x)
{
	if (!(x > -126.0f && x < 128.0f))   /* a denormal, 0, inf or nan out, or a nan in */
		return generic_exp2f(x);
	float n = flm_roundevenf(x);
	return flm_scale(flm_exp2_poly(x - n), (int)n);
}
