/* log2f on the card's exact float ops (gvm_libm.h): about half an ulp, the same bits on gvm_cpu and the card. */
#include "gvm_libm.h"

float log2f(float x)
{
	uint32_t ix = gvm_asu(x);
	if (ix - 0x00800000u >= 0x7f800000u - 0x00800000u) {
		if ((ix & 0x7fffffffu) < 0x00800000u)
			return -__builtin_inff();
		if (ix >> 31)
			return __builtin_nanf("");
		return x;
	}
	float lo;
	return gvm_log2_hl(ix, &lo);
}
