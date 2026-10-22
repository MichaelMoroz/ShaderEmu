// zlib on gpu_vm: deflate and inflate of text and of pseudo-random data at every level, checked byte for byte
// and by crc32 and adler32 against values computed another way.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"

static unsigned crc_ref(const unsigned char *p, unsigned n) {
  unsigned c = 0xffffffffu;
  for (unsigned i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++)
      c = (c >> 1) ^ (0xedb88320u & -(c & 1));
  }
  return ~c;
}

static unsigned adler_ref(const unsigned char *p, unsigned n) {
  unsigned a = 1, b = 0;
  for (unsigned i = 0; i < n; i++) {
    a = (a + p[i]) % 65521;
    b = (b + a) % 65521;
  }
  return b << 16 | a;
}

int main(void) {
  enum { N = 60000 };
  unsigned char *src = malloc(N), *cmp = malloc(N + N / 8 + 64), *out = malloc(N);
  const char *words[] = {"the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog ", "gpu_vm ",
                         "bundle ", "\n"};
  unsigned h = 12345;
  for (int kind = 0; kind < 2; kind++) {
    unsigned n = 0;
    while (n < N) {
      h = h * 1103515245u + 12345u;
      if (kind == 0) {
        const char *w = words[(h >> 16) % 11];
        while (*w && n < N)
          src[n++] = *w++;
      } else {
        src[n++] = (unsigned char)(h >> 24) & ((h >> 8) & 1 ? 0xff : 0x0f);
      }
    }
    if (crc32(0, src, N) != crc_ref(src, N) || adler32(1, src, N) != adler_ref(src, N)) {
      printf("checksums differ\n");
      return 1;
    }
    for (int level = 0; level <= 9; level++) {
      uLongf clen = N + N / 8 + 64, olen = N;
      int r = compress2(cmp, &clen, src, N, level);
      if (r != Z_OK) {
        printf("compress2 level %d: %d\n", level, r);
        return 1;
      }
      r = uncompress(out, &olen, cmp, clen);
      if (r != Z_OK || olen != N || memcmp(out, src, N)) {
        printf("uncompress level %d: %d, %lu bytes\n", level, r, (unsigned long)olen);
        return 1;
      }
      printf("%s level %d: %d to %lu\n", kind ? "random" : "text", level, N, (unsigned long)clen);
    }
  }
  puts("zlib ok");
  return 0;
}
