// Bundles a call of libm's functions on gvm_cpu (CYCLE, bundles run), for musl's generic libm and gpu_vm's
// (gpu_vm/userland/musl/src/math/gpuvm), and with -DFAST the card's own ops inline (-ffast-math). Each function is
// called 1024 times over 64 inputs of a game's range; the loop alone (a call of a function that returns its
// argument) is taken off. Bare metal: the exec runtime, then libm from the sysroot's libc.a (run.sh bench).
#include "gvm.h"

float sinf(float), cosf(float), tanf(float), atanf(float), atan2f(float, float), expf(float), exp2f(float);
float logf(float), log2f(float), log10f(float), powf(float, float), sqrtf(float), floorf(float), ceilf(float);
float truncf(float), roundf(float), fabsf(float), fminf(float, float), fmaxf(float, float), asinf(float), acosf(float);
void sincosf(float, float *, float *);
double sin(double), cos(double), tan(double), atan2(double, double), exp(double), log(double), pow(double, double);
double sqrt(double), floor(double), ceil(double), fabs(double), atan(double), acos(double), fmod(double, double);

static inline unsigned cycle(void) {
  unsigned c;
  __asm__ volatile("mfsr %0, 12" : "=r"(c));
  return c;
}

#define NI __attribute__((noinline))
static float in1[64], in2[64];
static double din1[64], din2[64];
static volatile float sink;
static volatile double dsink;

NI static float f_id(float x) { return x; }
NI static float f_id2(float x, float y) { return x; }
NI static double d_id(double x) { return x; }
NI static double d_id2(double x, double y) { return x; }
#ifdef FAST
#define W1(name, expr) NI static float w_##name(float x) { return expr; }
W1(sinf, __builtin_sinf(x))
W1(cosf, __builtin_cosf(x))
W1(expf, __builtin_expf(x))
W1(exp2f, __builtin_exp2f(x))
W1(logf, __builtin_logf(x))
W1(log2f, __builtin_log2f(x))
W1(log10f, __builtin_log10f(x))
W1(tanf, __builtin_tanf(x))
NI static float w_powf(float x, float y) { return __builtin_powf(x, y); }
#endif

static unsigned base1, base2, dbase1, dbase2;

static unsigned time1(float (*f)(float)) {
  float s = 0;
  unsigned c0 = cycle();
  for (int i = 0; i < 1024; i++) s += f(in1[i & 63]);
  unsigned c1 = cycle();
  sink = s;
  return c1 - c0;
}
static unsigned time2(float (*f)(float, float)) {
  float s = 0;
  unsigned c0 = cycle();
  for (int i = 0; i < 1024; i++) s += f(in1[i & 63], in2[i & 63]);
  unsigned c1 = cycle();
  sink = s;
  return c1 - c0;
}
static unsigned dtime1(double (*f)(double)) {
  double s = 0;
  unsigned c0 = cycle();
  for (int i = 0; i < 1024; i++) s += f(din1[i & 63]);
  unsigned c1 = cycle();
  dsink = s;
  return c1 - c0;
}
static unsigned dtime2(double (*f)(double, double)) {
  double s = 0;
  unsigned c0 = cycle();
  for (int i = 0; i < 1024; i++) s += f(din1[i & 63], din2[i & 63]);
  unsigned c1 = cycle();
  dsink = s;
  return c1 - c0;
}

static void fill(float lo, float hi, float lo2, float hi2) {
  for (int i = 0; i < 64; i++) {
    in1[i] = lo + (hi - lo) * (float)((i * 37) & 63) / 63.0f;
    in2[i] = lo2 + (hi2 - lo2) * (float)((i * 23) & 63) / 63.0f;
    din1[i] = in1[i] + 1e-9 * i;
    din2[i] = in2[i];
  }
}

static void report(const char *name, unsigned t, unsigned base) {
  // tenths of a bundle a call
  unsigned v = ((t - base) * 10 + 512) / 1024;
  printf("%-8s %6u.%u\n", name, v / 10, v % 10);
}
#define R1(name, lo, hi) (fill(lo, hi, 0, 1), report(#name, time1(name), base1))
#define R2(name, lo, hi, lo2, hi2) (fill(lo, hi, lo2, hi2), report(#name, time2(name), base2))
#define D1(name, lo, hi) (fill(lo, hi, 0, 1), report(#name, dtime1(name), dbase1))
#define D2(name, lo, hi, lo2, hi2) (fill(lo, hi, lo2, hi2), report(#name, dtime2(name), dbase2))
NI static float sincos_sum(float x) {
  float s, c;
  sincosf(x, &s, &c);
  return s + c;
}

int main(void) {
  fill(0, 1, 0, 1);
  base1 = time1(f_id), base2 = time2(f_id2), dbase1 = dtime1(d_id), dbase2 = dtime2(d_id2);
  printf("loop: %u %u %u %u bundles / 1024\n", base1, base2, dbase1, dbase2);
#ifdef FAST
  R1(w_sinf, -10, 10);
  R1(w_cosf, -10, 10);
  R1(w_tanf, -1.5f, 1.5f);
  R1(w_expf, -10, 10);
  R1(w_exp2f, -10, 10);
  R1(w_logf, 0.01f, 100);
  R1(w_log2f, 0.01f, 100);
  R1(w_log10f, 0.01f, 100);
  R2(w_powf, 0.1f, 10, -3, 3);
#else
  R1(sinf, -10, 10);
  R1(cosf, -10, 10);
  R1(sincos_sum, -10, 10);
  R1(tanf, -1.5f, 1.5f);
  R1(atanf, -10, 10);
  R2(atan2f, -10, 10, -10, 10);
  R1(asinf, -1, 1);
  R1(acosf, -1, 1);
  R1(expf, -10, 10);
  R1(exp2f, -10, 10);
  R1(logf, 0.01f, 100);
  R1(log2f, 0.01f, 100);
  R1(log10f, 0.01f, 100);
  R2(powf, 0.1f, 10, -3, 3);
  R1(sqrtf, 0, 100);
  R1(floorf, -100, 100);
  R1(ceilf, -100, 100);
  R1(truncf, -100, 100);
  R1(roundf, -100, 100);
  R1(fabsf, -100, 100);
  R2(fminf, -100, 100, -100, 100);
  R2(fmaxf, -100, 100, -100, 100);
  D1(sin, -10, 10);
  D1(cos, -10, 10);
  D1(tan, -1.5f, 1.5f);
  D1(atan, -10, 10);
  D2(atan2, -10, 10, -10, 10);
  D1(acos, -1, 1);
  D1(exp, -10, 10);
  D1(log, 0.01f, 100);
  D2(pow, 0.1f, 10, -3, 3);
  D1(sqrt, 0, 100);
  D1(floor, -100, 100);
  D1(ceil, -100, 100);
  D1(fabs, -100, 100);
  D2(fmod, -100, 100, 1, 10);
#endif
  return 0;
}
