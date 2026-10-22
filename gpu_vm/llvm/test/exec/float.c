// Floats on the card's units (in the integer registers) and doubles in
// software: arithmetic, compares with NaN, conversions, the rounding ops.
#include "gvm.h"

#define NI __attribute__((noinline))
static volatile float vf[] = {0.0f, 1.0f, -1.0f, 2.5f, -2.5f, 1e10f, -1e-10f,
                              3.14159265f, 1e30f, 0.1f, -0.0f};
#define NF (int)(sizeof vf / sizeof vf[0])

NI float fadd(float a, float b) { return a + b; }
NI float fsub(float a, float b) { return a - b; }
NI float fmul(float a, float b) { return a * b; }
NI float fdiv(float a, float b) { return a / b; }
NI float fmadd(float a, float b, float c) { return a * b + c; }
NI float fneg(float a) { return -a; }
NI float fabsf_(float a) { return __builtin_fabsf(a); }
NI float fcopy(float a, float b) { return __builtin_copysignf(a, b); }
NI float fsqrt(float a) { return __builtin_sqrtf(a); }
NI float ffloor(float a) { return __builtin_floorf(a); }
NI float fceil(float a) { return __builtin_ceilf(a); }
NI float ftrunc(float a) { return __builtin_truncf(a); }
NI float fmin_(float a, float b) { return __builtin_fminf(a, b); }
NI float fmax_(float a, float b) { return __builtin_fmaxf(a, b); }
NI int feq(float a, float b) { return a == b; }
NI int fne(float a, float b) { return a != b; }
NI int flt(float a, float b) { return a < b; }
NI int fle(float a, float b) { return a <= b; }
NI int fgt(float a, float b) { return a > b; }
NI int fge(float a, float b) { return a >= b; }
NI int funord(float a, float b) { return __builtin_isunordered(a, b); }
NI int fbr(float a, float b) {
  if (a < b)
    return 1;
  if (!(a >= b))
    return 2;
  return 3;
}
NI int f2i(float a) { return (int)a; }
NI unsigned f2u(float a) { return (unsigned)a; }
NI float i2f(int a) { return (float)a; }
NI float u2f(unsigned a) { return (float)a; }
NI long long f2l(float a) { return (long long)a; }
NI float l2f(long long a) { return (float)a; }
NI float fconst(float a) { return a * 0.5f + 1.25f; }

NI double dadd(double a, double b) { return a + b; }
NI double dmul(double a, double b) { return a * b; }
NI double ddiv(double a, double b) { return a / b; }
NI int dlt(double a, double b) { return a < b; }
NI double f2d(float a) { return a; }
NI float d2f(double a) { return (float)a; }
NI int d2i(double a) { return (int)a; }
NI double i2d(int a) { return a; }
NI long long d2l(double a) { return (long long)a; }

static float bits2f(unsigned u) {
  float f;
  memcpy(&f, &u, 4);
  return f;
}
static unsigned f2bits(float f) {
  unsigned u;
  memcpy(&u, &f, 4);
  return u;
}

int main(void) {
  float nan = bits2f(0x7fc00000u);
  CHECK(fadd(1.5f, 2.25f) == 3.75f);
  CHECK(fsub(1.5f, 2.25f) == -0.75f);
  CHECK(fmul(1.5f, -2.0f) == -3.0f);
  CHECK(fdiv(1.0f, 4.0f) == 0.25f);
  CHECK(fmadd(1.5f, 2.0f, 0.25f) == 3.25f);
  CHECK(fneg(2.0f) == -2.0f);
  CHECK(f2bits(fneg(0.0f)) == 0x80000000u);
  CHECK(fabsf_(-3.5f) == 3.5f);
  CHECK(fcopy(3.0f, -1.0f) == -3.0f);
  CHECK(fsqrt(16.0f) == 4.0f);
  CHECK(fsqrt(2.0f) > 1.41421f && fsqrt(2.0f) < 1.41422f);
  CHECK(ffloor(-2.5f) == -3.0f);
  CHECK(fceil(-2.5f) == -2.0f);
  CHECK(ftrunc(-2.5f) == -2.0f);
  CHECK(fmin_(1.0f, nan) == 1.0f);
  CHECK(fmax_(nan, -1.0f) == -1.0f);
  CHECK(fmin_(-3.0f, 2.0f) == -3.0f);
  CHECK(fconst(3.0f) == 2.75f);
  for (int i = 0; i < NF; i++)
    for (int j = 0; j < NF; j++) {
      float a = vf[i], b = vf[j];
      CHECK_EQ(feq(a, b), a == b);
      CHECK_EQ(fne(a, b), a != b);
      CHECK_EQ(flt(a, b), a < b);
      CHECK_EQ(fle(a, b), a <= b);
      CHECK_EQ(fgt(a, b), a > b);
      CHECK_EQ(fge(a, b), a >= b);
      CHECK_EQ(funord(a, b), 0);
    }
  CHECK_EQ(feq(nan, nan), 0);
  CHECK_EQ(fne(nan, nan), 1);
  CHECK_EQ(flt(nan, 1.0f), 0);
  CHECK_EQ(fge(nan, 1.0f), 0);
  CHECK_EQ(funord(nan, 1.0f), 1);
  CHECK_EQ(fbr(1.0f, 2.0f), 1);
  CHECK_EQ(fbr(nan, 2.0f), 2);
  CHECK_EQ(fbr(3.0f, 2.0f), 3);
  CHECK_EQ(f2i(-2.75f), -2);
  CHECK_EQ(f2i(1e9f), 1000000000);
  CHECK_EQ(f2u(3e9f), 3000000000u);
  CHECK(i2f(-7) == -7.0f);
  CHECK(u2f(0x80000000u) == 2147483648.0f);
  CHECK_EQ(f2l(-1e12f), -999999995904ll);
  CHECK(l2f(1ll << 40) == 1099511627776.0f);

  CHECK(dadd(0.1, 0.2) == 0.30000000000000004);
  CHECK(dmul(1.5, -4.0) == -6.0);
  CHECK(ddiv(1.0, 3.0) * 3.0 == 1.0);
  CHECK_EQ(dlt(1.0, 2.0), 1);
  CHECK_EQ(dlt(2.0, 1.0), 0);
  CHECK(f2d(0.1f) == (double)0.1f);
  CHECK(d2f(0.1) == 0.1f);
  CHECK_EQ(d2i(-123.9), -123);
  CHECK(i2d(-5) == -5.0);
  CHECK_EQ(d2l(1e15), 1000000000000000ll);
  double acc = 0;
  for (int i = 1; i <= 100; i++)
    acc += 1.0 / i;
  CHECK(acc > 5.187377517 && acc < 5.187377518);
  float facc = 0;
  for (int i = 1; i <= 100; i++)
    facc += 1.0f / (float)i;
  CHECK(facc > 5.18736f && facc < 5.18740f);
  printf("float ok %.3f\n", acc);
  return 0;
}
