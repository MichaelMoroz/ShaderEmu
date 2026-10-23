/* Every core's stores (and loads) from gvm_cpu's GVM_STLOG (GVM_STLOG_CORE=-1, GVM_STLOG_VALUES=1, GVM_STLOG_LOADS=1
 * for the loads' columns) replayed through several write caches, by job (gpu_vm/PROFILE_Q3.md, "RISC-V's cache on
 * gpu_vm's stores"):
 *
 *   wc_replay STLOG JOBS.tsv [FRAMES]      (FRAMES: the guest frames the log covers, for the "a frame" columns)
 *
 * JOBS.tsv: lines "lo hi name" (hex): a store whose sp is in [lo, hi) is that job's; "kernel" for supervisor stores, and
 * lines "pc lo hi name" put the pcs in [lo, hi) to a job whatever the stack (the firmware's loops). Otherwise core 0's
 * stores are "core 0", a worker's "worker".
 *
 * The designs: v0009 (two tables of 16 buckets of four, multiplicative hashes, two overflow entries; a store makes an
 * entry whatever it writes), v0009 with silent stores making none (the builder's sil), RISC-V's (three tables of 2^b,
 * 2^(b-1), 2^(b-2) buckets of three, experiments/rvc_opt/src/mem.h's hashes, a store that changes nothing makes no entry;
 * b 6 on core 0 and 5 on workers as Quake III runs: 336 and 168 entries), and RISC-V's at b 5 and b 4 on every core.
 * A cache starts empty at each of gvm_cpu's pass ends and when a store finds no room (the card's pass would end there).
 * Per design and job: stores, silent stores, full ends, bundles from a cache's start to its full end (median, p10, p90),
 * entries at a full end, distinct texels and stores per texel over a cache's life, loads that hit an entry, tags read a
 * store and a load (each bucket read is a texel of tags), and at the full ends the fullest bucket against the mean.
 * Lines "csv,..." repeat the table for the report's CSV.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NJ 16
#define ND 5
static char jname[NJ][32];
static int nj;
static struct { uint32_t lo, hi; int j, pc; } rng[4096];
static int nr;

static int job_of(int core, uint32_t sp, uint32_t pc, int sup) {
    for (int k = 0; k < nr; k++)
        if (rng[k].pc && pc >= rng[k].lo && pc < rng[k].hi) return rng[k].j;
    if (sup) return 0;
    for (int k = 0; k < nr; k++)
        if (!rng[k].pc && sp >= rng[k].lo && sp < rng[k].hi) return rng[k].j;
    return core == 0 ? 1 : 2;
}
static int jadd(const char *n) {
    for (int j = 0; j < nj; j++)
        if (!strcmp(jname[j], n)) return j;
    snprintf(jname[nj], 32, "%s", n);
    return nj++;
}

/* one cache: tags (texel + 1, 0 free) of nb buckets of w ways, plus ov overflow entries */
typedef struct {
    int kind, bits, w, nb, ov, silent;   /* kind 0: v0009, 1: RISC-V */
    uint32_t tag[512];
    int used;
    uint32_t bloom;
} wc;

static uint32_t rv_t(uint32_t ad) { return (ad - 0x02000000u) >> 4; }
static void buckets(const wc *c, uint32_t ad, int *b, int *n) {
    uint32_t t = ad >> 4;
    if (c->kind == 0) {
        b[0] = (int)((t * 0x9e3779b1u) >> 28);
        b[1] = 16 + (int)((t * 0x85ebca6bu + 0x165667b1u) >> 28);
        *n = 2;
    } else {
        uint32_t r = rv_t(ad), T = 1u << c->bits, m = c->bits - 1;
        b[0] = (int)(r & (T - 1));
        b[1] = (int)(T + ((((r >> 3) ^ (r << (m - 3)) ^ (r >> m))) & ((1u << m) - 1)));
        b[2] = (int)(T + (T >> 1) + ((((r >> 2) ^ (r << (m - 2)) ^ (r >> (m + 3))) & ((1u << m) - 1)) & ((T >> 2) - 1)));
        *n = 3;
    }
}
static void wc_init(wc *c, int kind, int bits, int silent) {
    memset(c, 0, sizeof *c);
    c->kind = kind, c->bits = bits, c->silent = silent;
    if (kind == 0) c->w = 4, c->nb = 32, c->ov = 2;
    else c->w = 3, c->nb = (7 << bits) >> 2, c->ov = 0;
}
static void wc_clear(wc *c) { memset(c->tag, 0, sizeof c->tag); c->used = 0; c->bloom = 0; }
/* where texel t is (entry index) or -1; *reads: bucket texels of tags read */
static int wc_find(const wc *c, uint32_t ad, int *reads) {
    int b[3], n, k;
    uint32_t t = (ad >> 4) + 1;
    buckets(c, ad, b, &n);
    if (c->kind == 0) {   /* both buckets' tags in two texels, the overflow entries when both are full */
        *reads += 2;
        for (k = 0; k < 2; k++)
            for (int i = 0; i < 4; i++)
                if (c->tag[b[k] * 4 + i] == t) return b[k] * 4 + i;
        int full = 1;
        for (k = 0; k < 2; k++)
            for (int i = 0; i < 4; i++) full &= c->tag[b[k] * 4 + i] != 0;
        if (full) {
            *reads += 1;
            for (int i = 0; i < c->ov; i++)
                if (c->tag[128 + i] == t) return 128 + i;
        }
        return -1;
    }
    for (k = 0; k < n; k++) {   /* a table's bucket is read only when the one before is full (and not empty: L1_OCC) */
        int any = 0, full = 1;
        for (int i = 0; i < 3; i++) any |= c->tag[b[k] * 3 + i] != 0, full &= c->tag[b[k] * 3 + i] != 0;
        if (!any) return -1;
        *reads += 1;
        for (int i = 0; i < 3; i++)
            if (c->tag[b[k] * 3 + i] == t) return b[k] * 3 + i;
        if (!full) return -1;
    }
    return -1;
}
/* a new entry for texel t: its index, or -1 (full) */
static int wc_put(wc *c, uint32_t ad) {
    int b[3], n;
    uint32_t t = (ad >> 4) + 1;
    buckets(c, ad, b, &n);
    for (int k = 0; k < n; k++)
        for (int i = 0; i < c->w; i++)
            if (!c->tag[b[k] * c->w + i]) { c->tag[b[k] * c->w + i] = t; c->used++; c->bloom |= ad >> 4; return b[k] * c->w + i; }
    for (int i = 0; i < c->ov; i++)
        if (!c->tag[128 + i]) { c->tag[128 + i] = t; c->used++; c->bloom |= ad >> 4; return 128 + i; }
    return -1;
}

typedef struct {
    uint64_t stores, silent, loads, lhit, lskip, sreads, lreads, fulls, segs, seg_st, seg_tex, full_used, full_max, full_mean100;
    uint32_t *bb; size_t nbb, cap;   /* bundles before full, one a full end */
} stat;
static stat S[ND][NJ];
static void push_bb(stat *s, uint32_t v) {
    if (s->nbb == s->cap) { s->cap = s->cap ? 2 * s->cap : 1024; s->bb = (uint32_t *)realloc(s->bb, 4 * s->cap); }
    s->bb[s->nbb++] = v;
}
static int cmpu(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }

/* a core's state for each design */
typedef struct {
    wc c[ND];
    uint32_t start[ND];      /* bundles at the cache's start */
    int jobs[ND][NJ];        /* stores by job since the start (the full end is the most storing job's) */
    uint32_t seen;           /* any record yet */
    uint32_t tex[ND][512];   /* stores into each entry (reuse) */
} core;
static core *C[128];

static int major(const int *j) { int m = 0; for (int k = 1; k < NJ; k++) if (j[k] > j[m]) m = k; return m; }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: wc_replay STLOG JOBS.tsv [FRAMES]\n"); return 1; }
    double frames = argc > 3 ? atof(argv[3]) : 0;
    jadd("kernel"); jadd("core 0"); jadd("worker");
    FILE *jf = fopen(argv[2], "r");
    char line[256];
    while (jf && fgets(line, sizeof line, jf)) {
        char a[32], b[32], c[32], d[64];
        int n = sscanf(line, "%31s %31s %31s %63s", a, b, c, d);
        if (n == 3) rng[nr].lo = (uint32_t)strtoul(a, 0, 16), rng[nr].hi = (uint32_t)strtoul(b, 0, 16), rng[nr].j = jadd(c), rng[nr].pc = 0, nr++;
        else if (n == 4 && !strcmp(a, "pc")) rng[nr].lo = (uint32_t)strtoul(b, 0, 16), rng[nr].hi = (uint32_t)strtoul(c, 0, 16), rng[nr].j = jadd(d), rng[nr].pc = 1, nr++;
    }
    static const char *dn[ND] = {"v0009 (130)", "v0009 + silent", "RISC-V (336 / 168)", "RISC-V b5 (168)", "RISC-V b4 (84)"};
    static const int kind[ND] = {0, 0, 1, 1, 1}, sil[ND] = {0, 1, 1, 1, 1}, bits0[ND] = {0, 0, 6, 5, 4}, bitsw[ND] = {0, 0, 5, 5, 4};
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    uint32_t r[7];
    uint64_t nrec = 0;
    while (fread(r, 4, 7, f) == 7) {
        nrec++;
        uint32_t ad = r[0], sp = r[1], bun = r[2], info = r[3], pc = r[4], nv = r[5], ov = r[6];
        int cid = (int)(info >> 25) & 127;
        if (!C[cid]) {
            C[cid] = (core *)calloc(1, sizeof(core));
            for (int d = 0; d < ND; d++) wc_init(&C[cid]->c[d], kind[d], cid ? bitsw[d] : bits0[d], sil[d]), C[cid]->start[d] = bun;
        }
        core *k = C[cid];
        if (ad == 0xffffffffu) {   /* gvm_cpu's pass end: every cache starts again */
            for (int d = 0; d < ND; d++) {
                int j = major(k->jobs[d]);
                if (k->c[d].used) {
                    S[d][j].segs++; S[d][j].seg_tex += k->c[d].used;
                    for (int e = 0; e < 512; e++) S[d][j].seg_st += k->tex[d][e];
                }
                wc_clear(&k->c[d]); memset(k->jobs[d], 0, sizeof k->jobs[d]); memset(k->tex[d], 0, sizeof k->tex[d]);
                k->start[d] = bun;
            }
            continue;
        }
        int load = (info >> 24) & 1, bytes = info & 255, sup = (info >> 16) & 1;
        int j = job_of(cid, sp, pc, sup);
        int two = (ad & 15) + (load ? 4 : bytes) > 16;
        for (int d = 0; d < ND; d++) {
            wc *c = &k->c[d];
            stat *s = &S[d][j];
            if (load) {
                s->loads++;
                if (((ad >> 4) & c->bloom) != (ad >> 4)) { s->lskip++; continue; }   /* the bloom word rules it out */
                int rd = 0, e = wc_find(c, ad, &rd);
                s->lreads += rd;
                if (e >= 0) s->lhit++;
                continue;
            }
            s->stores++;
            int same = bytes <= 4 && nv == ov;
            if (same) s->silent++;
            for (int h = 0; h <= two; h++) {
                uint32_t a = h ? ad + 16 : ad;
                int rd = 0, e = wc_find(c, a, &rd);
                s->sreads += rd;
                if (e < 0 && c->silent && same) continue;
                if (e < 0) e = wc_put(c, a);
                if (e < 0) {   /* full: the pass would end here; what was in it, and a new cache */
                    int m = major(k->jobs[d]);
                    stat *ms = &S[d][m];
                    ms->fulls++; push_bb(ms, bun - k->start[d]);
                    ms->full_used += c->used;
                    int most = 0, tot = 0, nb = c->nb;
                    for (int b = 0; b < nb; b++) {
                        int u = 0;
                        for (int i = 0; i < c->w; i++) u += c->tag[b * c->w + i] != 0;
                        most = u > most ? u : most; tot += u;
                    }
                    ms->full_max += most; ms->full_mean100 += (uint64_t)(100.0 * tot / nb);
                    ms->segs++; ms->seg_tex += c->used;
                    for (int x = 0; x < 512; x++) ms->seg_st += k->tex[d][x];
                    wc_clear(c); memset(k->jobs[d], 0, sizeof k->jobs[d]); memset(k->tex[d], 0, sizeof k->tex[d]);
                    k->start[d] = bun;
                    e = wc_put(c, a);
                }
                k->tex[d][e]++;
            }
            k->jobs[d][j]++;
        }
    }
    fprintf(stderr, "%llu records\n", (unsigned long long)nrec);
    for (int d = 0; d < ND; d++) {
        printf("\n%s\n%-12s %10s %7s %8s %9s %22s %8s %8s %9s %8s %8s %9s %9s\n", dn[d], "job", "stores", "silent", "fulls", "a frame",
               "bundles to full p10/50/90", "entries", "texels", "st/texel", "loads", "hit", "tags/st", "tags/ld", "most/mean");
        for (int j = 0; j < nj; j++) {
            stat *s = &S[d][j];
            if (!s->stores && !s->loads) continue;
            uint32_t p10 = 0, p50 = 0, p90 = 0;
            if (s->nbb) { qsort(s->bb, s->nbb, 4, cmpu); p10 = s->bb[s->nbb / 10]; p50 = s->bb[s->nbb / 2]; p90 = s->bb[s->nbb * 9 / 10]; }
            double fr = frames > 0 ? s->fulls / frames : 0;
            double ent = s->fulls ? (double)s->full_used / s->fulls : 0, tex = s->segs ? (double)s->seg_tex / s->segs : 0;
            double reuse = s->seg_tex ? (double)s->seg_st / s->seg_tex : 0;
            double mm = s->fulls ? (double)s->full_max / s->fulls : 0, mean = s->fulls ? s->full_mean100 / 100.0 / s->fulls : 0;
            double stl = s->loads - s->lskip ? (double)s->lreads / s->loads : 0;
            printf("%-12s %10llu %6.1f%% %8llu %9.2f %8u/%6u/%6u %8.1f %8.1f %9.2f %8llu %7.1f%% %9.2f %9.2f %4.1f/%4.2f\n", jname[j],
                   (unsigned long long)s->stores, s->stores ? 100.0 * s->silent / s->stores : 0, (unsigned long long)s->fulls, fr, p10, p50, p90,
                   ent, tex, reuse, (unsigned long long)s->loads, s->loads ? 100.0 * s->lhit / s->loads : 0,
                   s->stores ? (double)s->sreads / s->stores : 0, stl, mm, mean);
            fprintf(stderr, "csv,%s,%s,%llu,%llu,%llu,%.3f,%u,%u,%u,%.2f,%.2f,%.3f,%llu,%llu,%llu,%.3f,%.3f,%.2f,%.3f\n", dn[d], jname[j],
                    (unsigned long long)s->stores, (unsigned long long)s->silent, (unsigned long long)s->fulls, fr, p10, p50, p90, ent, tex, reuse,
                    (unsigned long long)s->loads, (unsigned long long)s->lhit, (unsigned long long)s->lskip,
                    s->stores ? (double)s->sreads / s->stores : 0, stl, mm, mean);
        }
    }
    return 0;
}
