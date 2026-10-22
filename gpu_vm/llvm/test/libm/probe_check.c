// probe.c's records against the host's double libm (gcc -O2 probe_check.c -lm): for each set the largest and the mean
// error in ulps of the float result, the share correctly rounded, and the largest absolute error.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static float bf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static double ulp_of(double r) {
  int e;
  if (r == 0) return ldexp(1, -149);
  frexp(fabs(r), &e);
  if (e - 1 < -126) e = -125;
  return ldexp(1, e - 1 - 23);
}

static const char *names[] = {"fsin [-pi, pi]", "fsin [-1000, 1000]", "fsin 2^-24..2^-2", "fcos [-pi, pi]",
                              "fcos [-1000, 1000]", "fexp2 [-1, 1]", "fexp2 [-125, 127]", "fexp2 2^-24..2^-4",
                              "flog2 [0.5, 2]", "flog2 any", "fsqrt", "frsqrt", "frcp", "fdiv any", "fdiv [1, 2)"};
#define NS 15

int main(int argc, char **argv) {
  FILE *f = argc > 1 ? fopen(argv[1], "rb") : stdin;
  if (!f) return 1;
  double maxu[NS] = {0}, sumu[NS] = {0}, maxa[NS] = {0}, worst[NS] = {0};
  long n[NS] = {0}, exact[NS] = {0};
  uint32_t r[4];
  while (fread(r, 4, 4, f) == 4) {
    unsigned s = r[0];
    if (s >= NS) { fprintf(stderr, "bad record\n"); return 1; }
    double a = bf(r[1]), b = bf(r[2]), y = bf(r[3]), ref;
    switch (s) {
    case 0: case 1: case 2: ref = sin(a); break;
    case 3: case 4: ref = cos(a); break;
    case 5: case 6: case 7: ref = exp2(a); break;
    case 8: case 9: ref = log2(a); break;
    case 10: ref = sqrt(a); break;
    case 11: ref = 1 / sqrt(a); break;
    case 12: ref = 1 / a; break;
    default: ref = a / b; break;
    }
    if (fabs(ref) < 0x1p-126 || isinf((float)ref)) continue;   // flushed or out of range
    double u = fabs(y - ref) / ulp_of(ref), ab = fabs(y - ref);
    n[s]++;
    sumu[s] += u;
    if (u > maxu[s]) maxu[s] = u, worst[s] = a;
    if (ab > maxa[s]) maxa[s] = ab;
    if ((float)ref == (float)y) exact[s]++;
  }
  printf("%-22s %8s %10s %9s %8s %10s %s\n", "set", "n", "max ulp", "mean ulp", "exact %", "max abs", "worst x");
  for (int s = 0; s < NS; s++)
    if (n[s])
      printf("%-22s %8ld %10.1f %9.2f %8.1f %10.2e %g\n", names[s], n[s], maxu[s], sumu[s] / n[s],
             100.0 * exact[s] / n[s], maxa[s], worst[s]);
  return 0;
}
