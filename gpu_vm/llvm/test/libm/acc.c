// The accuracy of gpu_vm's libm (gpu_vm/userland/musl/src/math/gpuvm), measured on the host over every float: the
// same C compiled for the host with the card's float semantics (-ffp-contract=off: a product rounded before its sum,
// as fmad does; denormals flushed in and out, MXCSR's DAZ and FTZ), against the host's double libm. The results are
// the card's bit for bit wherever the code uses no division (the card's divide is within 1.7 ulps, not exact).
//   bash gpu_vm/llvm/test/libm/run.sh acc [FUNCTION...]
#include <math.h>
#include <omp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>

#define GVM_HOST 1
#define sinf gvm_sinf
#define cosf gvm_cosf
#define sincosf gvm_sincosf
#define tanf gvm_tanf
#define expf gvm_expf
#define exp2f gvm_exp2f
#define logf gvm_logf
#define log2f gvm_log2f
#define log10f gvm_log10f
#define powf gvm_powf
#define asinf gvm_asinf
#define roundf gvm_roundf
#define floor gvm_floor
#define ceil gvm_ceil
#define trunc gvm_trunc
#define round gvm_round
#include "sinf.c"
#include "cosf.c"
#include "sincosf.c"
#include "tanf.c"
#include "expf.c"
#include "exp2f.c"
#include "logf.c"
#include "log2f.c"
#include "log10f.c"
#include "powf.c"
#include "asinf.c"
#include "roundf.c"
#include "floor.c"
#include "ceil.c"
#include "trunc.c"
#include "round.c"
#include "__gvm_log_data.c"
#undef sinf
#undef cosf
#undef sincosf
#undef tanf
#undef expf
#undef exp2f
#undef logf
#undef log2f
#undef log10f
#undef powf
#undef asinf
#undef roundf
#undef floor
#undef ceil
#undef trunc
#undef round

static float bf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// the error of y in ulps of the float nearest ref (0 when both are nan, or y is the flushed or overflowed result)
static double ulps(float y, double ref) {
  if (isnan(ref)) return isnan(y) ? 0 : 1e9;
  if (isnan(y)) return 1e9;
  if (fabs(ref) < 0x1p-126) return fabs(y) <= 0x1p-126 ? 0 : fabs(y - ref) / 0x1p-149;
  if (fabs(ref) >= 0x1.ffffffp127) return isinf(y) && (y > 0) == (ref > 0) ? 0 : 1e9;
  if (isinf(y)) return 1e9;
  int e;
  frexp(ref, &e);
  return fabs((double)y - ref) / ldexp(1, e - 1 - 23);
}

struct stat { double max, sum; float worst, worst2; long n, cr, over1, over2; };
static void add(struct stat *s, double u, float x, float x2, float y, double ref) {
  s->n++;
  s->sum += u;
  if (u > s->max) s->max = u, s->worst = x, s->worst2 = x2;
  if (y == (float)ref || (isnan(y) && isnan(ref)) || u == 0) s->cr++;
  if (u > 1.0) s->over1++;
  if (u > 2.0) s->over2++;
}
static void merge(struct stat *a, const struct stat *b) {
  a->n += b->n, a->sum += b->sum, a->cr += b->cr, a->over1 += b->over1, a->over2 += b->over2;
  if (b->max > a->max) a->max = b->max, a->worst = b->worst, a->worst2 = b->worst2;
}
static void report(const char *name, const char *range, const struct stat *s) {
  printf("%-8s %-26s %12ld %9.3f %9.4f %9.4f%% %10ld %10ld   worst %a", name, range, s->n, s->max, s->sum / s->n,
         100.0 * s->cr / s->n, s->over1, s->over2, s->worst);
  if (s->worst2 != 0) printf(", %a", s->worst2);
  printf("\n");
  fflush(stdout);
}

static void ftz(void) { _mm_setcsr(_mm_getcsr() | 0x8040); }

typedef float (*f1)(float);
typedef double (*d1)(double);
static float sin_sc(float x) { float s, c; gvm_sincosf(x, &s, &c); return s; }
static float cos_sc(float x) { float s, c; gvm_sincosf(x, &s, &c); return c; }
static struct { const char *name; f1 f; d1 ref; uint32_t lo, hi; } one[] = {
  {"sinf", gvm_sinf, sin, 0, 0x46000000u}, {"cosf", gvm_cosf, cos, 0, 0x46000000u},
  {"sincosf", sin_sc, sin, 0, 0x46000000u}, {"sincosf", cos_sc, cos, 0, 0x46000000u},
  {"tanf", gvm_tanf, tan, 0, 0x46000000u}, {"expf", gvm_expf, exp, 0, 0x7f800001u},
  {"exp2f", gvm_exp2f, exp2, 0, 0x7f800001u}, {"logf", gvm_logf, log, 0, 0x7f800001u},
  {"log2f", gvm_log2f, log2, 0, 0x7f800001u}, {"log10f", gvm_log10f, log10, 0, 0x7f800001u},
  {"asinf", gvm_asinf, asin, 0, 0x7f800001u}, {"roundf", gvm_roundf, round, 0, 0x7f800001u},
};

static void run1(int i) {
  struct stat all = {0};
  uint32_t lo = one[i].lo, hi = one[i].hi;
  // both signs of every float with |x| in [lo, hi)
#pragma omp parallel
  {
    ftz();
    struct stat s = {0};
#pragma omp for schedule(dynamic, 1 << 16)
    for (int64_t j = lo; j < (int64_t)hi; j++) {
      for (uint32_t sg = 0; sg < 2; sg++) {
        float x = bf((uint32_t)j | sg << 31);
        float y = one[i].f(x);
        double ref = one[i].ref((double)x);
        add(&s, ulps(y, ref), x, 0, y, ref);
      }
    }
#pragma omp critical
    merge(&all, &s);
  }
  char range[64];
  snprintf(range, sizeof range, "|x| < %g", bf(hi));
  report(one[i].name, hi == 0x7f800001u ? "every float" : range, &all);
}

static uint64_t rng(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

// powf over random pairs: x of every exponent and y such that |y log2 x| < 140; x near 1 with large y; small
// integers y with negative x
static void runpow(long n) {
  const char *names[] = {"x any, y in range", "x near 1, y large", "x < 0, y integer", "x in [0.5, 2], y in [-8, 8]"};
  for (int kind = 0; kind < 4; kind++) {
    struct stat all = {0};
#pragma omp parallel
    {
      ftz();
      struct stat s = {0};
      uint64_t st = 0x9e3779b97f4a7c15ull * (omp_get_thread_num() + 1 + 100 * kind);
#pragma omp for
      for (long j = 0; j < n; j++) {
        uint64_t r = rng(&st);
        float x, y;
        if (kind == 0) {
          x = bf(0x00800000u + (uint32_t)(r % (0x7f800000u - 0x00800000u)));
          double l = fabs(log2((double)x));
          double ymax = 140 / (l < 1e-30 ? 1e-30 : l);
          if (ymax > 1e9) ymax = 1e9;
          y = (float)(((double)(r >> 40) / (1 << 24) * 2 - 1) * ymax);
        } else if (kind == 1) {
          x = bf(0x3f800000u + (uint32_t)((int32_t)(r & 0xffff) - 0x8000));
          double l = fabs(log2((double)x));
          double ymax = l == 0 ? 1e6 : 140 / l;
          y = (float)(((double)(r >> 40) / (1 << 24) * 2 - 1) * ymax);
        } else if (kind == 2) {
          x = -bf(0x3c000000u + (uint32_t)(r % 0x07000000u));
          y = (float)((int)((r >> 32) % 61) - 30);
        } else {
          x = 0.5f + 1.5f * (float)((r & 0xffffff) / 16777216.0);
          y = (float)(((double)(r >> 40) / (1 << 24) * 2 - 1) * 8);
        }
        float v = gvm_powf(x, y);
        double ref = pow((double)x, (double)y);
        add(&s, ulps(v, ref), x, y, v, ref);
      }
#pragma omp critical
      merge(&all, &s);
    }
    report("powf", names[kind], &all);
  }
}

// floor, ceil, trunc, round of doubles by their bits: every exponent, random fractions, the halves, against the host's
static void rundouble(long n) {
  _mm_setcsr(_mm_getcsr() & ~0x8040u);   // soft double on gpu_vm: no flush
  const char *names[] = {"trunc", "floor", "ceil", "round"};
  double (*f[])(double) = {gvm_trunc, gvm_floor, gvm_ceil, gvm_round}, (*ref[])(double) = {trunc, floor, ceil, round};
  for (int m = 0; m < 4; m++) {
    long bad = 0;
    uint64_t st = 12345;
    for (long j = 0; j < n; j++) {
      uint64_t r = rng(&st);
      double x;
      memcpy(&x, &r, 8);
      if (j % 4 == 1) x = (double)((int64_t)r >> 20) * 0.5;                         // halves
      if (j % 4 == 2) x = ldexp((double)(r >> 11), (int)(r % 80) - 100);              // around 1
      if (j % 4 == 3) x = (double)(int64_t)(r >> 8) / (1 << (r & 15));
      double y = f[m](x), w = ref[m](x);
      if (memcmp(&y, &w, 8) != 0 && !(isnan(y) && isnan(w))) {
        if (bad++ < 5) printf("  %s(%a) = %a, want %a\n", names[m], x, y, w);
      }
    }
    printf("%-8s %-26s %12ld %s\n", names[m], "doubles (bits)", n, bad ? "WRONG" : "all exact");
  }
}

int main(int argc, char **argv) {
  printf("%-8s %-26s %12s %9s %9s %10s %10s %10s\n", "function", "inputs", "n", "max ulp", "mean ulp",
         "correctly", "> 1 ulp", "> 2 ulps");
  for (unsigned i = 0; i < sizeof one / sizeof one[0]; i++) {
    int want = argc < 2;
    for (int a = 1; a < argc; a++) want |= !strcmp(argv[a], one[i].name);
    if (want) run1(i);
  }
  int want = argc < 2;
  for (int a = 1; a < argc; a++) want |= !strcmp(argv[a], "powf");
  if (want) runpow(getenv("POW_N") ? atol(getenv("POW_N")) : 100000000L);
  want = argc < 2;
  for (int a = 1; a < argc; a++) want |= !strcmp(argv[a], "double");
  if (want) rundouble(100000000L);
  return 0;
}
