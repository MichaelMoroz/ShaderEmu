// Narrow types and bit fields: char is signed, loads and stores of every
// width, sign and zero extension, bit fields across words, unaligned access.
#include "gvm.h"

#define NI __attribute__((noinline))

struct bf {
  unsigned a : 3;
  signed b : 5;
  unsigned c : 12;
  signed d : 12;
  unsigned e : 20;
  unsigned f : 1;
};

struct __attribute__((packed)) pk {
  char c;
  int i;
  short s;
  long long l;
};

NI int load_s8(const signed char *p) { return *p; }
NI int load_u8(const unsigned char *p) { return *p; }
NI int load_s16(const short *p) { return *p; }
NI int load_u16(const unsigned short *p) { return *p; }
NI void store8(char *p, int v) { *p = v; }
NI void store16(short *p, int v) { *p = v; }
NI int idx_s8(const signed char *p, int i) { return p[i]; }
NI int idx_u16(const unsigned short *p, int i) { return p[i]; }
NI int idx_32(const int *p, int i) { return p[i]; }
NI void idx_st32(int *p, int i, int v) { p[i] = v; }
NI int getd(struct bf *b) { return b->d; }
NI void setd(struct bf *b, int v) { b->d = v; }
NI int pk_i(struct pk *p) { return p->i; }
NI long long pk_l(struct pk *p) { return p->l; }

int main(void) {
  char c = (char)200;
  CHECK(c < 0);
  signed char sc[4] = {-1, 127, -128, 5};
  unsigned char uc[4] = {255, 127, 128, 5};
  CHECK_EQ(load_s8(&sc[0]), -1);
  CHECK_EQ(load_s8(&sc[2]), -128);
  CHECK_EQ(load_u8(&uc[0]), 255);
  short ss[3] = {-1, 32767, -32768};
  unsigned short us[3] = {65535, 32767, 32768};
  CHECK_EQ(load_s16(&ss[2]), -32768);
  CHECK_EQ(load_u16(&us[0]), 65535);
  char cbuf[4];
  store8(cbuf + 1, 0x1ff);
  CHECK_EQ((unsigned char)cbuf[1], 0xff);
  short sbuf[2];
  store16(sbuf + 1, 0x18001);
  CHECK_EQ(sbuf[1], -32767);
  CHECK_EQ(idx_s8(sc, 2), -128);
  CHECK_EQ(idx_u16(us, 2), 32768);
  int ib[10];
  for (int i = 0; i < 10; i++)
    idx_st32(ib, i, i * i);
  CHECK_EQ(idx_32(ib, 7), 49);

  struct bf b = {5, -3, 4000, -2000, 999999, 1};
  CHECK_EQ(b.a, 5);
  CHECK_EQ(b.b, -3);
  CHECK_EQ(b.c, 4000);
  CHECK_EQ(getd(&b), -2000);
  setd(&b, 1234);
  CHECK_EQ(b.d, 1234);
  CHECK_EQ(b.c, 4000);
  CHECK_EQ(b.e, 999999);
  CHECK_EQ(b.f, 1);
  b.e += 1;
  CHECK_EQ(b.e, 1000000);
  CHECK_EQ(b.f, 1);

  struct pk p;
  p.c = 1;
  p.i = 0x12345678;
  p.s = -2;
  p.l = 0x1122334455667788ll;
  CHECK_EQ(pk_i(&p), 0x12345678);
  CHECK(pk_l(&p) == 0x1122334455667788ll);
  CHECK_EQ(sizeof(struct pk), 15);
  unsigned char raw[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  unsigned w;
  memcpy(&w, raw + 3, 4);
  CHECK_EQ(w, 0x06050403u);
  puts("bits ok");
  return 0;
}
