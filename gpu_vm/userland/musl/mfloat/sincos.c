/* sincos at float precision (README) */
#define _GNU_SOURCE
#include <math.h>

void sincos(double x, double *s, double *c)
{
	float fs, fc;
	sincosf((float)x, &fs, &fc);
	*s = fs;
	*c = fc;
}
