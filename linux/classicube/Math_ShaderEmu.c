/* Sine and cosine in single precision for ClassiCube on the ShaderEmu machine, whose float
   instructions are singles: the game's own work in doubles, a library call each operation. */
#include "Core.h"
#ifdef CC_BUILD_SHADEREMU
#include "ExtMath.h"

/* on -pi/4 .. pi/4 */
static CC_INLINE float SineSmall(float r) {
	float r2 = r * r;
	return r + r * r2 * (-0.16666667f + r2 * (0.0083333310f + r2 * -0.00019840874f));
}

static CC_INLINE float CosineSmall(float r) {
	float r2 = r * r;
	return 1.0f + r2 * (-0.5f + r2 * (0.041666638f + r2 * (-0.0013888378f + r2 * 0.000024760495f)));
}

/* sine of x + turns quarter turns */
static float SineFrom(float x, int turns) {
	/* the nearest whole number of quarter turns, and what is left of the angle */
	int k = (int)(x * 0.63661977f + (x < 0 ? -0.5f : 0.5f));
	float r = (x - (float)k * 1.5707855f) - (float)k * 1.0804334e-5f; /* pi/2 in two parts */

	switch ((k + turns) & 3) {
	case 0:  return  SineSmall(r);
	case 1:  return  CosineSmall(r);
	case 2:  return -SineSmall(r);
	default: return -CosineSmall(r);
	}
}

float Math_SinF(float x) { return SineFrom(x, 0); }
float Math_CosF(float x) { return SineFrom(x, 1); }
#endif
