/* lrintf: round to even (the one rounding mode) and convert: two of the card's ops */
#include <math.h>

long lrintf(float x)
{
	return (long)__builtin_rintf(x);
}
