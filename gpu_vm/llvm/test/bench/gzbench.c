/* gzip and gunzip of stdin in memory with zlib (as minigzip: level 6, the gzip wrapper, its crc32), on gvm_cpu:
   the bundles each takes (CYCLE), to measure the code generator on real code (gpu_vm/bench_real's input). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"

static unsigned cycles(void) {
  unsigned c;
  __asm__ volatile("mfsr %0, cycle" : "=r"(c));
  return c;
}

int main(void) {
  size_t cap = 1 << 20, n = 0, r;
  unsigned char *in = malloc(cap);
  while ((r = fread(in + n, 1, cap - n, stdin)) > 0) n += r;
  size_t zcap = n + n / 8 + 1024;
  unsigned char *z = malloc(zcap), *back = malloc(n + 1);

  unsigned t0 = cycles();
  z_stream s;
  memset(&s, 0, sizeof s);
  if (deflateInit2(&s, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return 1;
  s.next_in = in; s.avail_in = (uInt)n; s.next_out = z; s.avail_out = (uInt)zcap;
  if (deflate(&s, Z_FINISH) != Z_STREAM_END) return 2;
  size_t zn = s.total_out;
  deflateEnd(&s);
  unsigned t1 = cycles();

  memset(&s, 0, sizeof s);
  if (inflateInit2(&s, 15 + 16) != Z_OK) return 3;
  s.next_in = z; s.avail_in = (uInt)zn; s.next_out = back; s.avail_out = (uInt)n + 1;
  if (inflate(&s, Z_FINISH) != Z_STREAM_END) return 4;
  size_t bn = s.total_out;
  inflateEnd(&s);
  unsigned t2 = cycles();

  int ok = bn == n && memcmp(in, back, n) == 0;
  printf("input %u bytes, gzip %u bytes, back %s\n", (unsigned)n, (unsigned)zn, ok ? "ok" : "WRONG");
  printf("gzip %u bundles, gunzip %u bundles\n", t1 - t0, t2 - t1);
  return ok ? 0 : 5;
}
