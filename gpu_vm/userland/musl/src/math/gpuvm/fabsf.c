/* fabsf: the card's op (gpu_vm/userland/README.md, "libm") */
#include <math.h>

float fabsf(float x)
{
	return __builtin_fabsf(x);
}
