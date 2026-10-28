/* exp2f in float instructions alone (gvm_libm.h): 2^n 2^r, n = round(x), r = x - n exactly; about an ulp. x outside
 * (-126, 128): musl's generic exp2f. */
#include "gvm_libm.h"

#ifdef GVM_HOST
#define generic_exp2f gvm_host_exp2f
#else
#define exp2f generic_exp2f
#include "../exp2f.c"
#undef exp2f
#endif

float exp2f(float x)
{
	if (!(x > -126.0f && x < 128.0f))   /* a denormal, 0, inf or nan out, or a nan in */
		return generic_exp2f(x);
	float n = gvm_roundevenf(x);
	return gvm_scale(gvm_exp2_poly(x - n), (int)n);
}
