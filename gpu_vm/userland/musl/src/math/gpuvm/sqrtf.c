/* sqrtf: the card's op (gpu_vm/userland/README.md, "libm"): within 0.9 ulp on the card, correctly rounded on gvm_cpu.
 * (asm: __builtin_sqrtf is a call of sqrtf where math errno is on.) */
#include "gvm_libm.h"

float sqrtf(float x)
{
	return gvm_sqrtf(x);
}
