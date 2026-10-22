// The test runtime's few math functions (gpu_vm/llvm/test/exec/rt).
#ifndef GVM_MATH_H
#define GVM_MATH_H
#define HUGE_VAL __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
double fabs(double x);
double sqrt(double x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double fmod(double x, double y);
double ldexp(double x, int e);
double frexp(double x, int *e);
double pow(double x, double y);
double copysign(double x, double y);
float fabsf(float x);
float sqrtf(float x);
float floorf(float x);
float ceilf(float x);
float truncf(float x);
float fminf(float a, float b);
float fmaxf(float a, float b);
float copysignf(float x, float y);
#endif
