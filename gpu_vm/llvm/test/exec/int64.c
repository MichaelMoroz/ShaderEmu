// 64-bit integers in register pairs: sums by the carry ops, products by
// mulhu, shifts, compares, and division by compiler-rt.
#include "gvm.h"

#define NI __attribute__((noinline))
typedef long long i64;
typedef unsigned long long u64;

NI i64 add(i64 a, i64 b) { return (i64)((u64)a + (u64)b); }
NI i64 sub(i64 a, i64 b) { return (i64)((u64)a - (u64)b); }
NI u64 mul(u64 a, u64 b) { return a * b; }
NI i64 sdiv(i64 a, i64 b) { return a / b; }
NI i64 srem(i64 a, i64 b) { return a % b; }
NI u64 udiv(u64 a, u64 b) { return a / b; }
NI u64 urem(u64 a, u64 b) { return a % b; }
NI u64 shl(u64 a, int s) { return a << s; }
NI u64 lshr(u64 a, int s) { return a >> s; }
NI i64 ashr(i64 a, int s) { return a >> s; }
NI int lt(i64 a, i64 b) { return a < b; }
NI int ult(u64 a, u64 b) { return a < b; }
NI int eq(i64 a, i64 b) { return a == b; }
NI i64 neg(i64 a) { return -(u64)a; }
NI i64 sext(int a) { return a; }
NI u64 zext(unsigned a) { return a; }
NI int trunc(i64 a) { return (int)a; }
NI u64 mul32(unsigned a, unsigned b) { return (u64)a * b; }
NI i64 smul32(int a, int b) { return (i64)a * b; }
NI int clz64(u64 a) { return __builtin_clzll(a); }
NI int ctz64(u64 a) { return __builtin_ctzll(a); }
NI int pop64(u64 a) { return __builtin_popcountll(a); }
NI u64 bswap64(u64 a) { return __builtin_bswap64(a); }

// The reference: 32-bit halves by hand
static u64 ref_mul(u64 a, u64 b) {
  unsigned al = a, ah = a >> 32, bl = b, bh = b >> 32;
  u64 lo = (u64)al * bl;
  unsigned hi = (unsigned)(lo >> 32) + al * bh + ah * bl;
  return (u64)hi << 32 | (unsigned)lo;
}

static u64 ref_udiv(u64 a, u64 b, u64 *rem) {
  u64 q = 0, r = 0;
  for (int i = 63; i >= 0; i--) {
    r = (r << 1) | ((a >> i) & 1);
    if (r >= b) {
      r -= b;
      q |= 1ull << i;
    }
  }
  *rem = r;
  return q;
}

int main(void) {
  CHECK_EQ(add(0xffffffffll, 1), 0x100000000ll);
  CHECK_EQ(add(-1, 1), 0);
  CHECK_EQ(sub(0x100000000ll, 1), 0xffffffffll);
  CHECK_EQ(sub(0, 1), -1);
  CHECK_EQ(neg(5), -5);
  CHECK_EQ(neg(0x100000000ll), -0x100000000ll);
  CHECK_EQ(sext(-3), -3ll);
  CHECK_EQ(zext(0xfffffffdu), 0xfffffffdll);
  CHECK_EQ(trunc(0x123456789ll), 0x23456789);
  CHECK_EQ(mul32(0xffffffffu, 0xffffffffu) == 0xfffffffe00000001ull, 1);
  CHECK_EQ(smul32(-2, 0x40000000), -0x80000000ll);
  CHECK_EQ(clz64(1), 63);
  CHECK_EQ(clz64(0x100000000ull), 31);
  CHECK_EQ(ctz64(0x100000000ull), 32);
  CHECK_EQ(pop64(0xf0f0f0f0f0f0f0f0ull), 32);
  CHECK(bswap64(0x0102030405060708ull) == 0x0807060504030201ull);
  u64 h = 0x9e3779b97f4a7c15ull;
  for (int i = 0; i < 300; i++) {
    h ^= h << 13;
    h ^= h >> 7;
    h ^= h << 17;
    u64 a = h, b = (h >> (i % 61)) | 1;
    CHECK(mul(a, b) == ref_mul(a, b));
    u64 r, q = ref_udiv(a, b, &r);
    CHECK(udiv(a, b) == q);
    CHECK(urem(a, b) == r);
    i64 sa = (i64)a, sb = (i64)b;
    if (sb != -1) {
      i64 sq = sdiv(sa, sb), sr = srem(sa, sb);
      CHECK(sq * sb + sr == sa);
      CHECK((sr == 0) || ((sr < 0) == (sa < 0)));
    }
    int s = i % 64;
    CHECK(shl(a, s) == (s ? ref_mul(a, 1ull << s) : a));
    CHECK(lshr(a, s) == ref_udiv(a, 1ull << s, &r));
    CHECK(ashr(sa, s) == (sa < 0 ? ~(i64)(~(u64)sa >> s) : (i64)((u64)sa >> s)));
    CHECK_EQ(lt(sa, sb), sa < sb);
    CHECK_EQ(ult(a, b), a < b);
    CHECK_EQ(eq(a, b), a == b);
    CHECK_EQ(eq(a, a), 1);
  }
  puts("int64 ok");
  return 0;
}
