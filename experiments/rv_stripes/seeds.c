// Each pass runs K copies with different hash seeds; the one that went furthest is committed. Modelled on core 0 traces.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t a, sp, b, info, pc; } rec;
static uint32_t mult(int table, int seed) {
    uint32_t x = 0x9e3779b1u * (uint32_t)(table * 64 + seed + 1);
    x ^= x >> 15; x *= 0x85ebca6bu; x ^= x >> 13;
    return x | 1;
}
static uint32_t tab[8192];
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f) / sizeof(rec); fseek(f, 0, SEEK_SET);
    rec *R = malloc(n * sizeof(rec));
    if (fread(R, sizeof(rec), n, f) != n) return 1;
    fclose(f);
    int sil = atoi(argv[2]), budget = 8192;
    for (int a = 3; a < argc; a++) {
        int e, T, W, K;
        sscanf(argv[a], "%d,%d,%d,%d", &e, &T, &W, &K);
        int nb = e / (T * W);
        uint32_t m[8][64];
        for (int t = 0; t < T; t++) for (int s = 0; s < K; s++) m[t][s] = mult(t, s);
        size_t i = 0; uint32_t bs = R[0].b;
        uint64_t passes = 0, full = 0, usedsum = 0, bud = 0, real = 0;
        while (i < n) {
            size_t best = i; uint32_t bestb = bs; int bestwhy = -1, bestused = 0;
            for (int s = 0; s < K; s++) {
                memset(tab, 0, sizeof(uint32_t) * (size_t)(T * nb * W));
                int used = 0, why = -1; size_t j = i; uint32_t endb = bs;
                for (; j < n; j++) {
                    const rec *x = &R[j];
                    if (x->a == 0xffffffffu) {
                        int w = x->info & 0xff;
                        if (w != 0 && w != 3) { why = 1; endb = x->b; j++; break; }
                        continue;
                    }
                    if (x->b - bs >= (uint32_t)budget) { why = 0; endb = bs + budget; break; }
                    if ((x->info >> 24) & 1) continue;
                    if (sil && ((x->info >> 23) & 1)) continue;
                    uint32_t bytes = x->info & 0xff, t0 = x->a >> 4, t1 = (x->a + (bytes ? bytes : 1) - 1) >> 4;
                    int stop = 0;
                    for (uint32_t c = t0;; c = t1) {
                        int there = 0, free_t = -1, free_w = -1;
                        uint32_t bk[8];
                        for (int t = 0; t < T && !there; t++) {
                            bk[t] = (uint32_t)t * (uint32_t)nb + (((c * m[t][s]) >> 16) * (uint32_t)nb >> 16);
                            for (int w = 0; w < W; w++) {
                                uint32_t v = tab[bk[t] * W + w];
                                if (v == c + 1) { there = 1; break; }
                                if (!v && free_t < 0) { free_t = t; free_w = w; }
                            }
                        }
                        if (!there) {
                            if (free_t < 0) { stop = 1; break; }
                            tab[bk[free_t] * W + free_w] = c + 1; used++;
                        }
                        if (c == t1) break;
                    }
                    if (stop) { why = 2; endb = x->b ? x->b - 1 : 0; break; }
                }
                if (j >= n && why < 0) { why = 1; endb = R[n - 1].b; }
                if (j > best || s == 0) { if (j >= best) { best = j; bestb = endb; bestwhy = why; bestused = used; } }
            }
            passes++;
            if (bestwhy == 2) { full++; usedsum += bestused; } else if (bestwhy == 0) bud++; else real++;
            if (best == i && bestb == bs) { i++; continue; }
            i = best; bs = bestb;
        }
        printf("  %-16s b/p %5.0f  full %5.1f%%  used at full %3.0f%%\n", argv[a], (double)(R[n - 1].b - R[0].b) / passes, 100.0 * full / passes, full ? 100.0 * usedsum / full / (T * nb * W) : 0);
        fflush(stdout);
    }
    return 0;
}
