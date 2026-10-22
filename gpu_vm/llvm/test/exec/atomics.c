// Atomics: words by the amo ops and amocas, bytes and halves by a word's
// cmpxchg, fences, and inline asm with the ISA's ops.
#include "gvm.h"

#define NI __attribute__((noinline))

static int w;
static short h[4];
static char b[8];
static unsigned u;

NI int add(int v) { return __atomic_fetch_add(&w, v, __ATOMIC_SEQ_CST); }
NI int sub(int v) { return __atomic_fetch_sub(&w, v, __ATOMIC_SEQ_CST); }
NI int and_(int v) { return __atomic_fetch_and(&w, v, __ATOMIC_SEQ_CST); }
NI int or_(int v) { return __atomic_fetch_or(&w, v, __ATOMIC_SEQ_CST); }
NI int xor_(int v) { return __atomic_fetch_xor(&w, v, __ATOMIC_SEQ_CST); }
NI int nand(int v) { return __atomic_fetch_nand(&w, v, __ATOMIC_SEQ_CST); }
NI int xchg(int v) { return __atomic_exchange_n(&w, v, __ATOMIC_SEQ_CST); }
NI int cas(int e, int v) {
  return __atomic_compare_exchange_n(&w, &e, v, 0, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST);
}
NI int smin(int v) { return __atomic_fetch_min(&w, v, __ATOMIC_SEQ_CST); }
NI unsigned umax(unsigned v) {
  return __atomic_fetch_max(&u, v, __ATOMIC_SEQ_CST);
}
NI short hadd(int i, short v) {
  return __atomic_fetch_add(&h[i], v, __ATOMIC_SEQ_CST);
}
NI char bxchg(int i, char v) {
  return __atomic_exchange_n(&b[i], v, __ATOMIC_SEQ_CST);
}
NI int bcas(int i, char e, char v) {
  return __atomic_compare_exchange_n(&b[i], &e, v, 0, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST);
}
NI int load(void) { return __atomic_load_n(&w, __ATOMIC_SEQ_CST); }
NI void store(int v) { __atomic_store_n(&w, v, __ATOMIC_SEQ_CST); }
NI int sync_add(int v) { return __sync_add_and_fetch(&w, v); }

NI unsigned cycles(void) {
  unsigned c;
  __asm__ volatile("mfsr %0, cycle" : "=r"(c));
  return c;
}
NI int asm_add(int a, int b) {
  int r;
  __asm__("add %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
  return r;
}
NI int asm_mem(int *p) {
  int r;
  __asm__("lw %0, %1" : "=r"(r) : "m"(*p));
  return r;
}
NI int asm_imm(void) {
  int r;
  __asm__("li %0, %1" : "=r"(r) : "i"(4321));
  return r;
}

int main(void) {
  store(10);
  CHECK_EQ(add(5), 10);
  CHECK_EQ(load(), 15);
  CHECK_EQ(sub(3), 15);
  CHECK_EQ(and_(0xe), 12);
  CHECK_EQ(or_(0x30), 12);
  CHECK_EQ(xor_(0xff), 0x3c);
  CHECK_EQ(load(), 0xc3);
  CHECK_EQ(nand(0xf), 0xc3);
  CHECK_EQ(load(), ~3);
  CHECK_EQ(xchg(7), ~3);
  CHECK_EQ(cas(6, 8), 0);
  CHECK_EQ(load(), 7);
  CHECK_EQ(cas(7, 8), 1);
  CHECK_EQ(load(), 8);
  CHECK_EQ(smin(-5), 8);
  CHECK_EQ(load(), -5);
  u = 5;
  CHECK_EQ(umax(0x80000000u), 5);
  CHECK_EQ(u, 0x80000000u);
  CHECK_EQ(sync_add(10), 5);
  h[1] = 100;
  CHECK_EQ(hadd(1, 23), 100);
  CHECK_EQ(h[1], 123);
  CHECK_EQ(h[0], 0);
  CHECK_EQ(h[2], 0);
  b[3] = 'x';
  CHECK_EQ(bxchg(3, 'y'), 'x');
  CHECK_EQ(b[3], 'y');
  CHECK_EQ(b[2], 0);
  CHECK_EQ(bcas(3, 'y', 'z'), 1);
  CHECK_EQ(bcas(3, 'y', 'q'), 0);
  CHECK_EQ(b[3], 'z');
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  unsigned c0 = cycles(), c1 = cycles();
  CHECK(c1 > c0);
  CHECK_EQ(asm_add(40, 2), 42);
  int m = 1234;
  CHECK_EQ(asm_mem(&m), 1234);
  CHECK_EQ(asm_imm(), 4321);
  puts("atomics ok");
  return 0;
}
