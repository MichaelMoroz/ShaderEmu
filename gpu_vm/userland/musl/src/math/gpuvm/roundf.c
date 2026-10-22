/* roundf, half away from zero: trunc(x + copysign(0.5 - 2^-25, x)), three of the card's ops, exact (the sum can not
 * cross the integer it should not reach: below 2^23 a fraction of 0.5 - 2^-25 is never rounded up to 0.5 or past it
 * but where x's own fraction is 0.5 or more; from 2^23 x is an integer and the sum rounds back to it). */
#include <math.h>

float roundf(float x)
{
	return __builtin_truncf(x + __builtin_copysignf(0x1.fffffep-2f, x));
}
