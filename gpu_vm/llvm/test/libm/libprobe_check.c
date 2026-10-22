// libprobe.c's records against the same C on the host (acc.c's float semantics) and the host's double libm: for each
// set the share of results the same bits as the host's emulation (where it has the function: musl's own atanf,
// atan2f and acosf it has not), the largest and the mean error in ulps.
//   built by run.sh libprobe: gcc ... -DGVM_HOST -I<gpuvm math> libprobe_check.c acc_host.c -lm
#include <math.h>
#include <stdint.h>
#include <stdio.h>
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
#define sqrtf gvm_sqrtf_
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
#include "sqrtf.c"
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
#undef sqrtf

static float bf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
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
static float sc_s(float x) { float s, c; gvm_sincosf(x, &s, &c); return s; }
static float sc_c(float x) { float s, c; gvm_sincosf(x, &s, &c); return c; }

static const char *names[] = {"sinf [-10, 10]", "sinf [-1e4, 1e4]", "cosf [-10, 10]", "sincosf: sin", "sincosf: cos",
  "tanf [-1.5, 1.5]", "tanf [-10, 10]", "expf [-20, 20]", "expf any", "exp2f [-20, 20]", "logf [1e-3, 1e3]",
  "logf any", "log2f [0.5, 2]", "log10f [1e-3, 1e3]", "powf [0,10]^[-4,4]", "powf [.9,1.1]^[-200,200]",
  "asinf", "acosf", "atanf [-20, 20]", "atan2f", "sqrtf any", "roundf"};
#define NS 22

int main(int argc, char **argv) {
  _mm_setcsr(_mm_getcsr() | 0x8040);
  FILE *f = fopen(argv[1], "rb");
  if (!f) return 1;
  long n[NS] = {0}, same[NS] = {0}, have[NS] = {0};
  double maxu[NS] = {0}, sumu[NS] = {0};
  uint32_t r[4];
  while (fread(r, 4, 4, f) == 4) {
    unsigned s = r[0];
    float a = bf(r[1]), b = bf(r[2]), y = bf(r[3]), e = 0;
    double ref;
    int h = 1;
    switch (s) {
    case 0: case 1: e = gvm_sinf(a), ref = sin(a); break;
    case 2: e = gvm_cosf(a), ref = cos(a); break;
    case 3: e = sc_s(a), ref = sin(a); break;
    case 4: e = sc_c(a), ref = cos(a); break;
    case 5: case 6: e = gvm_tanf(a), ref = tan(a); break;
    case 7: case 8: e = gvm_expf(a), ref = exp(a); break;
    case 9: e = gvm_exp2f(a), ref = exp2(a); break;
    case 10: case 11: e = gvm_logf(a), ref = log(a); break;
    case 12: e = gvm_log2f(a), ref = log2(a); break;
    case 13: e = gvm_log10f(a), ref = log10(a); break;
    case 14: case 15: e = gvm_powf(a, b), ref = pow(a, b); break;
    case 16: e = gvm_asinf(a), ref = asin(a); break;
    case 17: h = 0, ref = acos(a); break;
    case 18: h = 0, ref = atan(a); break;
    case 19: h = 0, ref = atan2(a, b); break;
    case 20: e = gvm_sqrtf_(a), ref = sqrt(a); break;
    case 21: e = gvm_roundf(a), ref = round(a); break;
    default: fprintf(stderr, "bad record\n"); return 1;
    }
    double u = ulps(y, ref);
    n[s]++, sumu[s] += u;
    if (u > maxu[s]) maxu[s] = u;
    if (h) have[s]++, same[s] += fb(e) == fb(y) || (isnan(e) && isnan(y));
  }
  printf("%-28s %6s %16s %9s %9s\n", "set", "n", "same as host %", "max ulp", "mean ulp");
  for (int s = 0; s < NS; s++) {
    if (!n[s]) continue;
    char sm[32] = "-";
    if (have[s]) snprintf(sm, sizeof sm, "%.2f", 100.0 * same[s] / have[s]);
    printf("%-28s %6ld %16s %9.3f %9.4f\n", names[s], n[s], sm, maxu[s], sumu[s] / n[s]);
  }
  return 0;
}
