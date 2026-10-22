/* nearbyintf: the card's op (gpu_vm/userland/README.md, "libm"): to even, the one rounding mode */
#include <math.h>

float nearbyintf(float x)
{
	return __builtin_rintf(x);
}
