/* atan2 at float precision (README) */
#include <math.h>

double atan2(double x, double y)
{
	return atan2f((float)x, (float)y);
}
