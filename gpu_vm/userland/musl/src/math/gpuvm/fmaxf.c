/* fmaxf: the card's op (gpu_vm/userland/README.md, "libm") */
#include <math.h>

float fmaxf(float x, float y)
{
	return __builtin_fmaxf(x, y);
}
