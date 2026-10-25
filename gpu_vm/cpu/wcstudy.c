/* wcstudy LOG WORDS [ONLY_USER]: replays a gvm_cpu GVM_STLOG (GVM_STLOG_CORE=-1, GVM_STLOG_LOADS=1; WORDS 5, or 7 with
 * GVM_STLOG_VALUES) and measures, per pass and core, for core 0 and the workers apart:
 *  A. loads: how many need the write cache's lookup under today's OR word, a hashed bitmap of 128-1024 bits (one or two
 *     hashes), and exactly (the texel was stored by this core earlier in the pass);
 *  B. the cache's lookups: today's two buckets always read, against one array probed bucket by bucket (next bucket or a
 *     rehash), stopping at the first empty way: buckets read a store and a load, and passes that would end full;
 *  C. texels two cores store in one pass: in the same word (a race today too) or only in different words (new with
 *     whole-texel commits), with the stores' pcs.
 * A pass: each core's records end with a record of address 0xffffffff; cores run in order, so a record of a core at or
 * below the last one that ended starts the next pass. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXC 128
#define NONE 0xffffffffu
enum { NBM = 5 };
static const int BM_BITS[NBM] = {128, 256, 512, 1024, 256};   // the last one with two hashes
static const int BM_K[NBM] = {1, 1, 1, 1, 2};

// ---- designs: kind 0 today's two-choice (both buckets read), 1 linear by bucket, 2 rehash; nb buckets of 4; probe limit
typedef struct { const char *name; int kind, nb, limit; } design;
static const design DS[] = {
    {"today 2x32x4 (both read)", 0, 64, 2},
    {"linear 64x4, limit 2", 1, 64, 2},
    {"linear 64x4, limit 4", 1, 64, 4},
    {"linear 64x4, no limit", 1, 64, 64},
    {"rehash 64x4, limit 4", 2, 64, 4},
    {"rehash 64x4, no limit", 2, 64, 64},
    {"linear 32x4 (128), no limit", 1, 32, 32},
    {"linear 128x4 (512), no limit", 1, 128, 128},
};
enum { ND = sizeof DS / sizeof DS[0] };

typedef struct { uint64_t stores, loads, sbuckets, lbuckets, lookups_l, fullpasses, passes, entries_at_full, busy; } dstat;
typedef struct {
    uint64_t passes, loads, stores, ld_true, ld_or, ld_bm[NBM], words[5], size[17];
    uint64_t ld_mem, ld_pst, ld_pst_bad, ld_pany, ld_pany_bad, touched;   // loads past this pass's own stores, against the last pass
    dstat d[ND];
} gstat;
static gstat G[2];   // 0 core 0, 1 workers

typedef struct {
    uint32_t orw, bm[NBM][32];
    uint32_t set[8192]; uint8_t smask[8192]; int nset;   // texels stored this pass (exact), open addressing
    uint32_t tag[ND][512]; int used[ND], full[ND];
    uint32_t any[16384], pst[16384], pany[16384];   // texels touched this pass; stored, touched last pass
    int active, stored;
} core_t;
static core_t C[MAXC];

static uint32_t h1(uint32_t t) { return t * 0x9e3779b1u; }
static uint32_t h2(uint32_t t) { return t * 0x85ebca6bu + 0x165667b1u; }
static int set_has(core_t *c, uint32_t t) {
    for (uint32_t i = h1(t) >> 19;; i = (i + 1) & 8191) { if (c->set[i] == NONE) return 0; if (c->set[i] == t) return 1; }
}
static void set_add(core_t *c, uint32_t t, int words) {
    if (c->nset > 6000) return;
    for (uint32_t i = h1(t) >> 19;; i = (i + 1) & 8191) {
        if (c->set[i] == t) { c->smask[i] |= words; return; }
        if (c->set[i] == NONE) { c->set[i] = t; c->smask[i] = words; c->nset++; return; }
    }
}
static int tset_has(const uint32_t *s, uint32_t t) { for (uint32_t i = h1(t) >> 18;; i = (i + 1) & 16383) { if (s[i] == NONE) return 0; if (s[i] == t) return 1; } }
static void tset_add(uint32_t *s, uint32_t t) { int n = 0; for (uint32_t i = h1(t) >> 18; n < 12000; i = (i + 1) & 16383, n++) { if (s[i] == t) return; if (s[i] == NONE) { s[i] = t; return; } } }
static int bm_bit(int m, int k, uint32_t t) { uint32_t h = k ? h2(t) : h1(t); return (int)(((uint64_t)h * BM_BITS[m]) >> 32); }
static int bm_has(core_t *c, int m, uint32_t t) {
    for (int k = 0; k < BM_K[m]; k++) { int b = bm_bit(m, k, t); if (!((c->bm[m][b >> 5] >> (b & 31)) & 1)) return 0; }
    return 1;
}
static void bm_add(core_t *c, int m, uint32_t t) { for (int k = 0; k < BM_K[m]; k++) { int b = bm_bit(m, k, t); c->bm[m][b >> 5] |= 1u << (b & 31); } }

// the bucket of probe p for texel t under design d
static int bucket_at(const design *d, uint32_t t, int p) {
    uint32_t b1 = (uint32_t)(((uint64_t)(h1(t) >> 16) * (d->kind == 0 ? d->nb / 2 : d->nb)) >> 16);
    if (d->kind == 0) return p == 0 ? (int)b1 : d->nb / 2 + (int)(((uint64_t)(h2(t) >> 16) * (d->nb / 2)) >> 16);
    if (d->kind == 1) return (int)((b1 + p) % d->nb);
    uint32_t step = ((h2(t) >> 16) * (uint32_t)d->nb >> 16) | 1u;   // odd: visits every bucket of a power of two
    return (int)((b1 + p * step) % d->nb);
}
// a lookup: buckets read; *slot the entry that holds t, else the first free one met (-1 none)
static int look(const design *d, core_t *c, int di, uint32_t t, int *found, int *slot) {
    uint32_t *tag = c->tag[di];
    *found = 0; *slot = -1;
    if (d->kind == 0) {   // both buckets read, a match first, else the first free way
        for (int p = 0; p < 2; p++) { int b = bucket_at(d, t, p); for (int w = 0; w < 4; w++) { uint32_t g = tag[4 * b + w]; if (g == t) { *found = 1; *slot = 4 * b + w; return 2; } } }
        for (int p = 0; p < 2; p++) { int b = bucket_at(d, t, p); for (int w = 0; w < 4; w++) if (tag[4 * b + w] == NONE) { *slot = 4 * b + w; return 2; } }
        return 2;
    }
    for (int p = 0; p < d->limit; p++) {   // bucket by bucket, stopping at the first empty way (entries are only added)
        int b = bucket_at(d, t, p);
        for (int w = 0; w < 4; w++) {
            uint32_t g = tag[4 * b + w];
            if (g == t) { *found = 1; *slot = 4 * b + w; return p + 1; }
            if (g == NONE) { *slot = 4 * b + w; return p + 1; }
        }
    }
    return d->limit;
}

// ---- C: texels two cores store in one pass
typedef struct { uint32_t texel; uint8_t core, words; uint32_t pc; } wrec;
#define WMAP (1 << 20)
static wrec *W; static uint32_t *Wused; static uint32_t nW;
static uint64_t cx_passes, cx_any, cx_same, cx_diff, cx_texels;
typedef struct { uint32_t pc1, pc2; uint64_t n; int same; } pcpair;
static pcpair PP[4096]; static int nPP;
static void pp_add(uint32_t a, uint32_t b, int same) {
    for (int i = 0; i < nPP; i++) if (PP[i].pc1 == a && PP[i].pc2 == b && PP[i].same == same) { PP[i].n++; return; }
    if (nPP < 4096) PP[nPP++] = (pcpair){a, b, 1, same};
}
static uint8_t *Wflag;   // per slot: 1 counted as a conflict already (diff), 2 (same)
static void cx_store(int core, uint32_t texel, int words, uint32_t pc) {
    uint32_t i = (texel * 0x9e3779b1u) >> 12;
    for (;; i = (i + 1) & (WMAP - 1)) {
        if (W[i].texel == NONE) { W[i] = (wrec){texel, (uint8_t)core, (uint8_t)words, pc}; Wused[nW++] = i; return; }
        if (W[i].texel == texel) break;
    }
    if (W[i].core == core) { W[i].words |= words; return; }
    int same = (W[i].words & words) != 0;
    if (!(Wflag[i] & (same ? 2 : 1))) {
        if (!Wflag[i]) cx_texels++;
        Wflag[i] |= same ? 2 : 1;
        if (same) cx_same++; else cx_diff++;
        pp_add(W[i].pc, pc, same);
    }
    W[i].words |= words;
}
static int cx_this_pass;
static uint32_t *PT; static uint8_t *PC; static uint32_t *PU, nPU;   // the last pass's stored texels and their core
static int last_writer(uint32_t t) { for (uint32_t i = (t * 0x9e3779b1u) >> 12;; i = (i + 1) & (WMAP - 1)) { if (PT[i] == NONE) return -1; if (PT[i] == t) return PC[i]; } }
static void cx_flush(void) {
    for (uint32_t k = 0; k < nPU; k++) PT[PU[k]] = NONE;
    nPU = 0;
    for (uint32_t k = 0; k < nW; k++) {
        wrec *e = &W[Wused[k]];
        for (uint32_t i = (e->texel * 0x9e3779b1u) >> 12;; i = (i + 1) & (WMAP - 1)) if (PT[i] == NONE) { PT[i] = e->texel; PC[i] = Wflag[Wused[k]] ? 255 : e->core; PU[nPU++] = i; break; }
        if (Wflag[Wused[k]]) cx_this_pass = 1;
        e->texel = NONE; Wflag[Wused[k]] = 0;
    }
    cx_any += cx_this_pass; cx_passes++; cx_this_pass = 0; nW = 0;
}

static void pass_begin(core_t *c) {
    c->orw = 0; memset(c->bm, 0, sizeof c->bm);
    memset(c->set, 0xff, sizeof c->set); c->nset = 0;
    memset(c->tag, 0xff, sizeof c->tag); memset(c->used, 0, sizeof c->used); memset(c->full, 0, sizeof c->full);
    c->active = 1; c->stored = 0;
}
static void pass_end(int ci) {
    core_t *c = &C[ci]; gstat *g = &G[ci != 0];
    if (!c->active) pass_begin(c);
    g->passes++;
    for (int i = 0; i < 8192; i++) if (c->set[i] != NONE) g->words[__builtin_popcount(c->smask[i])]++;
    for (int di = 0; di < ND; di++) { g->d[di].passes++; g->d[di].busy += c->stored != 0; if (c->full[di]) g->d[di].fullpasses++; }
    for (int i = 0; i < 16384; i++) g->touched += c->any[i] != NONE;
    memcpy(c->pany, c->any, sizeof c->any); memset(c->any, 0xff, sizeof c->any); memset(c->pst, 0xff, sizeof c->pst);
    for (int i = 0; i < 8192; i++) if (c->set[i] != NONE) tset_add(c->pst, c->set[i]);
    c->active = 0;
}

static void access(int ci, uint32_t a, uint32_t bytes, int store, uint32_t pc) {
    core_t *c = &C[ci]; gstat *g = &G[ci != 0];
    if (!c->active) pass_begin(c);
    if (bytes == 0) bytes = 4;
    if (store) g->size[bytes > 16 ? 16 : bytes]++;
    uint32_t first = a >> 4, last = (a + bytes - 1) >> 4;
    for (uint32_t t = first; t <= last; t++) {
        if (store) {
            g->stores++; c->stored++;
            uint32_t lo = t == first ? (a >> 2) & 3 : 0, hi = t == last ? ((a + bytes - 1) >> 2) & 3 : 3;
            int words = ((1 << (hi + 1)) - 1) & ~((1 << lo) - 1);
            cx_store(ci, t, words, pc);
            c->orw |= t; set_add(c, t, words); tset_add(c->any, t);
            for (int m = 0; m < NBM; m++) bm_add(c, m, t);
            for (int di = 0; di < ND; di++) {
                const design *d = &DS[di]; int found, slot;
                g->d[di].stores++;
                g->d[di].sbuckets += look(d, c, di, t, &found, &slot);
                if (found) continue;
                if (slot < 0) { if (!c->full[di]) { c->full[di] = 1; g->d[di].entries_at_full += c->used[di]; } continue; }
                c->tag[di][slot] = t; c->used[di]++;
            }
        } else {
            g->loads++;
            int truly = set_has(c, t);
            g->ld_true += truly;
            tset_add(c->any, t);
            if (!truly) {
                int lw = last_writer(t), bad = lw >= 0 && lw != ci;
                g->ld_mem++;
                if (tset_has(c->pst, t)) { g->ld_pst++; g->ld_pst_bad += bad; }
                if (tset_has(c->pany, t)) { g->ld_pany++; g->ld_pany_bad += bad; }
            }
            g->ld_or += (t & c->orw) == t;
            for (int m = 0; m < NBM; m++) g->ld_bm[m] += bm_has(c, m, t);
            if (bm_has(c, 1, t)) for (int di = 0; di < ND; di++) {   // the lookups a 256-bit bitmap lets through
                int found, slot;
                g->d[di].lookups_l++;
                g->d[di].lbuckets += look(&DS[di], c, di, t, &found, &slot);
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "wcstudy LOG WORDS(5|7) [user-only 0|1]\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    int nw = atoi(argv[2]), user_only = argc > 3 && atoi(argv[3]);
    if (!f) { perror(argv[1]); return 1; }
    W = malloc(sizeof(wrec) * WMAP); Wused = malloc(4 * WMAP); Wflag = calloc(WMAP, 1);
    for (int i = 0; i < WMAP; i++) W[i].texel = NONE;
    PT = malloc(4 * WMAP); PC = malloc(WMAP); PU = malloc(4 * WMAP);
    for (int i = 0; i < WMAP; i++) PT[i] = NONE;
    for (int i = 0; i < MAXC; i++) { memset(C[i].any, 0xff, sizeof C[i].any); memset(C[i].pany, 0xff, sizeof C[i].pany); memset(C[i].pst, 0xff, sizeof C[i].pst); }
    uint32_t r[7]; int last_end = -1; uint64_t recs = 0;
    while (fread(r, 4, nw, f) == (size_t)nw) {
        recs++;
        int core = (int)((r[3] >> 25) & 127);
        if (core <= last_end) { cx_flush(); last_end = -1; }
        if (r[0] == NONE) { pass_end(core); last_end = core; continue; }
        int load = (r[3] >> 24) & 1, sup = (r[3] >> 16) & 1;
        if (user_only && sup) continue;
        access(core, r[0], r[3] & 255, !load, r[4]);
    }
    cx_flush();
    printf("log %s: %llu records, %llu passes (global)\n", argv[1], (unsigned long long)recs, (unsigned long long)cx_passes);
    for (int k = 0; k < 2; k++) {
        gstat *g = &G[k];
        if (!g->passes) continue;
        printf("\n== %s: %llu passes that store, %llu core-passes, %llu loads, %llu stores (texel accesses)\n", k ? "workers" : "core 0", (unsigned long long)g->d[0].busy,
               (unsigned long long)g->passes, (unsigned long long)g->loads, (unsigned long long)g->stores);
        double L = g->loads ? (double)g->loads : 1;
        uint64_t nt = g->words[1] + g->words[2] + g->words[3] + g->words[4], ns = 0;
        for (int i = 0; i <= 16; i++) ns += g->size[i];
        if (nt) printf("D. texels stored in a pass, by words stored: 1 %.1f%% | 2 %.1f%% | 3 %.1f%% | 4 %.1f%% (mean %.2f words, %llu texels)\n",
                       100.0 * g->words[1] / nt, 100.0 * g->words[2] / nt, 100.0 * g->words[3] / nt, 100.0 * g->words[4] / nt,
                       (g->words[1] + 2.0 * g->words[2] + 3.0 * g->words[3] + 4.0 * g->words[4]) / nt, (unsigned long long)nt);
        if (ns) printf("   stores by size: 1 byte %.1f%% | 2 %.1f%% | 4 %.1f%% | 8 %.1f%% | 16 %.1f%%\n", 100.0 * g->size[1] / ns,
                       100.0 * g->size[2] / ns, 100.0 * g->size[4] / ns, 100.0 * g->size[8] / ns, 100.0 * g->size[16] / ns);
        if (g->ld_mem) printf("E. loads not of this pass's own stores: %.1f%% of loads. Of them, texels this core stored last pass %.1f%% "
               "(another core stored it last pass: %.1f%%), touched last pass %.1f%% (another core stored it: %.1f%%)\n", 100.0 * g->ld_mem / L,
               100.0 * g->ld_pst / g->ld_mem, 100.0 * g->ld_pst_bad / g->ld_mem, 100.0 * g->ld_pany / g->ld_mem, 100.0 * g->ld_pany_bad / g->ld_mem);
        printf("   distinct texels a core touches in a pass that stores: %.0f\n", g->d[0].busy ? (double)g->touched / g->d[0].busy : 0);
        printf("A. loads that look up, %% of loads: exact %.1f | OR word %.1f", 100 * g->ld_true / L, 100 * g->ld_or / L);
        for (int m = 0; m < NBM; m++) printf(" | %d bits%s %.1f", BM_BITS[m], BM_K[m] > 1 ? " k2" : "", 100 * g->ld_bm[m] / L);
        printf("\nB. design | buckets read a store | buckets read a load (256-bit filter passed) | passes ending full %% | entries when full\n");
        for (int di = 0; di < ND; di++) {
            dstat *d = &g->d[di];
            printf("   %-30s %5.2f  %5.2f  %6.2f  %6.1f\n", DS[di].name, d->stores ? (double)d->sbuckets / d->stores : 0,
                   d->lookups_l ? (double)d->lbuckets / d->lookups_l : 0, d->busy ? 100.0 * d->fullpasses / d->busy : 0,
                   d->fullpasses ? (double)d->entries_at_full / d->fullpasses : 0);
        }
    }
    printf("\nC. texels stored by two cores in one pass: %llu texels (%llu in the same word, %llu in different words only "
           "or as well), in %llu of %llu passes\n", (unsigned long long)cx_texels, (unsigned long long)cx_same,
           (unsigned long long)cx_diff, (unsigned long long)cx_any, (unsigned long long)cx_passes);
    for (int i = 0; i < nPP; i++) for (int j = i + 1; j < nPP; j++) if (PP[j].n > PP[i].n) { pcpair x = PP[i]; PP[i] = PP[j]; PP[j] = x; }
    for (int i = 0; i < nPP && i < 15; i++)
        printf("   %8llu  %s  first %08x  then %08x\n", (unsigned long long)PP[i].n, PP[i].same ? "same word" : "diff words", PP[i].pc1, PP[i].pc2);
    return 0;
}
