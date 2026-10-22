// The log tables of gpu_vm's libm (gvm_libm.h, "log"): gcc -O2 gen_logtab.c -lm && ./a.out > __gvm_log_data.c
// Interval i of z (bits 0x3f330000 + i 2^17 to + 2^17, as the top six bits of ix - 0x3f330000 choose it): invc is 1/z
// at the interval's middle rounded to 8 significant bits (1 for the interval round 1); hi + lo is -log(invc) (and
// -log2(invc)), hi rounded to a multiple of 2^-16.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef long double R;
static float bf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

int main(void) {
  float invc[64];
  for (int i = 0; i < 64; i++) {
    uint32_t lo = 0x3f330000u + ((uint32_t)i << 17), hi = lo + (1u << 17);
    R mid = ((R)bf(lo) + (R)bf(hi)) / 2, ic = 1 / mid;
    int e;
    frexpl(ic, &e);
    R s = ldexpl(1, 8 - e);
    invc[i] = (float)(roundl(ic * s) / s);
    if (bf(lo) <= 1.0f && bf(hi) > 1.0f) invc[i] = 1.0f;
  }
  printf("/* The log tables of gpu_vm's libm (gvm_libm.h, \"log\"), written by gpu_vm/llvm/test/libm/gen_logtab.c:\n"
         " * {invc, hi, lo}: invc 1/z at the interval's middle to 8 bits, hi + lo = -log(invc) (-log2 in the second),\n"
         " * hi a multiple of 2^-16. */\n#include \"gvm_libm.h\"\n\n");
  for (int t = 0; t < 2; t++) {
    printf("const struct gvm_log_entry %s[64] __attribute__((aligned(16))) = {\n", t ? "__gvm_log2_tab" : "__gvm_log_tab");
    for (int i = 0; i < 64; i++) {
      R l = -logl((R)invc[i]);
      if (t) l /= logl(2);
      R h = roundl(l * 65536) / 65536;
      printf("\t{%a, %a, %a, 0},\n", invc[i], (float)h, (float)(l - h));
    }
    printf("};\n%s", t ? "" : "\n");
  }
  return 0;
}
