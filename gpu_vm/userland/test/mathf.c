/* Floating point and libm: double (software, compiler-rt) and float (the machine's ops), against known values. */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include "check.h"

static int near(double a, double b, double rel) { return fabs(a - b) <= rel * fabs(b) + 1e-300; }

int main(void)
{
	volatile double x = 0.5, y = 2.0;
	CHECK(near(sin(x), 0.479425538604203, 1e-15));
	CHECK(near(cos(x), 0.8775825618903728, 1e-15));
	CHECK(near(tan(x), 0.5463024898437905, 1e-15));
	CHECK(near(exp(y), 7.38905609893065, 1e-15));
	CHECK(near(log(y), 0.6931471805599453, 1e-15));
	CHECK(near(log10(1000.0 * x), 2.698970004336019, 1e-15));
	CHECK(near(pow(y, 0.5), 1.4142135623730951, 1e-15));
	CHECK(sqrt(y) == 1.4142135623730951);   /* correctly rounded */
	CHECK(near(atan2(1.0, y), 0.4636476090008061, 1e-15));
	CHECK(near(asin(x), 0.5235987755982989, 1e-15));
	CHECK(near(acos(x), 1.0471975511965979, 1e-15));
	CHECK(near(sinh(x), 0.5210953054937474, 1e-15));
	CHECK(near(cbrt(27.0 * x), 2.3811015779523490, 1e-15));
	CHECK(near(hypot(3.0, 4.0 * x * y), 5.0, 1e-16));
	CHECK(floor(-2.5) == -3.0 && ceil(-2.5) == -2.0 && round(2.5) == 3.0 && trunc(-2.7) == -2.0);
	CHECK(rint(2.5) == 2.0 && nearbyint(3.5) == 4.0);
	CHECK(fmod(10.0, 3.0) == 1.0 && remainder(10.0, 3.0) == 1.0);
	CHECK(fabs(-3.0) == 3.0 && copysign(2.0, -1.0) == -2.0);
	int e;
	CHECK(frexp(48.0, &e) == 0.75 && e == 6);
	CHECK(ldexp(0.75, 6) == 48.0);
	double ip;
	CHECK(modf(3.25, &ip) == 0.25 && ip == 3.0);
	CHECK(isnan(sqrt(-1.0)) && isinf(1.0 / (x - x)));
	CHECK(fma(2.0, 3.0, 1.0) == 7.0);
	CHECK(lround(-2.5) == -3 && llrint(1e15 + 0.5) == 1000000000000000LL);
	CHECK(DBL_EPSILON == 0x1p-52);

	/* float: the machine's ops for + - * / and sqrtf; libm's functions in float */
	volatile float f = 0.5f, g = 3.0f;
	CHECK(f + g == 3.5f && f * g == 1.5f && g - f == 2.5f && g / 2.0f == 1.5f);
	CHECK(fabsf(sinf(f) - 0.47942554f) < 1e-6f);
	CHECK(fabsf(expf(g) - 20.085537f) < 1e-5f);
	CHECK(fabsf(logf(g) - 1.0986123f) < 1e-6f);
	CHECK(fabsf(powf(g, f) - 1.7320508f) < 1e-6f);
	CHECK(fabsf(sqrtf(g) - 1.7320508f) < 1e-6f);
	CHECK(floorf(-1.5f) == -2.0f && roundf(-1.5f) == -2.0f);
	CHECK((int)(g * 7.0f) == 21 && (float)7 == 7.0f);
	CHECK(fmaxf(f, g) == g && fminf(f, g) == f);

	/* conversions between double and float, ints */
	volatile double big = 1e10;
	CHECK((long long)big == 10000000000LL);
	CHECK((float)big == 1e10f);
	CHECK((double)f == 0.5);
	CHECK((unsigned)(4294967295.0) == 4294967295u);
	volatile long long ll = -123456789012345LL;
	CHECK((double)ll == -123456789012345.0);

	printf("sin(0.5) = %.17g, expf(3) = %.9g\n", sin(x), (double)expf(g));
	return DONE("math");
}
