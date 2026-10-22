/* truncf: the card's op (gpu_vm/userland/README.md, "libm") */
#include <math.h>

float truncf(float x)
{
	return __builtin_truncf(x);
}
