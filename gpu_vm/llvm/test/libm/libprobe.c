// gpu_vm's libm as compiled, on a machine: each function over random inputs of a game's range and of the whole range,
// every result written to stdout as a record {set, a, b, result} (4 words, binary); libprobe_check (host) compares
// them with the same C on the host (acc.c's float semantics: the bits must be the same where the code has no division
// or square root) and with the host's double libm (ulps). Bare metal, the exec runtime and libm_gvm.a (run.sh libprobe);
// on the card:
//   pwsh tools\gpu_queue.ps1 "C:\Program Files\Git\bin\bash.exe" -c "bin/gvm_gpu.exe build/gvm_libm/libprobe.elf > build/gvm_libm/libprobe.gpu"
#include "gvm.h"

float sinf(float), cosf(float), tanf(float), expf(float), exp2f(float), logf(float), log2f(float), log10f(float);
float powf(float, float), asinf(float), acosf(float), atanf(float), atan2f(float, float), sqrtf(float), roundf(float);
void sincosf(float, float *, float *);

static unsigned s = 0x2545f491u;
static unsigned rnd(void) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}
static unsigned fb(float f) {
  union { float f; unsigned u; } x = {f};
  return x.u;
}
static float bf(unsigned u) {
  union { unsigned u; float f; } x = {u};
  return x.f;
}
static float uni(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() >> 8) * (1.0f / 16777216.0f); }
static float anyf(void) {   // any finite float but denormals, either sign
  unsigned u;
  do u = rnd(); while ((u & 0x7f800000u) == 0x7f800000u || (u & 0x7f800000u) == 0);
  return bf(u);
}

static unsigned buf[4096];
static int nb;
static void rec(unsigned set, float a, float b, float r) {
  buf[nb] = set, buf[nb + 1] = fb(a), buf[nb + 2] = fb(b), buf[nb + 3] = fb(r);
  nb += 4;
  if (nb == 4096) {
    write(1, buf, sizeof buf);
    nb = 0;
  }
}

#define N 4096
int main(void) {
  for (int i = 0; i < N; i++) {
    float x, y, sn, cs;
    x = uni(-10, 10), rec(0, x, 0, sinf(x));
    x = uni(-10000, 10000), rec(1, x, 0, sinf(x));
    x = uni(-10, 10), rec(2, x, 0, cosf(x));
    x = uni(-10, 10), sincosf(x, &sn, &cs), rec(3, x, 0, sn), rec(4, x, 0, cs);
    x = uni(-1.5f, 1.5f), rec(5, x, 0, tanf(x));
    x = uni(-10, 10), rec(6, x, 0, tanf(x));
    x = uni(-20, 20), rec(7, x, 0, expf(x));
    x = anyf(), rec(8, x, 0, expf(x));
    x = uni(-20, 20), rec(9, x, 0, exp2f(x));
    x = uni(0.001f, 1000), rec(10, x, 0, logf(x));
    x = __builtin_fabsf(anyf()), rec(11, x, 0, logf(x));
    x = uni(0.5f, 2), rec(12, x, 0, log2f(x));
    x = uni(0.001f, 1000), rec(13, x, 0, log10f(x));
    x = uni(0, 10), y = uni(-4, 4), rec(14, x, y, powf(x, y));
    x = uni(0.9f, 1.1f), y = uni(-200, 200), rec(15, x, y, powf(x, y));
    x = uni(-1, 1), rec(16, x, 0, asinf(x));
    x = uni(-1, 1), rec(17, x, 0, acosf(x));
    x = uni(-20, 20), rec(18, x, 0, atanf(x));
    x = uni(-10, 10), y = uni(-10, 10), rec(19, x, y, atan2f(x, y));
    x = __builtin_fabsf(anyf()), rec(20, x, 0, sqrtf(x));
    x = uni(-100, 100), rec(21, x, 0, roundf(x));
  }
  write(1, buf, nb * 4);
  return 0;
}
