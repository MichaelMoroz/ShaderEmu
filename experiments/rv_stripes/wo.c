// A write-only part of the cache, chosen by the store's pc (what a compiler's hint could say at best: trained on this same trace).
// Kept part: K entries, ideal placement, read back freely. Write-only part: W entries, ideal placement; a load of a texel there ends the pass.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t a, sp, b, info, pc; } rec;
#define TAB (1u << 18)
static uint32_t key[TAB], ep[TAB]; static uint8_t kind[TAB], rb[TAB]; static uint32_t spc[TAB]; static uint32_t epoch = 1;
static int find(uint32_t t) { uint32_t i = (t * 0x9e3779b1u) >> 14; while (ep[i] == epoch && key[i] != t) i = (i + 1) & (TAB - 1); return (int)i; }
#define PCS (1u << 22)
static uint32_t pkey[PCS], pst[PCS], prb[PCS];
static int pfind(uint32_t pc) { uint32_t i = (pc * 0x9e3779b1u) >> 10; while (pkey[i] && pkey[i] != pc + 1) i = (i + 1) & (PCS - 1); pkey[i] = pc + 1; return (int)i; }
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f) / sizeof(rec); fseek(f, 0, SEEK_SET);
    rec *R = malloc(n * sizeof(rec));
    if (fread(R, sizeof(rec), n, f) != n) return 1;
    fclose(f);
    int budget = 8192;
    // training: passes of the whole budget; a store pc that makes a texel new: was it read back in the pass?
    uint32_t bs = R[0].b; static uint32_t list[1 << 20]; int nl = 0; uint64_t tot = 0, totrb = 0;
#define FLUSH do { for (int q = 0; q < nl; q++) { int i = (int)list[q]; int p = pfind(spc[i]); pst[p]++; prb[p] += rb[i]; tot++; totrb += rb[i]; } nl = 0; epoch++; } while (0)
    for (size_t k = 0; k < n; k++) {
        const rec *x = &R[k];
        if (x->a == 0xffffffffu) { int w = x->info & 0xff; if (w != 0 && w != 3) { FLUSH; bs = x->b; } continue; }
        while (x->b - bs >= (uint32_t)budget) { FLUSH; bs += budget; }
        uint32_t bytes = x->info & 0xff, t0 = x->a >> 4, t1 = (x->a + (bytes ? bytes : 1) - 1) >> 4; int load = (x->info >> 24) & 1;
        for (uint32_t t = t0;; t = t1) {
            int i = find(t), there = ep[i] == epoch;
            if (load) { if (there) rb[i] = 1; }
            else if (!there) { ep[i] = epoch; key[i] = t; rb[i] = 0; spc[i] = x->pc; list[nl++] = (uint32_t)i; }
            if (t == t1) break;
        }
    }
    FLUSH;
    printf("  read back %.1f%% of texels\n", 100.0 * totrb / tot);
    for (int a = 2; a < argc; a++) {
        int K, W, th; sscanf(argv[a], "%d,%d,%d", &K, &W, &th);
        uint64_t passes = 0, efull = 0, eread = 0, ebud = 0, ereal = 0; int nk = 0, nw = 0; bs = R[0].b; epoch++;
        for (size_t k = 0; k < n; k++) {
            const rec *x = &R[k];
            if (x->a == 0xffffffffu) { int w = x->info & 0xff; if (w != 0 && w != 3) { passes++; ereal++; bs = x->b; nk = nw = 0; epoch++; } continue; }
            while (x->b - bs >= (uint32_t)budget) { passes++; ebud++; bs += budget; nk = nw = 0; epoch++; }
            uint32_t bytes = x->info & 0xff, t0 = x->a >> 4, t1 = (x->a + (bytes ? bytes : 1) - 1) >> 4; int load = (x->info >> 24) & 1;
            for (uint32_t t = t0;; t = t1) {
                int i = find(t), there = ep[i] == epoch;
                if (load) {
                    if (there && kind[i] == 1) { passes++; eread++; bs = x->b ? x->b - 1 : 0; nk = nw = 0; epoch++; }
                } else if (!there) {
                    int p = pfind(x->pc); int wo = K == 0 || (W > 0 && prb[p] * 100 <= pst[p] * (uint32_t)th);
                    if (!wo && nk >= K) wo = W > 0 ? 1 : 2;
                    if (wo == 1 && nw >= W) { if (nk < K) wo = 0; else wo = 2; }
                    if (wo == 2) { passes++; efull++; bs = x->b ? x->b - 1 : 0; nk = nw = 0; epoch++; i = find(t); wo = 0; p = pfind(x->pc); wo = K == 0 || (W > 0 && prb[p] * 100 <= pst[p] * (uint32_t)th); }
                    ep[i] = epoch; key[i] = t; kind[i] = (uint8_t)wo; if (wo) nw++; else nk++;
                }
                if (t == t1) break;
            }
        }
        double all = (double)(R[n - 1].b - R[0].b);
        printf("  kept %3d, write-only %4d, pc under %2d%%: b/p %5.0f | budget %5.1f real %5.1f full %5.1f readback %5.1f\n", K, W, th, all / passes, 100.0 * ebud / passes, 100.0 * ereal / passes, 100.0 * efull / passes, 100.0 * eread / passes);
        fflush(stdout);
    }
    return 0;
}
