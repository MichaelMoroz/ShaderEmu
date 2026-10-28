/* floor by the bits (flm_dround.h) */
#include <math.h>
#include "flm_dround.h"

double floor(double x)
{
	return flm_dround(x, 1);
}
