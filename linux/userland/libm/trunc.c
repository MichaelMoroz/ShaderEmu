/* trunc by the bits (flm_dround.h) */
#include <math.h>
#include "flm_dround.h"

double trunc(double x)
{
	return flm_dround(x, 0);
}
