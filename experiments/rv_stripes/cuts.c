// Passes as subgraphs, instructions in trace order: a stored texel read later in the pass lives in a register from its store to its
// last read in the pass (at most R at once); every stored texel of a pass is an output (at most W). Perfect knowledge of aliasing.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t a, sp, b, info, pc; } rec;
#define TAB (1u << 20)
static uint32_t lk[TAB], lv[TAB]; // last seen load, for the backward scan
static uint32_t key[TAB], ep[TAB]; static uint8_t live[TAB]; static uint32_t epoch = 1;
static int find(uint32_t *K, uint32_t *E, uint32_t e, uint32_t t) { uint32_t i = (t * 0x9e3779b1u) >> 12; while (E[i] == e && K[i] != t) i = (i + 1) & (TAB - 1); return (int)i; }
static uint32_t E2[TAB];
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f) / sizeof(rec); fseek(f, 0, SEEK_SET);
    rec *R = malloc(n * sizeof(rec));
    if (fread(R, sizeof(rec), n, f) != n) return 1;
    fclose(f);
    uint32_t *nx = malloc(n * 4);   // the bundle of the next load of this record's texel, 0xffffffff none
    for (size_t k = n; k-- > 0;) {
        const rec *x = &R[k]; nx[k] = 0xffffffffu;
        if (x->a == 0xffffffffu) continue;
        uint32_t t = x->a >> 4; int i = find(lk, E2, 1, t);
        if (E2[i] == 1) nx[k] = lv[i];
        if ((x->info >> 24) & 1) { E2[i] = 1; lk[i] = t; lv[i] = x->b; }
    }
    int budget = 8192;
    for (int a = 2; a < argc; a++) {
        int Rg, W; sscanf(argv[a], "%d,%d", &Rg, &W);
        uint64_t passes = 0, efull = 0, ereg = 0, ebud = 0, ereal = 0, peak = 0, outs = 0; uint32_t bs = R[0].b; int nl = 0, nw = 0; epoch++;
#define NEWP do { passes++; outs += nw; nl = nw = 0; epoch++; } while (0)
        for (size_t k = 0; k < n; k++) {
            const rec *x = &R[k];
            if (x->a == 0xffffffffu) { int w = x->info & 0xff; if (w != 0 && w != 3) { ereal++; NEWP; bs = x->b; } continue; }
            while (x->b - bs >= (uint32_t)budget) { ebud++; NEWP; bs += budget; }
            uint32_t t = x->a >> 4; int load = (x->info >> 24) & 1;
            uint32_t pend = bs + budget;
            int i = find(key, ep, epoch, t), there = ep[i] == epoch;
            if (load) {
                if (there && live[i] && (nx[k] == 0xffffffffu || nx[k] >= pend)) { live[i] = 0; nl--; }
            } else {
                int willread = nx[k] != 0xffffffffu && nx[k] < pend;
                int addw = !there, addl = willread && !(there && live[i]);
                if (nw + addw > W) { efull++; NEWP; bs = x->b ? x->b - 1 : 0; pend = bs + budget; i = find(key, ep, epoch, t); there = 0; addw = 1; willread = nx[k] != 0xffffffffu && nx[k] < pend; addl = willread; }
                else if (nl + addl > Rg) { ereg++; NEWP; bs = x->b ? x->b - 1 : 0; pend = bs + budget; i = find(key, ep, epoch, t); there = 0; addw = 1; willread = nx[k] != 0xffffffffu && nx[k] < pend; addl = willread; }
                if (!there) { ep[i] = epoch; key[i] = t; live[i] = 0; }
                nw += addw;
                if (addl) { live[i] = 1; nl++; }
                else if (!willread && live[i]) { live[i] = 0; nl--; }
            }
        }
        printf("  registers %3d, outputs %4d: b/p %5.0f | budget %5.1f real %5.1f out of outputs %5.1f out of registers %5.1f | outputs a pass %.0f\n", Rg, W, (double)(R[n - 1].b - R[0].b) / passes, 100.0 * ebud / passes, 100.0 * ereal / passes, 100.0 * efull / passes, 100.0 * ereg / passes, (double)outs / passes);
        fflush(stdout);
    }
    return 0;
}
