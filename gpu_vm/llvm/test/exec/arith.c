// Integer arithmetic of 32 bits: every op with registers and with constants
// (short and long immediates), division by the ISA's ops, the bit ops.
#include "gvm.h"

#define NI __attribute__((noinline))
static volatile int vi[] = {0, 1, -1, 7, -7, 123456789, -123456789, 0x7fffffff,
                            (int)0x80000000, 2048, -2048, 2047, 4096, 0x1234};
static volatile unsigned vu[] = {0, 1, 0xffffffffu, 7, 0x80000000u, 3000000000u,
                                 12345, 0xdeadbeefu, 2047, 2048};

NI int add(int a, int b) { return (int)((unsigned)a + (unsigned)b); }
NI int sub(int a, int b) { return a - b; }
NI int mul(int a, int b) { return (int)((unsigned)a * (unsigned)b); }
NI int sdiv(int a, int b) { return a / b; }
NI int srem(int a, int b) { return a % b; }
NI unsigned udiv(unsigned a, unsigned b) { return a / b; }
NI unsigned urem(unsigned a, unsigned b) { return a % b; }
NI int shl(int a, int b) { return (int)((unsigned)a << b); }
NI int sra(int a, int b) { return a >> b; }
NI unsigned srl(unsigned a, int b) { return a >> b; }
NI unsigned rotl(unsigned a, int b) { return (a << b) | (a >> ((32 - b) & 31)); }
NI unsigned rotr(unsigned a, int b) { return (a >> b) | (a << ((32 - b) & 31)); }
NI int mulhs(int a, int b) { return (int)(((long long)a * b) >> 32); }
NI unsigned mulhu(unsigned a, unsigned b) {
  return (unsigned)(((unsigned long long)a * b) >> 32);
}
NI int andn(int a, int b) { return a & ~b; }
NI int orn(int a, int b) { return a | ~b; }
NI int xnor(int a, int b) { return ~(a ^ b); }
NI int smin(int a, int b) { return a < b ? a : b; }
NI int smax(int a, int b) { return a > b ? a : b; }
NI unsigned umin(unsigned a, unsigned b) { return a < b ? a : b; }
NI unsigned umax(unsigned a, unsigned b) { return a > b ? a : b; }
NI int iabs(int a) { return a < 0 ? -a : a; }
NI int sext8(int a) { return (signed char)a; }
NI int sext16(int a) { return (short)a; }
NI unsigned zext8(unsigned a) { return (unsigned char)a; }
NI unsigned zext16(unsigned a) { return (unsigned short)a; }
NI unsigned field(unsigned a) { return (a >> 7) & 0x1fff; }
NI unsigned shadd(unsigned a, unsigned b) { return a + (b << 3); }

static int ref_clz(unsigned x) {
  int n = 0;
  while (n < 32 && !(x & (0x80000000u >> n)))
    n++;
  return n;
}
static int ref_ctz(unsigned x) {
  int n = 0;
  while (n < 32 && !(x & (1u << n)))
    n++;
  return n;
}
static int ref_pop(unsigned x) {
  int n = 0;
  for (; x; x >>= 1)
    n += x & 1;
  return n;
}

int main(void) {
  CHECK_EQ(add(vi[5], vi[6]), 0);
  CHECK_EQ(add(vi[7], vi[1]), (int)0x80000000);
  CHECK_EQ(sub(vi[3], vi[4]), 14);
  CHECK_EQ(mul(vi[5], vi[3]), (int)864197523);
  CHECK_EQ(mul(vi[6], vi[4]), (int)864197523);
  CHECK_EQ(sdiv(vi[5], vi[3]), 17636684);
  CHECK_EQ(sdiv(vi[6], vi[3]), -17636684);
  CHECK_EQ(srem(vi[6], vi[3]), -1);
  CHECK_EQ(srem(vi[5], vi[4]), 1);
  CHECK_EQ(udiv(vu[5], vu[6]), 243013u);
  CHECK_EQ(urem(vu[5], vu[6]), 3000000000u - 243013u * 12345u);
  CHECK_EQ(udiv(vu[2], vu[3]), 0xffffffffu / 7);
  CHECK_EQ(shl(vi[3], 30), (int)0xc0000000);
  CHECK_EQ(sra(vi[8], 31), -1);
  CHECK_EQ(srl(vu[4], 31), 1);
  CHECK_EQ(rotl(vu[7], 8), 0xadbeefdeu);
  CHECK_EQ(rotr(vu[7], 8), 0xefdeadbeu);
  CHECK_EQ(rotl(vu[7], 0), 0xdeadbeefu);
  CHECK_EQ(mulhs(vi[6], vi[5]), (int)(((long long)-123456789 * 123456789) >> 32));
  CHECK_EQ(mulhu(vu[7], vu[5]), (unsigned)(((unsigned long long)0xdeadbeefu * 3000000000u) >> 32));
  CHECK_EQ(andn(vi[13], vi[3]), 0x1230);
  CHECK_EQ(orn(vi[0], vi[3]), ~7);
  CHECK_EQ(xnor(vi[13], vi[13]), -1);
  CHECK_EQ(smin(vi[7], vi[8]), (int)0x80000000);
  CHECK_EQ(smax(vi[7], vi[8]), 0x7fffffff);
  CHECK_EQ(umin(vu[4], vu[3]), 7);
  CHECK_EQ(umax(vu[4], vu[3]), 0x80000000u);
  CHECK_EQ(iabs(vi[6]), 123456789);
  CHECK_EQ(sext8(vi[13] | 0x80), (signed char)0xb4);
  CHECK_EQ(sext16(0x18000), -32768);
  CHECK_EQ(zext8(vu[7]), 0xef);
  CHECK_EQ(zext16(vu[7]), 0xbeef);
  CHECK_EQ(field(vu[7]), (0xdeadbeefu >> 7) & 0x1fff);
  CHECK_EQ(shadd(vu[3], vu[3]), 63);

  // constants of every size
  int x = vi[3];
  CHECK_EQ(x + 2047, 2054);
  CHECK_EQ(x + 2048, 2055);
  CHECK_EQ(x - 2049, -2042);
  CHECK_EQ(x * 100000, 700000);
  CHECK_EQ(x ^ 0x12345678, 0x1234567f);
  CHECK_EQ(x | 0xff00, 0xff07);
  CHECK_EQ(x & -4, 4);
  CHECK_EQ(x << 3, 56);

  for (unsigned i = 0; i < sizeof vu / sizeof vu[0]; i++) {
    unsigned v = vu[i];
    if (v) {
      CHECK_EQ(__builtin_clz(v), ref_clz(v));
      CHECK_EQ(__builtin_ctz(v), ref_ctz(v));
    }
    CHECK_EQ(__builtin_popcount(v), ref_pop(v));
    CHECK_EQ(__builtin_bswap32(v),
             (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24));
    unsigned r = 0;
    for (int b = 0; b < 32; b++)
      r |= ((v >> b) & 1) << (31 - b);
    CHECK_EQ(__builtin_bitreverse32(v), r);
  }
  CHECK_EQ(__builtin_bswap16(0x1234), 0x3412);

  // a loop of every op against a reference computed another way
  unsigned h = 1;
  for (unsigned i = 0; i < 1000; i++) {
    h = h * 1103515245u + 12345u;
    unsigned a = h, b = (h >> 7) | 1;
    CHECK_EQ(a / b * b + a % b, a);
    int sa = (int)a, sb = (int)b;
    if (sb != -1)
      CHECK_EQ(sa / sb * sb + sa % sb, sa);
  }
  puts("arith ok");
  return 0;
}
