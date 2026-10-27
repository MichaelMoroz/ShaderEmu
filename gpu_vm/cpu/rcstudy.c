/* rcstudy LOG [WORDS]: replays a gvm_cpu GVM_STLOG (GVM_STLOG_CORE=-1, GVM_STLOG_LOADS=1; WORDS 5, or 7 with
 * GVM_STLOG_VALUES) for a write cache whose data lies with the lane that writes its texel out, and whose lanes all keep
 * only the tags and a small cache of the texels stored last (GPU-VM's plan, "own data"). A load of a word the core stored
 * earlier in the pass must find its texel in that small cache, or the pass ends before it. For each size K of the small
 * cache, kept by the order of stores (a), or of stores and the loads it answered (b), it counts such loads and the passes
 * they would end, for core 0 and the workers apart.
 * A pass: each core's records end with a record of address 0xffffffff; cores run in order. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXC 128
#define NONE 0xffffffffu
#define SETN 4096
enum { NK = 6 };
static const int KS[NK] = {2, 4, 8, 16, 32, 64};

typedef struct {
    uint32_t set[SETN];         // this pass's stored texels (open addressing), their word masks
    uint8_t mask[SETN];
    uint32_t mru[2][64];        // the small caches, most recent first: (a) by stores, (b) by stores and answered loads
    int ended[2][NK];           // this pass already ended there (the first such load), for each K
    uint64_t loads_stored;      // loads of a word stored earlier in the pass, this pass
} core_t;
typedef struct { uint64_t passes, loads, loads_stored, miss[2][NK], ends[2][NK]; } gstat;
static core_t C[MAXC];
static gstat G[2];

static uint32_t *find(core_t *c, uint32_t t, int make) {
    uint32_t h = (t * 0x9e3779b1u) >> 20;
    for (int i = 0; i < SETN; i++, h = (h + 1) & (SETN - 1)) {
        if (c->set[h] == t) return &c->set[h];
        if (c->set[h] == NONE) { if (make) { c->set[h] = t; return &c->set[h]; } return NULL; }
    }
    return NULL;
}
static void touch(uint32_t *m, uint32_t t) {   // t to the front
    int i = 0;
    while (i < 63 && m[i] != t && m[i] != NONE) i++;
    memmove(m + 1, m, (size_t)i * 4);
    m[0] = t;
}
static int rank(const uint32_t *m, uint32_t t) { for (int i = 0; i < 64 && m[i] != NONE; i++) if (m[i] == t) return i; return 64; }

static void pass_end(int k) {
    core_t *c = &C[k];
    gstat *g = &G[k != 0];
    g->passes++;
    for (int p = 0; p < 2; p++) for (int j = 0; j < NK; j++) g->ends[p][j] += c->ended[p][j];
    memset(c->set, 0xff, sizeof c->set);
    memset(c->mru, 0xff, sizeof c->mru);
    memset(c->ended, 0, sizeof c->ended);
}
static void access(int k, uint32_t a, uint32_t bytes, int store) {
    core_t *c = &C[k];
    gstat *g = &G[k != 0];
    uint32_t t = a >> 4, w = 0;
    for (uint32_t b = a & 15; b < (a & 15) + (bytes ? bytes : 4) && b < 16; b++) w |= 1u << (b >> 2);
    if (store) {
        uint32_t *e = find(c, t, 1);
        if (e) c->mask[e - c->set] |= (uint8_t)w;
        touch(c->mru[0], t); touch(c->mru[1], t);
        return;
    }
    g->loads++;
    uint32_t *e = find(c, t, 0);
    if (!e || !(c->mask[e - c->set] & w)) return;   // (a word not stored this pass: RAM answers it)
    g->loads_stored++;
    for (int p = 0; p < 2; p++) {
        int r = rank(c->mru[p], t);
        for (int j = 0; j < NK; j++)
            if (r >= KS[j]) { g->miss[p][j]++; c->ended[p][j] = 1; }
        if (p == 1 && r < 64) touch(c->mru[1], t);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "rcstudy LOG [WORDS(5|7)]\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    int nw = argc > 2 ? atoi(argv[2]) : 5;
    if (!f) { perror(argv[1]); return 1; }
    for (int i = 0; i < MAXC; i++) { memset(C[i].set, 0xff, sizeof C[i].set); memset(C[i].mru, 0xff, sizeof C[i].mru); }
    uint32_t r[7];
    uint64_t recs = 0;
    while (fread(r, 4, nw, f) == (size_t)nw) {
        recs++;
        int core = (int)((r[3] >> 25) & 127);
        if (r[0] == NONE) { pass_end(core); continue; }
        access(core, r[0], r[3] & 255, !((r[3] >> 24) & 1));
    }
    printf("log %s: %llu records\n", argv[1], (unsigned long long)recs);
    for (int k = 0; k < 2; k++) {
        gstat *g = &G[k];
        if (!g->passes) continue;
        printf("\n%s: %llu passes, %llu loads, %llu of a word stored earlier in the pass (%.1f%%)\n", k ? "workers" : "core 0", (unsigned long long)g->passes,
               (unsigned long long)g->loads, (unsigned long long)g->loads_stored, 100.0 * (double)g->loads_stored / (double)(g->loads ? g->loads : 1));
        printf("| last texels kept | (a) by stores: loads not found, %% of those | passes ended, %% | (b) by stores and loads: loads not found, %% | passes ended, %% |\n|---|---|---|---|---|\n");
        for (int j = 0; j < NK; j++)
            printf("| %d | %.2f | %.2f | %.2f | %.2f |\n", KS[j], 100.0 * (double)g->miss[0][j] / (double)(g->loads_stored ? g->loads_stored : 1), 100.0 * (double)g->ends[0][j] / (double)g->passes,
                   100.0 * (double)g->miss[1][j] / (double)(g->loads_stored ? g->loads_stored : 1), 100.0 * (double)g->ends[1][j] / (double)g->passes);
    }
    return 0;
}
