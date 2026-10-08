/*
 * Quake's trigonometry in floats (fmath_shaderemu.c), included before every file of the game
 * (build.sh). The source calls sin, cos, tan and atan, C's double functions, on floats; and the
 * C library's float versions work in doubles inside, which this machine has no instructions
 * for: a call was two to five thousand instructions.
 */
#ifndef SHADEREMU_QUAKE_FMATH_H
#define SHADEREMU_QUAKE_FMATH_H

#include <math.h>

float q_sin (float x);
float q_cos (float x);
float q_tan (float x);
float q_atan (float x);
float q_atan2 (float y, float x);

#define sin(x) q_sin (x)
#define cos(x) q_cos (x)
#define tan(x) q_tan (x)
#define atan(x) q_atan (x)
#define atan2(y, x) q_atan2 (y, x)
#define floor(x) floorf (x)
#define ceil(x) ceilf (x)
#define fabs(x) fabsf (x)
#define sqrt(x) sqrtf (x)

#endif
