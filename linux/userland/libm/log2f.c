/* log2f in float instructions alone (gvm_libm.h): about half an ulp. 0, denormals, negatives,
 * inf and nan: musl's generic log2f. */
#include "gvm_libm.h"

#ifdef GVM_HOST
#define generic_log2f gvm_host_log2f
#else
#define log2f generic_log2f
#include "../log2f.c"
#undef log2f
#endif

float log2f(float x)
{
	uint32_t ix = gvm_asu(x);
	if (ix - 0x00800000u >= 0x7f800000u - 0x00800000u)   /* 0, denormals, negatives, inf, nan */
		return generic_log2f(x);
	float lo;
	return gvm_log2_hl(ix, &lo);
}
