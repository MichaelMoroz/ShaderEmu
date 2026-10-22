/* fminf: the card's op (gpu_vm/userland/README.md, "libm") */
#include <math.h>

float fminf(float x, float y)
{
	return __builtin_fminf(x, y);
}
