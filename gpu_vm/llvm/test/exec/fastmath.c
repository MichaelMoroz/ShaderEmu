// FLAGS: -ffast-math
// -ffast-math: the card's approximate special functions (fsin fcos fexp2
// flog2 frcp frsqrt; exp and log through exp2 and log2), checked loosely.
#include "gvm.h"

#define NI __attribute__((noinline))
static volatile float vx[] = {0.25f, 0.5f, 1.0f, 2.0f, 3.0f};

NI float f_sin(float x) { return __builtin_sinf(x); }
NI float f_cos(float x) { return __builtin_cosf(x); }
NI float f_exp(float x) { return __builtin_expf(x); }
NI float f_log(float x) { return __builtin_logf(x); }
NI float f_exp2(float x) { return __builtin_exp2f(x); }
NI float f_log2(float x) { return __builtin_log2f(x); }
NI float f_rcp(float x) { return 1.0f / x; }
NI float f_rsqrt(float x) { return 1.0f / __builtin_sqrtf(x); }

static int near(float a, float b) {
  float d = a - b;
  if (d < 0)
    d = -d;
  float m = b < 0 ? -b : b;
  return d <= 1e-4f + 1e-4f * m;
}

int main(void) {
  // sin^2 + cos^2 = 1, exp(log x) = x, 2^log2 x = x, x * 1/x = 1
  for (int i = 0; i < 5; i++) {
    float x = vx[i];
    float s = f_sin(x), c = f_cos(x);
    CHECK(near(s * s + c * c, 1.0f));
    CHECK(near(f_exp(f_log(x)), x));
    CHECK(near(f_exp2(f_log2(x)), x));
    CHECK(near(x * f_rcp(x), 1.0f));
    CHECK(near(f_rsqrt(x) * f_rsqrt(x) * x, 1.0f));
  }
  CHECK(near(f_sin(0.5f), 0.4794255f));
  CHECK(near(f_exp(1.0f), 2.7182818f));
  CHECK(near(f_log2(8.0f), 3.0f));
  puts("fastmath ok");
  return 0;
}
