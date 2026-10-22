// Compares and branches: every condition on registers, on imm6 and on long
// immediates, signed and unsigned, bit tests, selects, and far branches
// (a body longer than a conditional branch reaches).
#include "gvm.h"

#define NI __attribute__((noinline))
static volatile int vals[] = {0,  1,  -1, 31, 32, -32, -33, 100, -100, 2047,
                              2048, 0x7fffffff, (int)0x80000000, 5, -5};
#define N (int)(sizeof vals / sizeof vals[0])

NI int br_eq(int a, int b) { if (a == b) return 1; return 0; }
NI int br_ne(int a, int b) { if (a != b) return 1; return 0; }
NI int br_lt(int a, int b) { if (a < b) return 1; return 0; }
NI int br_le(int a, int b) { if (a <= b) return 1; return 0; }
NI int br_gt(int a, int b) { if (a > b) return 1; return 0; }
NI int br_ge(int a, int b) { if (a >= b) return 1; return 0; }
NI int br_ult(unsigned a, unsigned b) { if (a < b) return 1; return 0; }
NI int br_ule(unsigned a, unsigned b) { if (a <= b) return 1; return 0; }
NI int br_ugt(unsigned a, unsigned b) { if (a > b) return 1; return 0; }
NI int br_uge(unsigned a, unsigned b) { if (a >= b) return 1; return 0; }

#define IMM_TESTS(K)                                                           \
  NI int ieq##K(int a) { if (a == (K)) return 1; return 0; }                   \
  NI int ine##K(int a) { if (a != (K)) return 1; return 0; }                   \
  NI int ilt##K(int a) { if (a < (K)) return 1; return 0; }                    \
  NI int ile##K(int a) { if (a <= (K)) return 1; return 0; }                   \
  NI int igt##K(int a) { if (a > (K)) return 1; return 0; }                    \
  NI int ige##K(int a) { if (a >= (K)) return 1; return 0; }                   \
  NI int iult##K(unsigned a) { if (a < (unsigned)(K)) return 1; return 0; }    \
  NI int iule##K(unsigned a) { if (a <= (unsigned)(K)) return 1; return 0; }   \
  NI int iugt##K(unsigned a) { if (a > (unsigned)(K)) return 1; return 0; }    \
  NI int iuge##K(unsigned a) { if (a >= (unsigned)(K)) return 1; return 0; }   \
  NI int seq##K(int a) { return a == (K); }                                    \
  NI int slt##K(int a) { return a < (K); }                                     \
  NI int sgt##K(int a) { return a > (K); }                                     \
  NI int sult##K(unsigned a) { return a < (unsigned)(K); }                     \
  NI int sugt##K(unsigned a) { return a > (unsigned)(K); }                     \
  static void check##K(int a) {                                                \
    unsigned u = a, k = (unsigned)(K);                                         \
    CHECK_EQ(ieq##K(a), a == (K));                                             \
    CHECK_EQ(ine##K(a), a != (K));                                             \
    CHECK_EQ(ilt##K(a), a < (K));                                              \
    CHECK_EQ(ile##K(a), a <= (K));                                             \
    CHECK_EQ(igt##K(a), a > (K));                                              \
    CHECK_EQ(ige##K(a), a >= (K));                                             \
    CHECK_EQ(iult##K(u), u < k);                                               \
    CHECK_EQ(iule##K(u), u <= k);                                              \
    CHECK_EQ(iugt##K(u), u > k);                                               \
    CHECK_EQ(iuge##K(u), u >= k);                                              \
    CHECK_EQ(seq##K(a), a == (K));                                             \
    CHECK_EQ(slt##K(a), a < (K));                                              \
    CHECK_EQ(sgt##K(a), a > (K));                                              \
    CHECK_EQ(sult##K(u), u < k);                                               \
    CHECK_EQ(sugt##K(u), u > k);                                               \
  }

#define M1 (-1)
#define M32 (-32)
#define M33 (-33)
#define BIG 100000
#define MBIG (-100000)
IMM_TESTS(0)
IMM_TESTS(1)
IMM_TESTS(M1)
IMM_TESTS(30)
IMM_TESTS(31)
IMM_TESTS(32)
IMM_TESTS(M32)
IMM_TESTS(M33)
IMM_TESTS(BIG)
IMM_TESTS(MBIG)

NI int bit5(unsigned a) { if (a & 32) return 1; return 0; }
NI int nbit31(unsigned a) { if (!(a & 0x80000000u)) return 1; return 0; }
NI int sel(int c, int a, int b) { return c ? a : b; }
NI float fsel(int c, float a, float b) { return c > 3 ? a : b; }

// A loop whose body is longer than 256 bundles: the back edge is relaxed.
NI unsigned far_loop(int n, volatile int *p) {
  unsigned s = 0;
  for (int i = 0; i < n; i++) {
#define S10 s += p[0]; s ^= p[1]; s += p[2] * 3; s -= p[3]; s += i;           \
            p[4] = s; s += p[5]; s ^= s >> 3; s += p[6]; s *= 5;
#define S100 S10 S10 S10 S10 S10 S10 S10 S10 S10 S10
    S100 S100 S100
    if (s == 12345)
      break;
  }
  return s;
}

static unsigned far_ref(int n, volatile int *p) {
  unsigned s = 0;
  for (int i = 0; i < n; i++) {
    for (int k = 0; k < 30; k++) {
      s += p[0]; s ^= p[1]; s += p[2] * 3; s -= p[3]; s += i;
      p[4] = s; s += p[5]; s ^= s >> 3; s += p[6]; s *= 5;
    }
    if (s == 12345)
      break;
  }
  return s;
}

int main(void) {
  for (int i = 0; i < N; i++) {
    int a = vals[i];
    for (int j = 0; j < N; j++) {
      int b = vals[j];
      unsigned ua = a, ub = b;
      CHECK_EQ(br_eq(a, b), a == b);
      CHECK_EQ(br_ne(a, b), a != b);
      CHECK_EQ(br_lt(a, b), a < b);
      CHECK_EQ(br_le(a, b), a <= b);
      CHECK_EQ(br_gt(a, b), a > b);
      CHECK_EQ(br_ge(a, b), a >= b);
      CHECK_EQ(br_ult(ua, ub), ua < ub);
      CHECK_EQ(br_ule(ua, ub), ua <= ub);
      CHECK_EQ(br_ugt(ua, ub), ua > ub);
      CHECK_EQ(br_uge(ua, ub), ua >= ub);
    }
    check0(a); check1(a); checkM1(a); check30(a); check31(a); check32(a);
    checkM32(a); checkM33(a); checkBIG(a); checkMBIG(a);
    CHECK_EQ(bit5(a), (a & 32) != 0);
    CHECK_EQ(nbit31(a), a >= 0);
    CHECK_EQ(sel(a, 3, 4), a ? 3 : 4);
    CHECK(fsel(a, 1.5f, 2.5f) == (a > 3 ? 1.5f : 2.5f));
  }
  static volatile int arr[8] = {1, 2, 3, 4, 0, 6, 7, 8};
  static volatile int arr2[8] = {1, 2, 3, 4, 0, 6, 7, 8};
  CHECK_EQ(far_loop(5, arr), far_ref(5, arr2));
  puts("branches ok");
  return 0;
}
