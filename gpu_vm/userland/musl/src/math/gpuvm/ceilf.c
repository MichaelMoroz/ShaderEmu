/* ceilf: the card's op (gpu_vm/userland/README.md, "libm") */
#include <math.h>

float ceilf(float x)
{
	return __builtin_ceilf(x);
}
