/* gzip or gunzip (zlib, as gpu_vm/llvm/test/bench/gzbench.c: level 6, the gzip wrapper) over an input linked into the
   program, round after round: real code for the tick's bench (gvmbench's cases gzip and gunzip, cases.h). No call to
   the host until the rounds are done: the bench patches gz_mode, gz_bytes and gz_rounds in the image. */
#include <stdlib.h>
#include <string.h>
#include "zlib.h"

extern const unsigned char gz_input[], gz_input_end[], gz_packed[], gz_packed_end[];
__asm__(".section .rodata\n.p2align 4\n.globl gz_input\ngz_input:\n.incbin \"" GZ_INPUT "\"\n.globl gz_input_end\ngz_input_end:\n"
        ".p2align 4\n.globl gz_packed\ngz_packed:\n.incbin \"" GZ_PACKED "\"\n.globl gz_packed_end\ngz_packed_end:\n.text\n");

volatile unsigned gz_mode = 1;        /* 1 gzip, 2 gunzip */
volatile unsigned gz_bytes = 262144;  /* gzip: the input's first bytes */
volatile unsigned gz_rounds = 1;
volatile unsigned gz_sum;

static unsigned char out[600000];
static unsigned char state[1 << 20];  /* zlib's allocations, made again each round */
static unsigned used;
static voidpf zalloc(voidpf o, uInt n, uInt m) { (void)o; void *p = state + used; used += (n * m + 15) & ~15u; return used <= sizeof state ? p : 0; }
static void zfree(voidpf o, voidpf p) { (void)o; (void)p; }

int main(void) {
  unsigned sum = 0;
  for (unsigned r = 0; r < gz_rounds; r++) {
    z_stream s;
    memset(&s, 0, sizeof s);
    s.zalloc = zalloc; s.zfree = zfree; used = 0;
    if (gz_mode == 1) {
      if (deflateInit2(&s, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return 1;
      s.next_in = (unsigned char *)gz_input; s.avail_in = gz_bytes; s.next_out = out; s.avail_out = sizeof out;
      if (deflate(&s, Z_FINISH) != Z_STREAM_END) return 2;
    } else {
      if (inflateInit2(&s, 15 + 16) != Z_OK) return 3;
      s.next_in = (unsigned char *)gz_packed; s.avail_in = (unsigned)(gz_packed_end - gz_packed); s.next_out = out; s.avail_out = sizeof out;
      int e = inflate(&s, Z_FINISH);
      if (e != Z_STREAM_END && !(e == Z_BUF_ERROR && s.avail_out == 0)) return 4;
    }
    sum = sum * 31 + (unsigned)s.total_out + out[s.total_out / 2];
  }
  gz_sum = sum;
  return 0;
}
