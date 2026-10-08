/*
 * Sine, cosine, tangent and arctangent in single precision only (fmath.h says why): the angle
 * is brought to within an eighth of a turn and a short polynomial does the rest. Good to
 * three hundred-thousandths for sine and cosine (of angles up to 700) and about one of a
 * radian for arctangent, which is finer than a pixel or a step of the game.
 */
#include <math.h>

#define PI		3.14159265f
#define HALF_PI		1.57079633f

/* on -pi/4 .. pi/4 */
static inline float sine_small (float r)
{
	float r2 = r * r;

	return r + r * r2 * (-0.16666667f + r2 * (0.0083333310f + r2 * -0.00019840874f));
}

static inline float cosine_small (float r)
{
	float r2 = r * r;

	return 1.0f + r2 * (-0.5f + r2 * (0.041666638f + r2 * (-0.0013888378f + r2 * 0.000024760495f)));
}

/* sine of x + turns quarter turns */
static float sine_from (float x, int turns)
{
	/* the nearest whole number of quarter turns, and what is left of the angle */
	int k = (int)(x * 0.63661977f + (x < 0 ? -0.5f : 0.5f));
	float r = (x - (float)k * 1.5707855f) - (float)k * 1.0804334e-5f;	/* pi/2 in two parts */

	switch ((k + turns) & 3) {
	case 0: return sine_small (r);
	case 1: return cosine_small (r);
	case 2: return -sine_small (r);
	default: return -cosine_small (r);
	}
}

float q_sin (float x) { return sine_from (x, 0); }
float q_cos (float x) { return sine_from (x, 1); }
float q_tan (float x) { return sine_from (x, 0) / sine_from (x, 1); }

/* on -1 .. 1 */
static inline float arctangent_small (float x)
{
	float x2 = x * x;

	return x * (0.99986600f + x2 * (-0.33029950f + x2 * (0.18014100f + x2 * (-0.08513300f + x2 * 0.02083510f))));
}

float q_atan (float x)
{
	if (x > 1.0f)
		return HALF_PI - arctangent_small (1.0f / x);
	if (x < -1.0f)
		return -HALF_PI - arctangent_small (1.0f / x);
	return arctangent_small (x);
}

float q_atan2 (float y, float x)
{
	float a;

	if (x == 0.0f)
		return y > 0.0f ? HALF_PI : y < 0.0f ? -HALF_PI : 0.0f;
	a = q_atan (y / x);
	return x > 0.0f ? a : y >= 0.0f ? a + PI : a - PI;
}
