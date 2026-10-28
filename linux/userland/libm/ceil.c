/* ceil by the bits (gvm_dround.h) */
#include <math.h>
#include "gvm_dround.h"

double ceil(double x)
{
	return gvm_dround(x, 2);
}
