// The polynomials of gpu_vm's libm (gpu_vm/userland/musl/src/math/gpuvm): weighted minimax fits by Lawson's algorithm
// (iteratively reweighted least squares on a dense grid, long double), printed as float literals.
//   gcc -O2 fit.c -o fit -lm && ./fit [NAME]
// Each fit is of a core g(t) on [a, b] with a weight w(t): the function's error is w(t) (g(t) - p(t)) relative to
// its value, so the weight is what carries the core's error into the result.
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef long double R;
#define M 4000

static R sinc3(R z) { R r = sqrtl(z); return z < 1e-12L ? -1.0L / 6 + z / 120 : (sinl(r) - r) / (z * r); }
static R cosc(R z) { return z < 1e-12L ? 1.0L / 24 - z / 720 : (cosl(sqrtl(z)) - 1 + z / 2) / (z * z); }
static R tanc(R z) { R r = sqrtl(z); return z < 1e-12L ? 1.0L / 3 + 2 * z / 15 : (tanl(r) - r) / (z * r); }
static R atanc(R z) { R r = sqrtl(z); return z < 1e-12L ? -1.0L / 3 + z / 5 : (atanl(r) - r) / (z * r); }
static R exp2c(R r) { return fabsl(r) < 1e-12L ? logl(2) : (exp2l(r) - 1) / r; }
static R expc(R s) { return fabsl(s) < 1e-9L ? 0.5L + s / 6 : (expl(s) - 1 - s) / (s * s); }
static R log1pc(R r) { return fabsl(r) < 1e-9L ? 1.0L / 3 - r / 4 : (log1pl(r) - r + r * r / 2) / (r * r * r); }
static R log2c(R r) { return fabsl(r) < 1e-9L ? -0.5L / logl(2) + r / (3 * logl(2)) : (log1pl(r) / logl(2) - r / logl(2)) / (r * r); }

static R w_z(R z) { return z; }             // sin, tan, atan: the core's error times z, relative to r
static R w_cos(R z) { return z * z; }       // cos: z^2 Q, relative to about 1
static R w_exp2(R r) { return fabsl(r) / exp2l(r); }
static R w_exp(R s) { return s * s / expl(s); }
static R w_log1p(R r) { return r * r; }     // r^3 X relative to r
static R w_log2(R r) { return fabsl(r); }   // r^2 Y relative to r

struct fit { const char *name; R (*g)(R); R (*w)(R); R a, b; int deg; };
static struct fit fits[] = {
  {"sin", sinc3, w_z, 0, 0.6169L, 2},
  {"sin3", sinc3, w_z, 0, 0.6169L, 3},
  {"cos", cosc, w_cos, 0, 0.6169L, 2},
  {"cos3", cosc, w_cos, 0, 0.6169L, 3},
  {"tan", tanc, w_z, 0, 0.6169L, 5},
  {"tan6", tanc, w_z, 0, 0.6169L, 6},
  {"atan", atanc, w_z, 0, 0.17158L, 4},
  {"atan5", atanc, w_z, 0, 0.17158L, 5},
  {"exp2", exp2c, w_exp2, -0.5L, 0.5L, 5},
  {"exp", expc, w_exp, -0.3466L, 0.3466L, 4},
  {"exp5", expc, w_exp, -0.3466L, 0.3466L, 5},
  {"log1p", log1pc, w_log1p, -0.0079L, 0.0079L, 2},
  {"log1p3", log1pc, w_log1p, -0.0079L, 0.0079L, 3},
  {"log2", log2c, w_log2, -0.0079L, 0.0079L, 3},
  {"log2_4", log2c, w_log2, -0.0079L, 0.0079L, 4},
};

static void solve(int n, R A[][10], R *b) {   // Gaussian elimination with partial pivoting
  for (int i = 0; i < n; i++) {
    int p = i;
    for (int k = i + 1; k < n; k++)
      if (fabsl(A[k][i]) > fabsl(A[p][i])) p = k;
    for (int j = 0; j < n; j++) { R t = A[i][j]; A[i][j] = A[p][j]; A[p][j] = t; }
    R t = b[i]; b[i] = b[p]; b[p] = t;
    for (int k = i + 1; k < n; k++) {
      R f = A[k][i] / A[i][i];
      for (int j = i; j < n; j++) A[k][j] -= f * A[i][j];
      b[k] -= f * b[i];
    }
  }
  for (int i = n - 1; i >= 0; i--) {
    for (int j = i + 1; j < n; j++) b[i] -= A[i][j] * b[j];
    b[i] /= A[i][i];
  }
}

static void run(struct fit *f) {
  static R t[M], g[M], w[M], lw[M];
  int n = f->deg + 1;
  for (int i = 0; i < M; i++) {
    t[i] = f->a + (f->b - f->a) * (0.5L - 0.5L * cosl(M_PI * i / (M - 1)));   // denser at the ends
    g[i] = f->g(t[i]);
    w[i] = f->w(t[i]);
    lw[i] = 1;
  }
  R c[10], err = 0;
  for (int it = 0; it < 400; it++) {
    R A[10][10] = {{0}}, b[10] = {0};
    for (int i = 0; i < M; i++) {
      R ww = lw[i] * w[i] * w[i], p = 1, pw[10];
      for (int j = 0; j < n; j++) pw[j] = p, p *= t[i];
      for (int j = 0; j < n; j++) {
        b[j] += ww * pw[j] * g[i];
        for (int k = 0; k < n; k++) A[j][k] += ww * pw[j] * pw[k];
      }
    }
    solve(n, A, b);
    memcpy(c, b, sizeof c);
    R sum = 0;
    err = 0;
    for (int i = 0; i < M; i++) {
      R p = 0;
      for (int j = n - 1; j >= 0; j--) p = p * t[i] + c[j];
      R e = fabsl(w[i] * (g[i] - p));
      if (e > err) err = e;
      lw[i] *= e + 1e-40L;
      sum += lw[i];
    }
    for (int i = 0; i < M; i++) lw[i] /= sum;
  }
  printf("%s: degree %d on [%Lg, %Lg], weighted error %.3Lg (2^%.1Lf)\n", f->name, f->deg, f->a, f->b, err,
         log2l(err));
  for (int j = 0; j < n; j++) printf("  c%d = %.9ef  /* %a */\n", j, (float)c[j], (float)c[j]);
}

int main(int argc, char **argv) {
  for (unsigned i = 0; i < sizeof fits / sizeof fits[0]; i++)
    if (argc < 2 || !strcmp(argv[1], fits[i].name)) run(&fits[i]);
  return 0;
}
