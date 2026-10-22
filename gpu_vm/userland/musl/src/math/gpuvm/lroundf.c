/* lroundf: roundf's three ops and a conversion */
#include <math.h>

long lroundf(float x)
{
	return (long)__builtin_truncf(x + __builtin_copysignf(0x1.fffffep-2f, x));
}
