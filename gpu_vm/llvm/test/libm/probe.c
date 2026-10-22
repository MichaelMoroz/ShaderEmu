// The card's special functions measured: each op of gpu_vm's float class that is the card's approximation (fsin fcos
// fexp2 flog2 fsqrt frsqrt frcp fdiv) run over sets of inputs, every result written to stdout as a record
// {set, a, b, result} (4 words, binary). `probe_check` (host) compares them with the host's double libm.
// Bare metal (the exec runtime); run on gvm_cpu (the reference: the host's float functions) and on the card:
//   pwsh tools\gpu_queue.ps1 bin\gvm_gpu.exe build\gvm_libm\probe.elf > build\gvm_libm\probe.gpu
#include "gvm.h"

#define OP1(name)                                                                                                      \
  static inline unsigned name(unsigned a) {                                                                            \
    unsigned r;                                                                                                        \
    __asm__ volatile(#name " %0, %1" : "=r"(r) : "r"(a));                                                              \
    return r;                                                                                                          \
  }
OP1(fsin)
OP1(fcos)
OP1(fexp2)
OP1(flog2)
OP1(fsqrt)
OP1(frsqrt)
OP1(frcp)
static inline unsigned fdiv(unsigned a, unsigned b) {
  unsigned r;
  __asm__ volatile("fdiv %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
  return r;
}

static unsigned s = 0x12345678u;
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
// a float of exponent in [e0, e1] and a random mantissa, sign s (0, 1 or 2 for either)
static float expo(int e0, int e1, int sg) {
  unsigned r = rnd(), e = (unsigned)(e0 + (int)(rnd() % (unsigned)(e1 - e0 + 1)) + 127);
  unsigned sign = sg == 2 ? (r >> 31) : (unsigned)sg;
  return bf(sign << 31 | e << 23 | (r & 0x7fffffu));
}

static unsigned buf[4096];
static int nb;
static void rec(unsigned set, unsigned a, unsigned b, unsigned r) {
  buf[nb] = set, buf[nb + 1] = a, buf[nb + 2] = b, buf[nb + 3] = r;
  nb += 4;
  if (nb == 4096) {
    write(1, buf, sizeof buf);
    nb = 0;
  }
}

#define N 16384
int main(void) {
  for (int i = 0; i < N; i++) {
    unsigned x;
    x = fb(uni(-3.14159265f, 3.14159265f)), rec(0, x, 0, fsin(x));     // sin [-pi, pi]
    x = fb(uni(-1000.0f, 1000.0f)), rec(1, x, 0, fsin(x));             // sin [-1000, 1000]
    x = fb(expo(-24, -2, 2)), rec(2, x, 0, fsin(x));                   // sin of small numbers
    x = fb(uni(-3.14159265f, 3.14159265f)), rec(3, x, 0, fcos(x));     // cos [-pi, pi]
    x = fb(uni(-1000.0f, 1000.0f)), rec(4, x, 0, fcos(x));
    x = fb(uni(-1.0f, 1.0f)), rec(5, x, 0, fexp2(x));                  // exp2 [-1, 1]
    x = fb(uni(-125.0f, 127.0f)), rec(6, x, 0, fexp2(x));
    x = fb(expo(-24, -4, 2)), rec(7, x, 0, fexp2(x));
    x = fb(uni(0.5f, 2.0f)), rec(8, x, 0, flog2(x));                   // log2 near 1
    x = fb(expo(-125, 126, 0)), rec(9, x, 0, flog2(x));                // log2 of any positive number
    x = fb(expo(-125, 126, 0)), rec(10, x, 0, fsqrt(x));
    x = fb(expo(-125, 126, 0)), rec(11, x, 0, frsqrt(x));
    x = fb(expo(-125, 125, 2)), rec(12, x, 0, frcp(x));
    unsigned y = fb(expo(-60, 60, 2));
    x = fb(expo(-60, 60, 2)), rec(13, x, y, fdiv(x, y));
    y = fb(uni(1.0f, 2.0f));
    x = fb(uni(1.0f, 2.0f)), rec(14, x, y, fdiv(x, y));
  }
  write(1, buf, nb * 4);
  return 0;
}
