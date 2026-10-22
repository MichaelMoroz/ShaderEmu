// memcpy, memmove, memset of known sizes: inline (words, texels by lq/sq),
// libc, and from 4 KB up the commit's operation; the builtins that always
// make the operation; struct copies.
#include "gvm.h"

#define NI __attribute__((noinline))

static unsigned char src[20000] __attribute__((aligned(16)));
static unsigned char dst[20000] __attribute__((aligned(16)));

static void fill_src(void) {
  for (int i = 0; i < (int)sizeof src; i++)
    src[i] = (unsigned char)(i * 7 + 3);
}

static void check_copy(const unsigned char *d, const unsigned char *s, int n,
                       int line) {
  for (int i = 0; i < n; i++)
    if (d[i] != s[i]) {
      printf("line %d: byte %d is %d, want %d\n", line, i, d[i], s[i]);
      exit(1);
    }
}
#define CHECK_COPY(d, s, n) check_copy(d, s, n, __LINE__)

struct s48 {
  int v[12];
} __attribute__((aligned(16)));
struct s20 {
  char c[20];
};

NI void cp16(void *d, const void *s) { __builtin_memcpy(d, s, 16); }
NI void cp7(void *d, const void *s) { __builtin_memcpy(d, s, 7); }
NI void cp64a(struct s48 *d, const struct s48 *s) { *d = *s; }
NI void cp20(struct s20 *d, const struct s20 *s) { *d = *s; }
NI void cp100(void *d, const void *s) { __builtin_memcpy(d, s, 100); }
NI void cp4096(void *d, const void *s) { __builtin_memcpy(d, s, 4096); }
NI void cp10000(void *d, const void *s) { __builtin_memcpy(d, s, 10000); }
NI void mv5000(void *d, const void *s) { __builtin_memmove(d, s, 5000); }
NI void mv100(void *d, const void *s) { __builtin_memmove(d, s, 100); }
NI void set40(void *d, int c) { __builtin_memset(d, c, 40); }
NI void set64a(struct s48 *d) { __builtin_memset(d, 0, sizeof *d); }
NI void set8000(void *d, int c) { __builtin_memset(d, c, 8000); }
NI void cpn(void *d, const void *s, unsigned n) { __builtin_memcpy(d, s, n); }

int main(void) {
  fill_src();
  cp16(dst, src);
  CHECK_COPY(dst, src, 16);
  cp7(dst + 1, src + 3);
  CHECK_COPY(dst + 1, src + 3, 7);
  struct s48 a, b;
  for (int i = 0; i < 12; i++)
    a.v[i] = i * i;
  cp64a(&b, &a);
  CHECK(memcmp(&a, &b, sizeof a) == 0);
  struct s20 c, d;
  memcpy(&c, src, sizeof c);
  cp20(&d, &c);
  CHECK(memcmp(&c, &d, sizeof c) == 0);
  cp100(dst + 3, src + 5);
  CHECK_COPY(dst + 3, src + 5, 100);

  // The commit's operations: the core goes on, and a load of what one writes
  // waits for it.
  memset(dst, 0, sizeof dst);
  cp4096(dst, src);
  CHECK_COPY(dst, src, 4096);
  CHECK_EQ(dst[4096], 0);
  cp10000(dst + 3, src + 1);
  CHECK_COPY(dst + 3, src + 1, 10000);
  // memmove overlapping, both ways
  memcpy(dst, src, 6000);
  mv5000(dst + 100, dst);
  CHECK_COPY(dst + 100, src, 5000);
  memcpy(dst, src, 6000);
  mv5000(dst, dst + 100);
  CHECK_COPY(dst, src + 100, 5000);
  memcpy(dst, src, 300);
  mv100(dst + 10, dst);
  CHECK_COPY(dst + 10, src, 100);

  set40(dst + 1, 0xab);
  for (int i = 0; i < 40; i++)
    CHECK_EQ(dst[1 + i], 0xab);
  set64a(&a);
  for (int i = 0; i < 12; i++)
    CHECK_EQ(a.v[i], 0);
  set8000(dst + 2, 0x5c);
  for (int i = 0; i < 8000; i++)
    CHECK_EQ(dst[2 + i], 0x5c);
  CHECK_EQ(dst[8002], src[8000]);

  // The builtins: always the operation, any size
  __builtin_gpuvm_copy(dst, src + 7, 33);
  CHECK_COPY(dst, src + 7, 33);
  __builtin_gpuvm_fill(dst + 1, 0x04030201u, 9);
  for (int i = 0; i < 9; i++)
    CHECK_EQ(dst[1 + i], (i & 3) + 1);
  // and a store into a region still being copied waits too
  __builtin_gpuvm_copy(dst, src, 16000);
  dst[100] = 7;
  CHECK_EQ(dst[100], 7);
  CHECK_EQ(dst[99], src[99]);
  CHECK_EQ(dst[15999], src[15999]);

  // unknown sizes: libc
  for (unsigned n = 0; n < 40; n++) {
    cpn(dst + 1, src + 2, n);
    CHECK_COPY(dst + 1, src + 2, n);
  }
  cpn(dst, src, 12345);
  CHECK_COPY(dst, src, 12345);
  puts("mem ok");
  return 0;
}
