/* trunc by the bits (gvm_dround.h) */
#include <math.h>
#include "gvm_dround.h"

double trunc(double x)
{
	return gvm_dround(x, 0);
}
