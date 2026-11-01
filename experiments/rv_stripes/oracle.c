// The ceiling of any policy for what all blocks keep: it knows the pass's future. A texel stored and read back later in the
// pass is kept by all from its first store to its last read; the rest of the time, and any texel never read back, costs
// only its stripe, here of unlimited size and with no filter's false ends. The pass ends where more than R are kept at once.
//   oracle TRACE sil R...     Modelled.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t a, sp, b, info, pc; } rec;
#define TAB (1u << 16)
static uint32_t key[TAB], ep[TAB], lastb[TAB], E = 1;
static int16_t occ[8200 + 8];
static inline int find(uint32_t t) { uint32_t i = (t * 0x9e3779b1u) >> 16; while (ep[i] == E && key[i] != t) i = (i + 1) & (TAB - 1); return (int)i; }
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f) / sizeof(rec); fseek(f, 0, SEEK_SET);
    rec *X = malloc(n * sizeof(rec)); if (fread(X, sizeof(rec), n, f) != n) return 1; fclose(f);
    int sil = atoi(argv[2]);
    for (int a = 3; a < argc; a++) {
        int R = atoi(argv[a]); uint64_t passes = 0, ebud = 0, enat = 0, eread = 0; uint32_t bs = X[0].b;
#define NEW(b) do { passes++; E++; bs = (b); memset(occ, 0, sizeof occ); } while (0)
        memset(occ, 0, sizeof occ);
        for (size_t k = 0; k < n; k++) {
            const rec *x = &X[k];
            if (x->a == 0xffffffffu) { int w = x->info & 0xff; if (w != 0 && w != 3) { enat++; NEW(x->b); } continue; }
            while (x->b - bs > 8192) { ebud++; NEW(bs + 8192); }
            uint32_t by = x->info & 0xff, t0 = x->a >> 4, t1 = (x->a + (by ? by : 1) - 1) >> 4;
            int load = (x->info >> 24) & 1, s = (x->info >> 23) & 1;
            for (uint32_t t = t0;; t = t1) {
                int i = find(t), there = ep[i] == E; uint32_t p = x->b - bs;
                if (load) { if (there) {
                    int over = 0;
                    for (uint32_t q = lastb[i] + 1; q <= p; q++) if (occ[q] >= R) { over = 1; break; }
                    if (over) { eread++; NEW(x->b - 1); k--; goto next; }
                    for (uint32_t q = lastb[i] + 1; q <= p; q++) occ[q]++;
                    lastb[i] = p;
                } } else if (!there && !(sil && s)) { ep[i] = E; key[i] = t; lastb[i] = p; if (occ[p] < 32000) occ[p]++; }
                if (t == t1) break;
            }
            next:;
        }
        uint64_t all = X[n - 1].b - X[0].b;
        printf("oracle sil=%d R=%-4d passes %8llu b/p %6.0f | budget %5.1f real %5.1f read-back %5.1f\n", sil, R, (unsigned long long)passes, (double)all / passes, 100.0 * ebud / passes, 100.0 * enat / passes, 100.0 * eread / passes);
        fflush(stdout);
    }
    return 0;
}
