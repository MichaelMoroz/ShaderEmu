/* floorf: the card's op (gpu_vm/userland/README.md, "libm") */
#include <math.h>

float floorf(float x)
{
	return __builtin_floorf(x);
}
