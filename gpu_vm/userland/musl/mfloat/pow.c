/* pow at float precision (README) */
#include <math.h>

double pow(double x, double y)
{
	return powf((float)x, (float)y);
}
