// Of the distinct texels stored in a window of a pass: stored only, or read back after their first store in the window.
// A load before the first store is no reuse. Stack: the first store within [sp - 256, sp + 4096). Windows are cut at a real end
// (sync, wait, halt) and every W bundles.   storedonly TRACE W sil
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct { uint32_t a, sp, b, info, pc; } rec;
#define TAB (1u << 16)
static uint32_t key[TAB], ep[TAB], ord_[TAB], E = 1; static uint8_t rb[TAB], stk[TAB];
static inline int find(uint32_t t) { uint32_t i = (t * 0x9e3779b1u) >> 16; while (ep[i] == E && key[i] != t) i = (i + 1) & (TAB - 1); return (int)i; }
int main(int c, char **v) {
    FILE *f = fopen(v[1], "rb"); uint32_t W = atoi(v[2]); int sil = atoi(v[3]); rec x;
    uint64_t win = 0, n[2] = {0, 0}, r[2] = {0, 0}, hit = 0, before = 0, past = 0, sum = 0; uint32_t bs = 0, nst = 0; int first = 1, seen = 0;
#define FL do { win++; sum += nst; if (!seen) past += nst; E++; nst = 0; seen = 0; } while (0)
    while (fread(&x, sizeof x, 1, f) == 1) {
        if (first) { bs = x.b; first = 0; }
        if (x.a == 0xffffffffu) { int w = x.info & 0xff; if (w != 0 && w != 3) { FL; bs = x.b; } continue; }
        while (x.b - bs > W) { FL; bs += W; }
        uint32_t by = x.info & 0xff, t0 = x.a >> 4, t1 = (x.a + (by ? by : 1) - 1) >> 4; int load = (x.info >> 24) & 1, s = (x.info >> 23) & 1;
        for (uint32_t t = t0;; t = t1) {
            int i = find(t), there = ep[i] == E;
            if (load) { if (there) {
                if (!rb[i]) { rb[i] = 1; r[stk[i]]++; }
                if (ord_[i] >= 225 && !seen) { seen = 1; hit++; before += nst; past += nst; }
            } } else if (!there && !(sil && s)) {
                ep[i] = E; key[i] = t; ord_[i] = nst++; rb[i] = 0; stk[i] = x.a - x.sp + 256u < 4352u; n[stk[i]]++;
            }
            if (t == t1) break;
        }
    }
    FL;
    double N = n[0] + n[1];
    printf("%-14s W=%-5u sil=%d windows %7llu | texels/window %6.1f (stack %5.1f, rest %6.1f) | stored only %5.1f%% read back %5.1f%% | stack: only %5.1f%% back %5.1f%% | rest: only %5.1f%% back %5.1f%% | stack share %4.1f%% | read-back past 225 in %5.1f%% of windows, %5.0f distinct before it | distinct before it, all windows %6.1f of %6.1f\n",
        v[1], W, sil, (unsigned long long)win, N / win, (double)n[1] / win, (double)n[0] / win, 100 * (N - r[0] - r[1]) / N, 100 * (r[0] + r[1]) / N,
        n[1] ? 100.0 * (n[1] - r[1]) / n[1] : 0, n[1] ? 100.0 * r[1] / n[1] : 0, n[0] ? 100.0 * (n[0] - r[0]) / n[0] : 0, n[0] ? 100.0 * r[0] / n[0] : 0, 100 * n[1] / N,
        100.0 * hit / win, hit ? (double)before / hit : 0, (double)past / win, (double)sum / win);
    return 0;
}
