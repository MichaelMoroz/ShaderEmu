// The host's functions behind acc.c's fallbacks (gvm_libm.h's GVM_HOST: musl's generic code on gpu_vm).
#define _GNU_SOURCE
#include <math.h>

float gvm_host_sinf(float x) { return sinf(x); }
float gvm_host_cosf(float x) { return cosf(x); }
float gvm_host_tanf(float x) { return tanf(x); }
float gvm_host_powf(float x, float y) { return powf(x, y); }
void gvm_host_sincosf(float x, float *s, float *c) { sincosf(x, s, c); }
