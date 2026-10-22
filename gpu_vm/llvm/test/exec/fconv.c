// Float conversions and roundings made inline (gpu_vm/llvm/README.md, "float"): f32 to f64 and back by their bits,
// against compiler-rt's __extendsfdf2 and __truncdfsf2 called by name over edge cases and random values (a denormal
// float, or a double below 2^-126, is +-0: the card's floats have none); round, lround, lrint; f32 to i64 and u64.
#include "gvm.h"

double __extendsfdf2(float);
float __truncdfsf2(double);
long long __fixsfdi(float);
unsigned long long __fixunssfdi(float);

#define NI __attribute__((noinline))
NI double ext(float x) { return x; }
NI float trn(double x) { return (float)x; }
NI float rnd(float x) { return __builtin_roundf(x); }
NI long lrnd(float x) { return __builtin_lroundf(x); }
NI long lrin(float x) { return __builtin_lrintf(x); }
NI long long f2i64(float x) { return (long long)x; }
NI unsigned long long f2u64(float x) { return (unsigned long long)x; }

static unsigned s = 0x9e3779b9u;
static unsigned next(void) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}
static unsigned fb(float f) {
  union { float f; unsigned u; } v = {f};
  return v.u;
}
static float bf(unsigned u) {
  union { unsigned u; float f; } v = {u};
  return v.f;
}
static unsigned long long db(double d) {
  union { double d; unsigned long long u; } v = {d};
  return v.u;
}
static double bd(unsigned long long u) {
  union { unsigned long long u; double d; } v = {u};
  return v.d;
}

static void check_ext(unsigned u) {
  double got = ext(bf(u));
  if ((u & 0x7f800000u) == 0) {   // zero and the denormals: +-0 (at -O0, optnone: compiler-rt's exact value)
    if (db(got) != (unsigned long long)(u & 0x80000000u) << 32 && db(got) != db(__extendsfdf2(bf(u)))) {
      printf("ext %08x: %016llx, want +-0\n", u, db(got));
      exit(1);
    }
    return;
  }
  double want = __extendsfdf2(bf(u));
  if (db(got) != db(want)) {
    printf("ext %08x: %016llx, want %016llx\n", u, db(got), db(want));
    exit(1);
  }
}

static void check_trn(unsigned long long u) {
  unsigned got = fb(trn(bd(u))), want = fb(__truncdfsf2(bd(u)));
  if ((want & 0x7f800000u) == 0 && (u & 0x7ff0000000000000ull) != 0x7ff0000000000000ull) {
    // below 2^-126: +-0 (compiler-rt's may be a denormal, or 2^-126 rounded up from just below it)
    CHECK(got == (want & 0x80000000u) || got == ((unsigned)(u >> 32) & 0x80000000u));
    return;
  }
  if ((want & 0x7fffffffu) > 0x7f800000u) {   // nan: a nan
    CHECK((got & 0x7fffffffu) > 0x7f800000u);
    return;
  }
  if (got != want && !((want & 0x7fffffffu) == 0x00800000u && (got & 0x7fffffffu) == 0)) {
    printf("trn %016llx: %08x, want %08x\n", u, got, want);
    exit(1);
  }
}

int main(void) {
  static const unsigned fe[] = {0, 0x80000000u, 1, 0x007fffffu, 0x00800000u, 0x3f800000u, 0xbf800000u, 0x7f7fffffu,
                                0x7f800000u, 0xff800000u, 0x7fc00000u, 0x7fa00001u, 0x34000000u, 0x4b800001u};
  for (unsigned i = 0; i < sizeof fe / sizeof fe[0]; i++) check_ext(fe[i]);
  for (int i = 0; i < 100000; i++) check_ext(next());
  static const unsigned long long de[] = {0, 0x8000000000000000ull, 0x3ff0000000000000ull, 0x47efffffe0000000ull,
    0x47effffff0000000ull, 0x47efffffefffffffull, 0x47f0000000000000ull, 0x7ff0000000000000ull, 0x7ff8000000000000ull,
    0x7ff0000000000001ull, 0x3810000000000000ull, 0x380fffffffffffffull, 0x3ff0000010000000ull, 0x3ff0000030000000ull,
    0x3ff0000010000001ull, 0x3fefffffffffffffull, 0x0000000000000001ull, 0x36a0000000000000ull};
  for (unsigned i = 0; i < sizeof de / sizeof de[0]; i++) check_trn(de[i]), check_trn(de[i] ^ 0x8000000000000000ull);
  for (int i = 0; i < 100000; i++) {
    unsigned long long u = (unsigned long long)next() << 32 | next();
    check_trn(u);
    // doubles a float holds nearly: a float's exponent range, every rounding case of the low bits
    check_trn((u & 0x800fffffffffffffull) | (unsigned long long)(0x380 + (next() % 0xfe)) << 52);
  }

  // round half away from zero, lround, lrint (to even)
  CHECK(rnd(0.5f) == 1 && rnd(-0.5f) == -1 && rnd(0.49999997f) == 0 && rnd(1.5f) == 2 && rnd(2.5f) == 3);
  CHECK(rnd(-2.5f) == -3 && rnd(8388607.5f) == 8388608.0f && rnd(8388609.0f) == 8388609.0f);
  CHECK(fb(rnd(-0.0f)) == 0x80000000u && fb(rnd(-0.3f)) == 0x80000000u && rnd(1e30f) == 1e30f);
  CHECK(lrnd(2.5f) == 3 && lrnd(-2.5f) == -3 && lrnd(1234567.4f) == 1234567);
  CHECK(lrin(2.5f) == 2 && lrin(3.5f) == 4 && lrin(-2.5f) == -2 && lrin(-0.7f) == -1);
  for (int i = 0; i < 20000; i++) {
    float x = bf(next() & 0xcbffffffu);   // |x| below 2^24 or so
    float r = rnd(x), t = __builtin_truncf(x), d = x - t;
    float want = (d >= 0.5f) ? t + 1 : (d <= -0.5f) ? t - 1 : t;
    CHECK(r == want);
  }

  // f32 to i64 and u64 against compiler-rt
  static const float ie[] = {0, -0.0f, 0.9f, -0.9f, 1, -1, 4294967295.0f, 4294967296.0f, -4294967296.0f, 1e18f,
                             -1e18f, 9.2233715e18f, -9.2233715e18f, 123456789012.0f, -0.5f};
  for (unsigned i = 0; i < sizeof ie / sizeof ie[0]; i++) CHECK(f2i64(ie[i]) == __fixsfdi(ie[i]));
  for (int i = 0; i < 20000; i++) {
    float x = bf(next() & 0xdeffffffu);   // |x| below 2^62
    if (f2i64(x) != __fixsfdi(x)) {
      printf("f2i64 %08x: %lld, want %lld\n", fb(x), f2i64(x), __fixsfdi(x));
      exit(1);
    }
    if (x >= 0) CHECK(f2u64(x) == __fixunssfdi(x));
  }
  CHECK(f2u64(1.8e19f) == __fixunssfdi(1.8e19f));
  puts("fconv ok");
  return 0;
}
