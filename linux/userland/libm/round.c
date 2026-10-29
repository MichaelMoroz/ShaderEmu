/* round by the bits (flm_dround.h) */
#include <math.h>
#include "flm_dround.h"

double round(double x)
{
	return flm_dround(x, 3);
}
