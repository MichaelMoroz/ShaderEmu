/* log2f in float instructions alone (flm_libm.h): about half an ulp. 0, denormals, negatives,
 * inf and nan: musl's generic log2f. */
#include "flm_libm.h"

#define log2f generic_log2f
#include "../log2f.c"
#undef log2f

float log2f(float x)
{
	uint32_t ix = flm_asu(x);
	if (ix - 0x00800000u >= 0x7f800000u - 0x00800000u)   /* 0, denormals, negatives, inf, nan */
		return generic_log2f(x);
	float lo;
	return flm_log2_hl(ix, &lo);
}
