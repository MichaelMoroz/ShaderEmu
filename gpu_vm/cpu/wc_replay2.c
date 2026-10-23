/* The write cache's designs on gvm_cpu's stores (GVM_STLOG, GVM_STLOG_CORE=-1, GVM_STLOG_VALUES=1), a generalisation of
 * wc_replay.c for IDEAS 42 (the builder's evf):
 *
 *   wc_replay2 STLOG FRAMES DESIGN...      DESIGN = "R:n1xw1,n2xw2[,n3xw3]:ov:sil:pol"
 *
 * R 0 every table's bucket read (v0009), 1 a cascade (the next table read only when a bucket is full, RISC-V's);
 * tables of n buckets of w ways, multiplicative hashes of any count; ov overflow entries; sil 0 none, 1 a silent store to a
 * texel with no entry makes none, 3 only when it finds no room, 4 at a full end the candidate buckets' entries that hold
 * RAM's bytes are dropped (the lookup then takes a match before a free way), 5 every such entry is dropped; pol 1 a new
 * texel to the less full of two buckets. Per design: full ends a frame (all, core 0, workers), entries at a full end, tags
 * read a store, and for sil 4 and 5 the scans, texels fetched and entries freed a frame.
 */
/* wcr2 STLOG FRAMES design...   a design: "T:n1xw1,n2xw2[,n3xw3]:ov:sil:pol"
 * T 0 = both tables' buckets read (v0009), 1 = cascade (read on only when the bucket is full)
 * sil 0 none, 1 every silent store to a texel with no entry makes none, 3 only when the store finds no room
 * pol 0 first free in table order, 1 less full bucket (two tables) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAXE 1100
#define MAXD 24
typedef struct { int casc, nt, nb[3], w[3], base[3], ov, sil, pol, total; char name[64]; } design;
static design D[MAXD]; static int nd;
typedef struct { uint32_t tag[MAXE]; int used; uint8_t orig[MAXE][16], cur[MAXE][16]; uint16_t known[MAXE]; uint8_t dirty[MAXE]; } wc;
static int equal(wc *c, int e) { if (c->dirty[e]) return 0; for (int i = 0; i < 16; i++) if ((c->known[e] >> i & 1) && c->orig[e][i] != c->cur[e][i]) return 0; return 1; }
static void drop(wc *c, int e) { c->tag[e] = 0; c->used--; c->known[e] = 0; c->dirty[e] = 0; }
typedef struct { uint64_t stores, fulls, reads, used_at_full, silent_skip; uint64_t c0fulls, wfulls, scans, fetched, freed; } stat;
static stat S[MAXD];
static wc *C[128][MAXD];
static uint32_t start[128][MAXD];
static uint32_t H[3][2] = {{0x9e3779b1u, 0}, {0x85ebca6bu, 0x165667b1u}, {0xc2b2ae35u, 0x27d4eb2fu}};
static int bk(const design *d, int k, uint32_t t) {
    uint32_t h = t * H[k][0] + H[k][1];
    return d->base[k] + (int)(((uint64_t)(h >> 8) * d->nb[k]) >> 24) * d->w[k];
}
static int find(const design *d, wc *c, uint32_t t, int *rd) {
    for (int k = 0; k < d->nt; k++) {
        int b = bk(d, k, t), full = 1, any = 0;
        if (d->casc) { for (int i = 0; i < d->w[k]; i++) any |= c->tag[b + i] != 0; if (!any) return -1; }
        (*rd)++;
        for (int i = 0; i < d->w[k]; i++) { if (c->tag[b + i] == t) return b + i; full &= c->tag[b + i] != 0; }
        if (d->casc && !full && d->sil < 4) return -1;
    }
    if (d->ov) { (*rd)++; for (int i = 0; i < d->ov; i++) if (c->tag[d->total - d->ov + i] == t) return d->total - d->ov + i; }
    return -1;
}
static int put(const design *d, wc *c, uint32_t t) {
    if (d->pol == 1 && d->nt == 2) {
        int b0 = bk(d, 0, t), b1 = bk(d, 1, t), u0 = 0, u1 = 0;
        for (int i = 0; i < d->w[0]; i++) u0 += c->tag[b0 + i] != 0;
        for (int i = 0; i < d->w[1]; i++) u1 += c->tag[b1 + i] != 0;
        int b = u1 < u0 ? b1 : b0, w = u1 < u0 ? d->w[1] : d->w[0];
        for (int i = 0; i < w; i++) if (!c->tag[b + i]) { c->tag[b + i] = t; c->used++; return b + i; }
    } else
        for (int k = 0; k < d->nt; k++) {
            int b = bk(d, k, t);
            for (int i = 0; i < d->w[k]; i++) if (!c->tag[b + i]) { c->tag[b + i] = t; c->used++; return b + i; }
        }
    for (int i = 0; i < d->ov; i++) if (!c->tag[d->total - d->ov + i]) { c->tag[d->total - d->ov + i] = t; c->used++; return d->total - d->ov + i; }
    return -1;
}
int main(int argc, char **argv) {
    double frames = atof(argv[2]);
    for (int a = 3; a < argc; a++) {
        design *d = &D[nd++]; memset(d, 0, sizeof *d);
        snprintf(d->name, 64, "%s", argv[a]);
        char *p = argv[a]; d->casc = atoi(p); p = strchr(p, ':') + 1;
        while (1) { d->nb[d->nt] = strtol(p, &p, 10); p++; d->w[d->nt] = strtol(p, &p, 10); d->nt++; if (*p == ',') p++; else break; }
        p++; d->ov = strtol(p, &p, 10); p++; d->sil = strtol(p, &p, 10); p++; d->pol = strtol(p, &p, 10);
        int tot = 0; for (int k = 0; k < d->nt; k++) d->base[k] = tot, tot += d->nb[k] * d->w[k];
        d->total = tot + d->ov;
        if (d->total > MAXE) { fprintf(stderr, "too big\n"); return 1; }
    }
    FILE *f = fopen(argv[1], "rb");
    uint32_t r[7];
    while (fread(r, 4, 7, f) == 7) {
        uint32_t ad = r[0], bun = r[2], info = r[3], nv = r[5], ov = r[6];
        int cid = (int)(info >> 25) & 127;
        if (!C[cid][0]) for (int d = 0; d < nd; d++) C[cid][d] = calloc(1, sizeof(wc)), start[cid][d] = bun;
        if (ad == 0xffffffffu) { for (int d = 0; d < nd; d++) memset(C[cid][d]->tag, 0, sizeof C[cid][d]->tag), C[cid][d]->used = 0, start[cid][d] = bun; continue; }
        int load = (info >> 24) & 1, bytes = info & 255;
        if (load) continue;
        int two = (ad & 15) + bytes > 16, same = bytes <= 4 && nv == ov;
        for (int di = 0; di < nd; di++) {
            design *d = &D[di]; wc *c = C[cid][di]; stat *s = &S[di];
            s->stores++;
            for (int h = 0; h <= two; h++) {
                uint32_t t = ((h ? ad + 16 : ad) >> 4) + 1;
                int rd = 0, e = find(d, c, t, &rd);
                s->reads += rd;
                if (e >= 0) goto rec;
                if (d->sil == 1 && same) { s->silent_skip++; continue; }
                e = put(d, c, t);
                if (e < 0 && d->sil == 3 && same) { s->silent_skip++; continue; }
                if (e < 0 && d->sil == 4) {   /* the candidate buckets' entries that hold RAM's bytes are dropped */
                    s->scans++;
                    for (int k = 0; k < d->nt; k++) { int b = bk(d, k, t); for (int i = 0; i < d->w[k]; i++) { s->fetched++; if (c->tag[b + i] && equal(c, b + i)) drop(c, b + i), s->freed++; } }
                    e = put(d, c, t);
                }
                if (e < 0 && d->sil == 5) {   /* every entry that holds RAM's bytes is dropped */
                    s->scans++;
                    for (int x = 0; x < d->total; x++) if (c->tag[x]) { s->fetched++; if (equal(c, x)) drop(c, x), s->freed++; }
                    e = put(d, c, t);
                }
                if (e >= 0) { c->known[e] = 0; c->dirty[e] = 0; }
                if (e < 0) {
                    s->fulls++; s->used_at_full += c->used; if (cid) s->wfulls++; else s->c0fulls++;
                    memset(c->tag, 0, sizeof c->tag); c->used = 0; start[cid][di] = bun; e = put(d, c, t); c->known[e] = 0; c->dirty[e] = 0;
                }
              rec:
                if (bytes > 4) c->dirty[e] = 1;
                else for (int i = 0; i < bytes; i++) {
                    uint32_t a = ad + i; if ((a >> 4) + 1 != t) continue;
                    int pos = a & 15; uint8_t o = ov >> (8 * i), n = nv >> (8 * i);
                    if (!(c->known[e] >> pos & 1)) c->orig[e][pos] = o, c->known[e] |= 1 << pos;
                    c->cur[e][pos] = n;
                }
            }
        }
    }
    printf("%-26s %5s %6s %8s %7s %7s %7s %7s %8s %8s %8s\n", "design", "entr", "texels", "fulls/fr", "c0/fr", "wk/fr", "atfull", "tags/st", "scans/fr", "fetch/fr", "freed/fr");
    for (int di = 0; di < nd; di++) {
        design *d = &D[di]; stat *s = &S[di];
        int tex = 40 + d->total + 2 + (d->total + 2 + 3) / 4;
        printf("%-26s %5d %6d %8.2f %7.2f %7.2f %7.1f %7.2f %8.1f %8.0f %8.0f\n", d->name, d->total, tex, s->fulls / frames, s->c0fulls / frames, s->wfulls / frames,
               s->fulls ? (double)s->used_at_full / s->fulls : 0, (double)s->reads / s->stores, s->scans / frames, s->fetched / frames, s->freed / frames);
    }
    return 0;
}
