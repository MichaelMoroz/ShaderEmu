/* floor by the bits (gvm_dround.h) */
#include <math.h>
#include "gvm_dround.h"

double floor(double x)
{
	return gvm_dround(x, 1);
}
