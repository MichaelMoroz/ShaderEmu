// Frames: variable-length arrays and alloca (fp), over-aligned locals (a
// realigned sp, and bp with both), a large frame, many live values across a
// call (the callee-saved registers), deep recursion, the frame address.
#include "gvm.h"

#define NI __attribute__((noinline))

NI int vla(int n) {
  int a[n];
  for (int i = 0; i < n; i++)
    a[i] = i * 2;
  int s = 0;
  for (int i = 0; i < n; i++)
    s += a[i];
  return s;
}

NI int use_alloca(int n) {
  char *p = __builtin_alloca(n);
  memset(p, 1, n);
  int s = 0;
  for (int i = 0; i < n; i++)
    s += p[i];
  return s;
}

NI int aligned_local(void) {
  int x __attribute__((aligned(64))) = 5;
  volatile int *px = &x;
  CHECK_EQ((unsigned)px & 63, 0);
  return *px;
}

NI int aligned_vla(int n) {
  int x __attribute__((aligned(128))) = n;
  int a[n];
  for (int i = 0; i < n; i++)
    a[i] = x + i;
  volatile int *px = &x;
  CHECK_EQ((unsigned)px & 127, 0);
  return a[n - 1] + *px;
}

NI int big_frame(int k) {
  volatile int a[20000];
  for (int i = 0; i < 20000; i += 97)
    a[i] = i + k;
  int s = 0;
  for (int i = 0; i < 20000; i += 97)
    s += a[i];
  return s;
}

NI int ident(int x) { return x; }

NI int many_live(int seed) {
  int v[40];
  for (int i = 0; i < 40; i++)
    v[i] = seed * (i + 1);
  int a0 = v[0], a1 = v[1], a2 = v[2], a3 = v[3], a4 = v[4], a5 = v[5],
      a6 = v[6], a7 = v[7], a8 = v[8], a9 = v[9], a10 = v[10], a11 = v[11],
      a12 = v[12], a13 = v[13], a14 = v[14], a15 = v[15], a16 = v[16],
      a17 = v[17], a18 = v[18], a19 = v[19], a20 = v[20], a21 = v[21],
      a22 = v[22], a23 = v[23], a24 = v[24], a25 = v[25], a26 = v[26],
      a27 = v[27], a28 = v[28], a29 = v[29], a30 = v[30];
  int r = ident(seed);
  return r + a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 +
         a12 + a13 + a14 + a15 + a16 + a17 + a18 + a19 + a20 + a21 + a22 +
         a23 + a24 + a25 + a26 + a27 + a28 + a29 + a30;
}

NI int depth(int n) {
  volatile int pad[16];
  pad[0] = n;
  if (n == 0)
    return 0;
  return depth(n - 1) + pad[0] - n + 1;
}

NI void *frame(void) { return __builtin_frame_address(0); }

int main(void) {
  CHECK_EQ(vla(10), 90);
  CHECK_EQ(vla(1000), 999000);
  CHECK_EQ(use_alloca(300), 300);
  CHECK_EQ(aligned_local(), 5);
  CHECK_EQ(aligned_vla(10), 29);
  int ref = 0;
  for (int i = 0; i < 20000; i += 97)
    ref += i + 3;
  CHECK_EQ(big_frame(3), ref);
  CHECK_EQ(many_live(3), 3 + 3 * 31 * 32 / 2);
  CHECK_EQ(depth(5000), 5000);
  CHECK(frame() != 0);
  puts("frame ok");
  return 0;
}
